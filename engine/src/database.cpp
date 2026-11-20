#include "database.h"
#include <cstdlib>

static const uint32_t MAGIC = 0x50474442;   // "PGDB"

// ============================================================ row <-> bytes (serialisation)
std::string EncodeRow(const TableEntry& t, const Row& r) {
    std::string out;
    for (size_t i = 0; i < t.cols.size(); ++i) {
        if (t.cols[i].type == COL_INT) { char b[4]; WrI32(b, r[i].i); out.append(b, 4); }
        else {
            if (r[i].s.size() > 255) throw std::runtime_error("text value too long (max 255 characters)");
            char b[2]; WrU16(b, (uint16_t)r[i].s.size()); out.append(b, 2); out += r[i].s;
        }
    }
    return out;
}

Row DecodeRow(const TableEntry& t, const char* d, int len) {
    Row r; int pos = 0;
    for (size_t i = 0; i < t.cols.size(); ++i) {
        if (t.cols[i].type == COL_INT) {
            if (pos + 4 > len) throw std::runtime_error("corrupt record");
            r.push_back(Value::Int(RdI32(d + pos))); pos += 4;
        } else {
            if (pos + 2 > len) throw std::runtime_error("corrupt record");
            int n = RdU16(d + pos); pos += 2;
            if (pos + n > len) throw std::runtime_error("corrupt record");
            r.push_back(Value::Text(std::string(d + pos, n))); pos += n;
        }
    }
    return r;
}

// ============================================================ lifecycle
Database::Database() : frames_(0), next_table_id_(1), ckpt_lsn_(0), clean_flag_(true), txn_active_(false),
    txn_explicit_(false), txn_logged_(false), txn_id_(0), next_txn_id_(1), crashed_(false), open_(false) {}

Database::~Database() { if (open_ && !crashed_) { try { Close(); } catch (...) {} } }

void Database::MakeBpm() {
    bpm_.reset();
    bpm_.reset(new BufferPoolManager(frames_, disk_.get(), MakeReplacer(policy_, frames_), log_.get()));
}

void Database::Open(const std::string& path, size_t frames, const std::string& policy) {
    path_ = path; frames_ = frames; policy_ = Upper(policy);
    if (!IsValidPolicy(policy_) || policy_ == "OPT") throw std::runtime_error("policy must be FIFO, LRU or CLOCK");
    if (frames < 8) throw std::runtime_error("the buffer pool needs at least 8 frames");
    disk_.reset(new DiskManager(path));
    log_.reset(new LogManager(path + ".wal"));
    MakeBpm();
    bool fresh = disk_->NumPages() == 0;
    if (fresh) {
        { PageGuard g = PageGuard::Create(bpm_.get()); g.MarkDirty(); }       // page 0 = catalog page
        clean_flag_ = true; ckpt_lsn_ = 0; next_table_id_ = 1;
        SaveMeta();
    } else {
        std::string text;
        {
            PageGuard g(bpm_.get(), 0);
            char* d = g.data();
            if ((unsigned char)d[OFF_TYPE] != PT_META || RdU32(d + 16) != MAGIC) throw std::runtime_error("not a PageDB database file");
            clean_flag_ = d[24] != 0; next_table_id_ = RdI32(d + 28); ckpt_lsn_ = RdI64(d + 32);
            d[PAGE_SIZE - 1] = 0;
            text = std::string(d + 64);
        }
        LoadCatalog(text.c_str());
    }
    bool was_clean = clean_flag_;
    if (!was_clean) Recover();                // REDO / UNDO from the write-ahead log
    BindTables();
    if (!was_clean) { RebuildIndexes(); recovery_.indexes_rebuilt = true; }
    clean_flag_ = false;                      // from now on a crash is detectable
    SaveMeta();
    if (!was_clean) Checkpoint();
    open_ = true;
    bool tr; bpm_->DrainEvents(&tr); bpm_->ResetStats(); disk_->ResetStats();
}

void Database::Close() {
    if (!open_) return;
    if (txn_active_) RollbackTxn();
    Checkpoint();
    clean_flag_ = true;
    SaveMeta();
    bpm_->FlushAllPages();
    disk_->Sync();
    open_ = false;
    bpm_.reset(); log_.reset(); disk_.reset();
}

void Database::Abandon() {                    // crash: buffer pool and unflushed log tail simply vanish
    crashed_ = true; open_ = false;
    bpm_.reset(); log_.reset(); disk_.reset();
}

// ============================================================ catalog
std::string Database::SerializeCatalog() {
    std::ostringstream os;
    for (std::map<std::string, TableEntry>::iterator it = tables_.begin(); it != tables_.end(); ++it) {
        TableEntry& t = it->second;
        os << "T " << t.id << " " << t.name << " " << t.first_page << " " << t.root_page << " " << t.cols.size() << "\n";
        for (size_t i = 0; i < t.cols.size(); ++i)
            os << "C " << t.cols[i].name << " " << (t.cols[i].type == COL_INT ? "I" : "T") << " " << (t.cols[i].pk ? 1 : 0) << "\n";
    }
    return os.str();
}

void Database::LoadCatalog(const char* text) {
    tables_.clear();
    std::istringstream in(text);
    std::string line;
    TableEntry cur; int remaining = 0; bool have = false;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string tag; ls >> tag;
        if (tag == "T") {
            if (have) tables_[cur.name] = cur;
            cur = TableEntry(); size_t nc = 0;
            ls >> cur.id >> cur.name >> cur.first_page >> cur.root_page >> nc;
            cur.last_page = cur.first_page; remaining = (int)nc; have = true;
        } else if (tag == "C" && remaining > 0) {
            Column c; std::string ty; int pk = 0;
            ls >> c.name >> ty >> pk;
            c.type = ty == "I" ? COL_INT : COL_TEXT; c.pk = pk != 0;
            if (c.pk) cur.pk_idx = (int)cur.cols.size();
            cur.cols.push_back(c); --remaining;
        }
    }
    if (have) tables_[cur.name] = cur;
}

void Database::SaveMeta() {
    for (std::map<std::string, TableEntry>::iterator it = tables_.begin(); it != tables_.end(); ++it)
        if (it->second.pk_idx >= 0 && it->second.index.root() != INVALID_PAGE_ID) it->second.root_page = it->second.index.root();
    std::string txt = SerializeCatalog();
    if (txt.size() > PAGE_SIZE - 80) throw std::runtime_error("catalog page is full (too many tables)");
    {
        PageGuard g(bpm_.get(), 0);
        char* d = g.data();
        d[OFF_TYPE] = (char)PT_META;
        WrU32(d + 16, MAGIC); d[24] = clean_flag_ ? 1 : 0; WrI32(d + 28, next_table_id_); WrI64(d + 32, ckpt_lsn_);
        std::memset(d + 64, 0, PAGE_SIZE - 64);
        std::memcpy(d + 64, txt.data(), txt.size());
        g.MarkDirty();
    }
    bpm_->FlushPage(0);
}

void Database::BindTables() {
    for (std::map<std::string, TableEntry>::iterator it = tables_.begin(); it != tables_.end(); ++it) {
        TableEntry& t = it->second;
        if (t.root_page >= 0) t.index.Init(bpm_.get(), t.root_page);
        page_id_t pid = t.first_page, last = t.first_page;
        while (pid != INVALID_PAGE_ID) {
            PageGuard g(bpm_.get(), pid);
            if ((unsigned char)g.data()[OFF_TYPE] != PT_HEAP) break;
            last = pid; pid = SlottedPage(g.data()).NextPage();
        }
        t.last_page = last;
    }
}

void Database::RebuildIndexes() {
    for (std::map<std::string, TableEntry>::iterator it = tables_.begin(); it != tables_.end(); ++it) {
        TableEntry& t = it->second;
        if (t.pk_idx < 0) continue;
        t.index.Reset();
        std::vector<RowRef> rows; Plan p;
        CollectRows(t, std::vector<Cond>(), p, -1, &rows);
        for (size_t i = 0; i < rows.size(); ++i) t.index.Insert(rows[i].row[t.pk_idx].i, rows[i].rid);
    }
}

TableEntry* Database::FindTable(const std::string& name) {
    std::map<std::string, TableEntry>::iterator it = tables_.find(Lower(name));
    return it == tables_.end() ? 0 : &it->second;
}
TableEntry* Database::FindTableById(int id) {
    for (std::map<std::string, TableEntry>::iterator it = tables_.begin(); it != tables_.end(); ++it) if (it->second.id == id) return &it->second;
    return 0;
}

void Database::Reconfigure(const std::string& policy, size_t frames) {
    std::string p = Upper(policy);
    if (!IsValidPolicy(p) || p == "OPT") throw std::runtime_error("policy must be FIFO, LRU or CLOCK (OPT is offline-only, see the OS Lab / Benchmark tabs)");
    if (frames < 8 || frames > 512) throw std::runtime_error("frames must be between 8 and 512");
    bpm_->FlushAllPages();                    // WAL rule respected
    policy_ = p; frames_ = frames;
    MakeBpm();
    for (std::map<std::string, TableEntry>::iterator it = tables_.begin(); it != tables_.end(); ++it)
        if (it->second.root_page >= 0) it->second.index.Init(bpm_.get(), it->second.index.root());
}

// ============================================================ transactions
void Database::StartTxn(bool explicit_) {
    txn_active_ = true; txn_explicit_ = explicit_; txn_logged_ = false; txn_id_ = next_txn_id_++; undo_.clear();
}
void Database::EnsureLogged() {
    if (!txn_logged_) { log_->Append(LOG_BEGIN, txn_id_, 0, -1, -1, 0, ""); txn_logged_ = true; }
}
void Database::EndTxn() { txn_active_ = false; txn_explicit_ = false; txn_logged_ = false; undo_.clear(); }

void Database::CommitTxn() {
    if (txn_logged_) { log_->Append(LOG_COMMIT, txn_id_, 0, -1, -1, 0, ""); log_->Flush(); }   // force log = durable
    EndTxn();
}

void Database::ApplyUndo(const UndoOp& op) {
    TableEntry* t = FindTableById(op.table);
    if (!t) return;
    Row row = DecodeRow(*t, op.data.data(), (int)op.data.size());
    if (op.type == LOG_INSERT) {              // undo an insert = delete it
        lsn_t lsn = log_->Append(LOG_DELETE, txn_id_, t->id, op.rid.page, op.rid.slot, LOGF_CLR, op.data);
        { PageGuard g(bpm_.get(), op.rid.page); SlottedPage sp(g.data()); sp.Delete(op.rid.slot); g.SetLsn(lsn); }
        if (t->pk_idx >= 0) t->index.Remove(row[t->pk_idx].i);
    } else {                                  // undo a delete = put the record back in the same slot
        lsn_t lsn = log_->Append(LOG_INSERT, txn_id_, t->id, op.rid.page, op.rid.slot, LOGF_CLR, op.data);
        { PageGuard g(bpm_.get(), op.rid.page); SlottedPage sp(g.data()); sp.InsertAt(op.rid.slot, op.data.data(), (int)op.data.size()); g.SetLsn(lsn); }
        if (t->pk_idx >= 0) { t->index.Insert(row[t->pk_idx].i, op.rid); SyncRoot(*t); }
    }
}

void Database::RollbackTxn() {
    if (txn_logged_) {
        for (int i = (int)undo_.size() - 1; i >= 0; --i) ApplyUndo(undo_[i]);
        log_->Append(LOG_ABORT, txn_id_, 0, -1, -1, 0, "");
        log_->Flush();
    }
    EndTxn();
}

void Database::WithAutoTxn(const std::function<void()>& body) {
    bool mine = false;
    if (!txn_active_) { StartTxn(false); mine = true; }
    try { body(); }
    catch (...) { if (mine) RollbackTxn(); throw; }
    if (mine) CommitTxn();
}

void Database::Checkpoint() {
    if (txn_active_ && txn_logged_) throw std::runtime_error("cannot checkpoint while a transaction has uncommitted changes");
    log_->Flush();
    bpm_->FlushAllPages();
    lsn_t l = log_->Append(LOG_CHECKPOINT, 0, 0, -1, -1, 0, "");
    log_->Flush();
    disk_->Sync();
    ckpt_lsn_ = l;
    SaveMeta();
}

// ============================================================ storage operations (all WAL-logged)
lsn_t Database::LogOp(int type, TableEntry& t, page_id_t page, int slot, const std::string& data) {
    EnsureLogged();
    return log_->Append(type, txn_id_, t.id, page, slot, 0, data);
}

void Database::SyncRoot(TableEntry& t) {
    if (t.pk_idx >= 0 && t.index.root() != t.root_page) { t.root_page = t.index.root(); SaveMeta(); }
}

RID Database::HeapInsert(TableEntry& t, const std::string& rec) {
    int len = (int)rec.size();
    {   // 1) try the last page of the table
        PageGuard g(bpm_.get(), t.last_page);
        SlottedPage sp(g.data());
        if (sp.CanInsert(len)) {
            int slot = sp.FindFreeSlot();
            lsn_t lsn = LogOp(LOG_INSERT, t, t.last_page, slot, rec);      // log FIRST ...
            sp.InsertAt(slot, rec.data(), len);                            // ... then change the page
            g.SetLsn(lsn);
            return RID(t.last_page, slot);
        }
    }
    // 2) page full: allocate a new heap page and link it
    page_id_t prev = t.last_page;
    PageGuard ng = PageGuard::Create(bpm_.get());
    page_id_t npid = ng.id();
    EnsureLogged();
    lsn_t alloc = log_->Append(LOG_ALLOC, txn_id_, t.id, npid, prev, 0, "");
    SlottedPage nsp(ng.data());
    nsp.Init(); ng.SetLsn(alloc);
    { PageGuard pg(bpm_.get(), prev); SlottedPage psp(pg.data()); psp.SetNextPage(npid); pg.SetLsn(alloc); }
    t.last_page = npid;
    int slot = nsp.FindFreeSlot();
    lsn_t lsn = LogOp(LOG_INSERT, t, npid, slot, rec);
    nsp.InsertAt(slot, rec.data(), len);
    ng.SetLsn(lsn);
    return RID(npid, slot);
}

RID Database::InsertRow(TableEntry& t, const Row& row) {
    std::string rec = EncodeRow(t, row);
    RID rid = HeapInsert(t, rec);
    UndoOp u; u.type = LOG_INSERT; u.table = t.id; u.rid = rid; u.data = rec;
    undo_.push_back(u);
    if (t.pk_idx >= 0) {
        if (!t.index.Insert(row[t.pk_idx].i, rid)) throw std::runtime_error("duplicate primary key");
        SyncRoot(t);
    }
    return rid;
}

void Database::DeleteRow(TableEntry& t, const RID& rid, const Row& row, const std::string& raw) {
    lsn_t lsn = LogOp(LOG_DELETE, t, rid.page, rid.slot, raw);             // before-image goes into the log (for UNDO)
    { PageGuard g(bpm_.get(), rid.page); SlottedPage sp(g.data()); sp.Delete(rid.slot); g.SetLsn(lsn); }
    UndoOp u; u.type = LOG_DELETE; u.table = t.id; u.rid = rid; u.data = raw;
    undo_.push_back(u);
    if (t.pk_idx >= 0) t.index.Remove(row[t.pk_idx].i);
}

// ============================================================ crash recovery (ARIES-style: analysis, redo, undo)
bool Database::RedoRecord(const LogRecord& r) {
    disk_->EnsureAllocated(r.page);
    if (r.type == LOG_ALLOC) {
        bool applied = false;
        { PageGuard g(bpm_.get(), r.page); if (g.lsn() < r.lsn) { SlottedPage sp(g.data()); sp.Init(); g.SetLsn(r.lsn); applied = true; } }
        if (r.slot >= 0) {
            PageGuard g(bpm_.get(), r.slot);
            if (g.lsn() < r.lsn) { SlottedPage sp(g.data()); sp.SetNextPage(r.page); g.SetLsn(r.lsn); applied = true; }
        }
        return applied;
    }
    PageGuard g(bpm_.get(), r.page);
    if (g.lsn() >= r.lsn) return false;                  // page already contains this change (pageLSN test)
    SlottedPage sp(g.data());
    if ((unsigned char)g.data()[OFF_TYPE] != PT_HEAP) sp.Init();
    if (r.type == LOG_INSERT) sp.InsertAt(r.slot, r.data.data(), (int)r.data.size());
    else sp.Delete(r.slot);
    g.SetLsn(r.lsn);
    return true;
}

bool Database::UndoRecord(const LogRecord& r) {
    PageGuard g(bpm_.get(), r.page);
    SlottedPage sp(g.data());
    if ((unsigned char)g.data()[OFF_TYPE] != PT_HEAP) return false;
    if (r.type == LOG_INSERT) {
        int len; const char* p = sp.Get(r.slot, &len);
        if (!p || len != (int)r.data.size() || std::memcmp(p, r.data.data(), len) != 0) return false;
        lsn_t lsn = log_->Append(LOG_DELETE, r.txn, r.table, r.page, r.slot, LOGF_CLR, r.data);
        sp.Delete(r.slot); g.SetLsn(lsn);
        return true;
    }
    if (sp.SlotUsed(r.slot)) return false;
    lsn_t lsn = log_->Append(LOG_INSERT, r.txn, r.table, r.page, r.slot, LOGF_CLR, r.data);
    sp.InsertAt(r.slot, r.data.data(), (int)r.data.size());
    g.SetLsn(lsn);
    return true;
}

void Database::Recover() {
    recovery_ = RecoveryReport();
    recovery_.ran = true;
    int64_t start = ckpt_lsn_ > 0 ? ckpt_lsn_ - 1 : 0;
    std::vector<LogRecord> recs = log_->ReadFrom(start, false);
    recovery_.log_records = (long)recs.size();
    // ---- 1. ANALYSIS: which transactions finished, which were still running at the crash?
    std::map<txn_id_t, int> state;                        // 1 running, 2 committed, 3 aborted
    txn_id_t maxtxn = 0;
    for (size_t i = 0; i < recs.size(); ++i) {
        const LogRecord& r = recs[i];
        if (r.txn > maxtxn) maxtxn = r.txn;
        if (r.type == LOG_BEGIN) state[r.txn] = 1;
        else if (r.type == LOG_COMMIT) state[r.txn] = 2;
        else if (r.type == LOG_ABORT) state[r.txn] = 3;
        else if ((r.type == LOG_INSERT || r.type == LOG_DELETE || r.type == LOG_ALLOC) && !state.count(r.txn)) state[r.txn] = 1;
    }
    for (std::map<txn_id_t, int>::iterator it = state.begin(); it != state.end(); ++it) {
        ++recovery_.txns;
        if (it->second == 2) ++recovery_.committed; else if (it->second == 3) ++recovery_.aborted; else ++recovery_.losers;
    }
    // ---- 2. REDO: repeat history for every logged change whose effect is missing on the page
    for (size_t i = 0; i < recs.size(); ++i) {
        int ty = recs[i].type;
        if (ty != LOG_INSERT && ty != LOG_DELETE && ty != LOG_ALLOC) continue;
        if (RedoRecord(recs[i])) ++recovery_.redo_applied; else ++recovery_.redo_skipped;
    }
    // ---- 3. UNDO: roll back the transactions that never committed (newest change first)
    std::vector<const LogRecord*> ops;
    for (size_t i = 0; i < recs.size(); ++i) {
        const LogRecord& r = recs[i];
        if ((r.type == LOG_INSERT || r.type == LOG_DELETE) && !(r.flags & LOGF_CLR) && state[r.txn] == 1) ops.push_back(&r);
    }
    for (int i = (int)ops.size() - 1; i >= 0; --i) if (UndoRecord(*ops[i])) ++recovery_.undo_applied;
    for (std::map<txn_id_t, int>::iterator it = state.begin(); it != state.end(); ++it)
        if (it->second == 1) log_->Append(LOG_ABORT, it->first, 0, -1, -1, 0, "");
    log_->Flush();
    next_txn_id_ = maxtxn + 1;
}

// ============================================================ read-only inspectors (do not disturb the pool)
int Database::CountHeapPages(TableEntry& t) {
    int n = 0; page_id_t pid = t.first_page; char buf[PAGE_SIZE];
    while (pid != INVALID_PAGE_ID && n < 100000) {
        bpm_->Peek(pid, buf);
        if ((unsigned char)buf[OFF_TYPE] != PT_HEAP) break;
        ++n; pid = SlottedPage(buf).NextPage();
    }
    return n;
}

std::map<page_id_t, std::string> Database::OwnerMap() {
    std::map<page_id_t, std::string> m;
    m[0] = "catalog";
    char buf[PAGE_SIZE];
    for (std::map<std::string, TableEntry>::iterator it = tables_.begin(); it != tables_.end(); ++it) {
        TableEntry& t = it->second;
        page_id_t pid = t.first_page; int guard = 0;
        while (pid != INVALID_PAGE_ID && guard++ < 100000) {
            bpm_->Peek(pid, buf);
            if ((unsigned char)buf[OFF_TYPE] != PT_HEAP) break;
            m[pid] = t.name; pid = SlottedPage(buf).NextPage();
        }
        if (t.pk_idx >= 0) {
            std::vector<BTree::Node> nodes; t.index.Dump(&nodes);
            for (size_t i = 0; i < nodes.size(); ++i) m[nodes[i].id] = t.name;
        }
    }
    return m;
}

std::string Database::DecodePageRows(page_id_t pid, TableEntry* t) {
    char buf[PAGE_SIZE];
    bpm_->Peek(pid, buf);
    SlottedPage sp(buf);
    std::ostringstream os; os << "[";
    bool first = true;
    for (int s = 0; s < sp.NumSlots(); ++s) {
        int len; const char* p = sp.Get(s, &len);
        if (!first) os << ",";
        first = false;
        os << "{\"slot\":" << s << ",\"used\":" << (p ? "true" : "false");
        if (p) {
            os << ",\"len\":" << len;
            if (t) {
                try {
                    Row r = DecodeRow(*t, p, len);
                    os << ",\"values\":[";
                    for (size_t i = 0; i < r.size(); ++i) {
                        if (i) os << ",";
                        if (r[i].type == COL_INT) os << r[i].i; else os << Q(r[i].s);
                    }
                    os << "]";
                } catch (...) {}
            }
        }
        os << "}";
    }
    os << "]";
    return os.str();
}
