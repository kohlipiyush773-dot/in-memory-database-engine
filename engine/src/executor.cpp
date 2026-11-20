#include "database.h"
#include <chrono>

static const long long INF = 1LL << 40;

// ============================================================ helpers
static int CmpVal(const Value& a, const Value& b) {
    if (a.type == COL_INT) return a.i < b.i ? -1 : (a.i > b.i ? 1 : 0);
    return a.s.compare(b.s) < 0 ? -1 : (a.s == b.s ? 0 : 1);
}
static bool Test(CmpOp op, int c) {
    switch (op) { case OP_EQ: return c == 0; case OP_NE: return c != 0; case OP_LT: return c < 0;
                  case OP_LE: return c <= 0; case OP_GT: return c > 0; default: return c >= 0; }
}
static bool Matches(const std::vector<Cond>& cs, const Row& r) {
    for (size_t i = 0; i < cs.size(); ++i) if (!Test(cs[i].op, CmpVal(r[cs[i].col], cs[i].v))) return false;
    return true;
}
static std::string ValText(const Value& v) { return v.type == COL_INT ? Str(v.i) : "'" + v.s + "'"; }

IoStats Database::Snapshot() const {
    IoStats s; const BpmStats& b = bpm_->stats();
    s.hits = b.hits; s.misses = b.misses; s.evictions = b.evictions; s.dirty_evictions = b.dirty_evictions;
    s.flushes = b.flushes; s.wal_forces = b.wal_forces; s.new_pages = b.new_pages;
    s.disk_reads = disk_->Reads(); s.disk_writes = disk_->Writes();
    s.wal_bytes = (long)log_->EndOffset(); s.wal_records = log_->Records();
    return s;
}

// ============================================================ entry points
QueryResult Database::RunWrapped(const std::function<void(QueryResult&)>& body) {
    QueryResult r;
    bool tr; bpm_->DrainEvents(&tr);
    IoStats a = Snapshot();
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    try { body(r); }
    catch (const std::exception& e) { r.ok = false; r.error = e.what(); r.rows.clear(); r.columns.clear(); }
    r.elapsed_us = (long)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
    IoStats b = Snapshot();
    r.io.hits = b.hits - a.hits; r.io.misses = b.misses - a.misses; r.io.evictions = b.evictions - a.evictions;
    r.io.dirty_evictions = b.dirty_evictions - a.dirty_evictions; r.io.flushes = b.flushes - a.flushes;
    r.io.wal_forces = b.wal_forces - a.wal_forces; r.io.new_pages = b.new_pages - a.new_pages;
    r.io.disk_reads = b.disk_reads - a.disk_reads; r.io.disk_writes = b.disk_writes - a.disk_writes;
    r.io.wal_bytes = b.wal_bytes - a.wal_bytes; r.io.wal_records = b.wal_records - a.wal_records;
    r.row_count = (long)r.rows.size();
    if (r.rows.size() > 500) { r.truncated = true; r.rows.resize(500); }
    r.events = bpm_->DrainEvents(&r.events_truncated);
    return r;
}

QueryResult Database::Execute(const std::string& sql) {
    return RunWrapped([&](QueryResult& r) { Stmt st = ParseSql(sql); Dispatch(st, r); });
}

void Database::Dispatch(const Stmt& st, QueryResult& r) {
    switch (st.type) {
        case ST_CREATE: DoCreate(st, r); break;
        case ST_DROP: DoDrop(st, r); break;
        case ST_INSERT: DoInsert(st, r); break;
        case ST_SELECT: DoSelect(st, r); break;
        case ST_UPDATE: DoUpdate(st, r); break;
        case ST_DELETE: DoDelete(st, r); break;
        case ST_SHOW_TABLES: DoShowTables(r); break;
        case ST_DESCRIBE: DoDescribe(st, r); break;
        case ST_BEGIN:
            if (txn_active_) throw std::runtime_error("a transaction is already active");
            StartTxn(true); r.kind = "BEGIN"; r.message = "transaction " + Str(txn_id_) + " started"; break;
        case ST_COMMIT:
            if (!txn_active_) throw std::runtime_error("no active transaction");
            { bool lg = txn_logged_; CommitTxn(); r.kind = "COMMIT"; r.message = lg ? "committed (log forced to disk)" : "committed (nothing was changed)"; }
            break;
        case ST_ROLLBACK:
            if (!txn_active_) throw std::runtime_error("no active transaction");
            { bool lg = txn_logged_; RollbackTxn(); r.kind = "ROLLBACK"; r.message = lg ? "rolled back: all changes undone using the log" : "rolled back (nothing was changed)"; }
            break;
        case ST_CHECKPOINT:
            Checkpoint(); r.kind = "CHECKPOINT"; r.message = "all dirty pages written to disk, checkpoint record logged"; break;
    }
}

TableEntry& Database::NeedTable(const std::string& name) {
    TableEntry* t = FindTable(name);
    if (!t) throw std::runtime_error("no such table: " + name);
    return *t;
}

// ============================================================ DDL
void Database::DoCreate(const Stmt& st, QueryResult& r) {
    if (txn_active_) throw std::runtime_error("DDL is not allowed inside a transaction");
    if (FindTable(st.table)) throw std::runtime_error("table already exists: " + st.table);
    TableEntry t; t.id = next_table_id_++; t.name = st.table;
    for (size_t i = 0; i < st.coldefs.size(); ++i) {
        Column c; c.name = st.coldefs[i].name; c.type = st.coldefs[i].is_int ? COL_INT : COL_TEXT; c.pk = st.coldefs[i].pk;
        if (c.pk) t.pk_idx = (int)i;
        t.cols.push_back(c);
    }
    {
        PageGuard g = PageGuard::Create(bpm_.get());
        SlottedPage(g.data()).Init(); g.MarkDirty();
        t.first_page = t.last_page = g.id();
    }
    bpm_->FlushPage(t.first_page);
    if (t.pk_idx >= 0) {
        t.root_page = BTree::CreateEmpty(bpm_.get());
        bpm_->FlushPage(t.root_page);
    }
    tables_[t.name] = t;
    TableEntry& ref = tables_[t.name];
    if (ref.root_page >= 0) ref.index.Init(bpm_.get(), ref.root_page);
    SaveMeta();
    r.kind = "CREATE"; r.message = "table " + t.name + " created" + (t.pk_idx >= 0 ? " (with B+ tree index on " + t.cols[t.pk_idx].name + ")" : "");
}

void Database::DoDrop(const Stmt& st, QueryResult& r) {
    if (txn_active_) throw std::runtime_error("DDL is not allowed inside a transaction");
    NeedTable(st.table);
    tables_.erase(Lower(st.table));
    SaveMeta();
    r.kind = "DROP"; r.message = "table " + st.table + " dropped (its pages are not reused in this version)";
}

void Database::DoShowTables(QueryResult& r) {
    r.kind = "SHOW";
    r.columns.push_back("table"); r.columns.push_back("columns"); r.columns.push_back("heap_pages"); r.columns.push_back("index_root_page");
    for (std::map<std::string, TableEntry>::iterator it = tables_.begin(); it != tables_.end(); ++it) {
        TableEntry& t = it->second; std::string cs;
        for (size_t i = 0; i < t.cols.size(); ++i) { if (i) cs += ", "; cs += t.cols[i].name + " " + (t.cols[i].type == COL_INT ? "INT" : "TEXT") + (t.cols[i].pk ? " PK" : ""); }
        Row row; row.push_back(Value::Text(t.name)); row.push_back(Value::Text(cs)); row.push_back(Value::Int(CountHeapPages(t)));
        row.push_back(Value::Int(t.pk_idx >= 0 ? t.index.root() : -1));
        r.rows.push_back(row);
    }
    r.message = Str(tables_.size()) + " table(s)";
}

void Database::DoDescribe(const Stmt& st, QueryResult& r) {
    TableEntry& t = NeedTable(st.table);
    r.kind = "DESCRIBE";
    r.columns.push_back("column"); r.columns.push_back("type"); r.columns.push_back("key");
    for (size_t i = 0; i < t.cols.size(); ++i) {
        Row row; row.push_back(Value::Text(t.cols[i].name)); row.push_back(Value::Text(t.cols[i].type == COL_INT ? "INT" : "TEXT"));
        row.push_back(Value::Text(t.cols[i].pk ? "PRIMARY KEY (B+ tree)" : ""));
        r.rows.push_back(row);
    }
    r.message = "first heap page " + Str(t.first_page);
}

// ============================================================ binding
Value Database::BindLiteral(const Column& c, const Literal& l) {
    if (c.type == COL_INT) {
        if (!l.is_int) throw std::runtime_error("column " + c.name + " expects an INT");
        if (l.i > 2147483647LL || l.i < -2147483647LL) throw std::runtime_error("integer out of range for column " + c.name);
        return Value::Int((int32_t)l.i);
    }
    if (l.is_int) throw std::runtime_error("column " + c.name + " expects TEXT (use quotes)");
    if (l.s.size() > 255) throw std::runtime_error("text too long for column " + c.name + " (max 255)");
    return Value::Text(l.s);
}

std::vector<Cond> Database::BindConds(const TableEntry& t, const std::vector<CondAst>& w) {
    std::vector<Cond> out;
    for (size_t i = 0; i < w.size(); ++i) {
        Cond c; c.col = t.FindCol(w[i].col);
        if (c.col < 0) throw std::runtime_error("no such column: " + w[i].col);
        c.op = w[i].op; c.v = BindLiteral(t.cols[c.col], w[i].lit);
        out.push_back(c);
    }
    return out;
}

// ============================================================ planner: choose index or full scan
Plan Database::MakePlan(TableEntry& t, const std::vector<Cond>& conds) {
    Plan p;
    std::string filt;
    long long lo = -INF, hi = INF; bool has_eq = false; long long eq = 0;
    for (size_t i = 0; i < conds.size(); ++i) {
        const Cond& c = conds[i];
        bool on_pk = (t.pk_idx >= 0 && c.col == t.pk_idx);
        if (on_pk && c.op != OP_NE) {
            long long v = c.v.i;
            if (c.op == OP_EQ) { has_eq = true; eq = v; }
            else if (c.op == OP_GT) lo = std::max(lo, v + 1);
            else if (c.op == OP_GE) lo = std::max(lo, v);
            else if (c.op == OP_LT) hi = std::min(hi, v - 1);
            else if (c.op == OP_LE) hi = std::min(hi, v);
        } else {
            filt += (filt.empty() ? "" : " AND ") + t.cols[c.col].name + " " + OpText(c.op) + " " + ValText(c.v);
        }
    }
    std::string pk = t.pk_idx >= 0 ? t.cols[t.pk_idx].name : "";
    if (has_eq) {
        p.kind = 1; p.key = eq;
        p.text = "Index Scan on " + t.name + " using B+ tree: " + pk + " = " + Str(eq) + "  (descends root -> leaf, no heap scan)";
    } else if (lo > -INF || hi < INF) {
        p.kind = 2; p.lo = lo; p.hi = hi;
        p.text = "Index Range Scan on " + t.name + " using B+ tree: " + pk + " in [" + (lo > -INF ? Str(lo) : "-inf") + ", " + (hi < INF ? Str(hi) : "+inf") + "]  (follows the leaf chain)";
    } else {
        p.kind = 0;
        p.text = "Seq Scan on " + t.name + "  (reads every heap page)";
    }
    if (!filt.empty()) p.text += "  + Filter: " + filt;
    return p;
}

bool Database::FetchRow(TableEntry& t, const RID& rid, RowRef* out) {
    PageGuard g(bpm_.get(), rid.page);
    SlottedPage sp(g.data());
    int len; const char* p = sp.Get(rid.slot, &len);
    if (!p) return false;
    out->rid = rid; out->raw.assign(p, len); out->row = DecodeRow(t, p, len);
    return true;
}

void Database::CollectRows(TableEntry& t, const std::vector<Cond>& conds, const Plan& plan, long long limit, std::vector<RowRef>* out) {
    if (plan.kind == 1) {
        if (plan.key > 2147483647LL || plan.key < -2147483647LL) return;
        RID rid; RowRef rr;
        if (t.index.Search((int32_t)plan.key, &rid) && FetchRow(t, rid, &rr) && Matches(conds, rr.row)) out->push_back(rr);
        return;
    }
    if (plan.kind == 2) {
        std::vector<std::pair<int32_t, RID> > hits; t.index.Range(plan.lo, plan.hi, &hits);
        for (size_t i = 0; i < hits.size(); ++i) {
            RowRef rr;
            if (FetchRow(t, hits[i].second, &rr) && Matches(conds, rr.row)) { out->push_back(rr); if (limit >= 0 && (long long)out->size() >= limit) return; }
        }
        return;
    }
    page_id_t pid = t.first_page;
    while (pid != INVALID_PAGE_ID) {
        PageGuard g(bpm_.get(), pid);
        if ((unsigned char)g.data()[OFF_TYPE] != PT_HEAP) break;
        SlottedPage sp(g.data());
        for (int s = 0; s < sp.NumSlots(); ++s) {
            int len; const char* p = sp.Get(s, &len);
            if (!p) continue;
            RowRef rr; rr.row = DecodeRow(t, p, len);
            if (!Matches(conds, rr.row)) continue;
            rr.rid = RID(pid, s); rr.raw.assign(p, len);
            out->push_back(rr);
            if (limit >= 0 && (long long)out->size() >= limit) return;
        }
        pid = sp.NextPage();
    }
}

// ============================================================ DML
void Database::DoInsert(const Stmt& st, QueryResult& r) {
    TableEntry& t = NeedTable(st.table);
    std::vector<Row> rows;
    for (size_t i = 0; i < st.rows.size(); ++i) {
        if (st.rows[i].size() != t.cols.size())
            throw std::runtime_error("table " + t.name + " has " + Str(t.cols.size()) + " columns but " + Str(st.rows[i].size()) + " values were given");
        Row row;
        for (size_t c = 0; c < t.cols.size(); ++c) row.push_back(BindLiteral(t.cols[c], st.rows[i][c]));
        if (EncodeRow(t, row).size() > 1000) throw std::runtime_error("row too large (max 1000 bytes)");
        rows.push_back(row);
    }
    if (t.pk_idx >= 0) {                      // validate BEFORE touching anything: statement is all-or-nothing
        std::set<int32_t> batch;
        for (size_t i = 0; i < rows.size(); ++i) {
            int32_t k = rows[i][t.pk_idx].i; RID tmp;
            if (!batch.insert(k).second || t.index.Search(k, &tmp)) throw std::runtime_error("duplicate primary key: " + Str(k));
        }
    }
    WithAutoTxn([&]() { for (size_t i = 0; i < rows.size(); ++i) InsertRow(t, rows[i]); });
    r.kind = "INSERT"; r.message = Str(rows.size()) + " row(s) inserted";
}

void Database::DoSelect(const Stmt& st, QueryResult& r) {
    TableEntry& t = NeedTable(st.table);
    std::vector<Cond> conds = BindConds(t, st.where);
    std::vector<int> proj;
    if (st.star || st.count_star) for (size_t i = 0; i < t.cols.size(); ++i) proj.push_back((int)i);
    else for (size_t i = 0; i < st.cols.size(); ++i) {
        int c = t.FindCol(st.cols[i]); if (c < 0) throw std::runtime_error("no such column: " + st.cols[i]);
        proj.push_back(c);
    }
    int ord = -1;
    if (!st.order_col.empty()) { ord = t.FindCol(st.order_col); if (ord < 0) throw std::runtime_error("no such column: " + st.order_col); }
    Plan plan = MakePlan(t, conds);
    r.plan = plan.text; r.kind = st.explain ? "EXPLAIN" : "SELECT";
    if (st.explain) { r.message = "plan only - the query was not executed"; return; }
    std::vector<RowRef> refs;
    CollectRows(t, conds, plan, (ord < 0 && !st.count_star) ? st.limit : -1, &refs);
    if (ord >= 0) {
        bool desc = st.order_desc;
        std::stable_sort(refs.begin(), refs.end(), [ord, desc](const RowRef& a, const RowRef& b) {
            int c = CmpVal(a.row[ord], b.row[ord]); return desc ? c > 0 : c < 0; });
    }
    if (st.count_star) {
        r.columns.push_back("count");
        Row row; row.push_back(Value::Int((int32_t)refs.size())); r.rows.push_back(row);
        r.message = "1 row"; return;
    }
    for (size_t i = 0; i < proj.size(); ++i) r.columns.push_back(t.cols[proj[i]].name);
    size_t n = refs.size();
    if (st.limit >= 0 && (long long)n > st.limit) n = (size_t)st.limit;
    for (size_t i = 0; i < n; ++i) {
        Row row; for (size_t c = 0; c < proj.size(); ++c) row.push_back(refs[i].row[proj[c]]);
        r.rows.push_back(row);
    }
    r.message = Str(n) + " row(s)";
}

void Database::DoUpdate(const Stmt& st, QueryResult& r) {
    TableEntry& t = NeedTable(st.table);
    std::vector<Cond> conds = BindConds(t, st.where);
    std::vector<std::pair<int, Value> > asg;
    for (size_t i = 0; i < st.assigns.size(); ++i) {
        int c = t.FindCol(st.assigns[i].first);
        if (c < 0) throw std::runtime_error("no such column: " + st.assigns[i].first);
        if (c == t.pk_idx) throw std::runtime_error("the PRIMARY KEY column cannot be updated");
        asg.push_back(std::make_pair(c, BindLiteral(t.cols[c], st.assigns[i].second)));
    }
    Plan plan = MakePlan(t, conds);
    r.plan = plan.text; r.kind = st.explain ? "EXPLAIN" : "UPDATE";
    if (st.explain) { r.message = "plan only - the statement was not executed"; return; }
    std::vector<RowRef> refs; CollectRows(t, conds, plan, -1, &refs);
    std::vector<Row> news;
    for (size_t i = 0; i < refs.size(); ++i) {
        Row nr = refs[i].row;
        for (size_t a = 0; a < asg.size(); ++a) nr[asg[a].first] = asg[a].second;
        if (EncodeRow(t, nr).size() > 1000) throw std::runtime_error("row too large (max 1000 bytes)");
        news.push_back(nr);
    }
    // an update is logged as DELETE old image + INSERT new image
    WithAutoTxn([&]() { for (size_t i = 0; i < refs.size(); ++i) { DeleteRow(t, refs[i].rid, refs[i].row, refs[i].raw); InsertRow(t, news[i]); } });
    r.message = Str(refs.size()) + " row(s) updated";
}

void Database::DoDelete(const Stmt& st, QueryResult& r) {
    TableEntry& t = NeedTable(st.table);
    std::vector<Cond> conds = BindConds(t, st.where);
    Plan plan = MakePlan(t, conds);
    r.plan = plan.text; r.kind = st.explain ? "EXPLAIN" : "DELETE";
    if (st.explain) { r.message = "plan only - the statement was not executed"; return; }
    std::vector<RowRef> refs; CollectRows(t, conds, plan, -1, &refs);
    WithAutoTxn([&]() { for (size_t i = 0; i < refs.size(); ++i) DeleteRow(t, refs[i].rid, refs[i].row, refs[i].raw); });
    r.message = Str(refs.size()) + " row(s) deleted";
}

// ============================================================ demo data
QueryResult Database::Seed(int n) {
    return RunWrapped([&](QueryResult& r) {
        if (n < 1 || n > 20000) throw std::runtime_error("seed count must be between 1 and 20000");
        if (!FindTable("students")) {
            Stmt st = ParseSql("CREATE TABLE students (id INT PRIMARY KEY, name TEXT, dept TEXT, marks INT)");
            QueryResult tmp; DoCreate(st, tmp);
        }
        TableEntry& t = NeedTable("students");
        if (t.cols.size() != 4) throw std::runtime_error("table 'students' exists with a different shape");
        std::vector<std::pair<int32_t, RID> > all; t.index.Range(-INF, INF, &all);
        int start = all.empty() ? 1 : all.back().first + 1;
        static const char* first[] = {"Aarav","Diya","Rohan","Isha","Kabir","Meera","Vihaan","Anaya","Arjun","Saanvi","Aditya","Riya","Dev","Tara","Neel","Kiara"};
        static const char* last[] = {"Sharma","Verma","Rawat","Negi","Bisht","Joshi","Pant","Kohli","Yadav","Chauhan","Dobal","Gupta"};
        static const char* dept[] = {"CSE","ECE","ME","CE","IT","EE"};
        WithAutoTxn([&]() {
            for (int i = 0; i < n; ++i) {
                int id = start + i; unsigned h = (unsigned)id * 2654435761u;
                Row row;
                row.push_back(Value::Int(id));
                row.push_back(Value::Text(std::string(first[(h >> 3) % 16]) + " " + last[(h >> 9) % 12]));
                row.push_back(Value::Text(dept[(h >> 15) % 6]));
                row.push_back(Value::Int(30 + (int)((h >> 20) % 71)));
                InsertRow(t, row);
            }
        });
        r.kind = "SEED"; r.message = Str(n) + " students inserted (ids " + Str(start) + ".." + Str(start + n - 1) + ")";
    });
}

QueryResult Database::Admin(const std::string& what) {
    return RunWrapped([&](QueryResult& r) {
        if (what == "flush") {
            bpm_->FlushAllPages();
            r.kind = "FLUSH"; r.message = "every dirty page was written to disk (the WAL rule forced the log first). Uncommitted changes may now be on disk too = STEAL.";
        } else if (what == "checkpoint") {
            Checkpoint(); r.kind = "CHECKPOINT"; r.message = "checkpoint done: all dirty pages on disk, recovery will start from here";
        } else throw std::runtime_error("unknown admin command");
    });
}
