#include "protocol.h"
#include "simulator.h"
#include <cstdio>
#include <cstdlib>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {
std::string Num(double d) { char b[64]; std::sprintf(b, "%.6f", d); return b; }

std::string TxnJson(Database& db) {
    return std::string("{\"active\":") + B(db.txn_active()) + ",\"explicit\":" + B(db.txn_explicit()) + ",\"id\":" + Str(db.txn_id()) + "}";
}

std::string ValueJson(const Value& v) { return v.type == COL_INT ? Str(v.i) : Q(v.s); }

std::string RecoveryJson(const RecoveryReport& r) {
    std::ostringstream os;
    os << "{\"ran\":" << B(r.ran) << ",\"log_records\":" << r.log_records << ",\"txns\":" << r.txns << ",\"committed\":" << r.committed
       << ",\"aborted\":" << r.aborted << ",\"losers\":" << r.losers << ",\"redo_applied\":" << r.redo_applied
       << ",\"redo_skipped\":" << r.redo_skipped << ",\"undo_applied\":" << r.undo_applied
       << ",\"indexes_rebuilt\":" << B(r.indexes_rebuilt) << "}";
    return os.str();
}

std::string PoolJson(Database& db) {
    BufferPoolManager* b = db.bpm();
    std::ostringstream os;
    os << "{\"ok\":true,\"policy\":" << Q(db.policy()) << ",\"size\":" << b->PoolSize() << ",\"hand\":" << b->replacer()->Hand() << ",\"frames\":[";
    for (size_t i = 0; i < b->PoolSize(); ++i) {
        FrameInfo f = b->Info((frame_id_t)i);
        if (i) os << ",";
        os << "{\"f\":" << f.frame << ",\"p\":" << f.page << ",\"pin\":" << f.pin << ",\"dirty\":" << B(f.dirty) << ",\"ref\":" << f.ref
           << ",\"lsn\":" << f.lsn << ",\"type\":" << Q(f.type < 0 ? "" : PageTypeName(f.type)) << "}";
    }
    const BpmStats& s = b->stats();
    os << "],\"stats\":{\"hits\":" << s.hits << ",\"misses\":" << s.misses << ",\"evictions\":" << s.evictions
       << ",\"dirty_evictions\":" << s.dirty_evictions << ",\"flushes\":" << s.flushes << ",\"wal_forces\":" << s.wal_forces
       << ",\"new_pages\":" << s.new_pages << ",\"hit_ratio\":" << Num(s.hit_ratio()) << "}"
       << ",\"disk\":{\"reads\":" << db.disk()->Reads() << ",\"writes\":" << db.disk()->Writes() << ",\"pages\":" << db.disk()->NumPages() << "}"
       << ",\"wal\":{\"flushed\":" << db.log()->FlushedLsn() << ",\"end\":" << db.log()->EndOffset() << ",\"records\":" << db.log()->Records()
       << ",\"forces\":" << db.log()->Forces() << "},\"txn\":" << TxnJson(db) << "}";
    return os.str();
}

std::vector<page_id_t> ParseRefs(const std::string& s) {
    std::vector<page_id_t> v; std::string cur;
    for (size_t i = 0; i <= s.size(); ++i) {
        char c = i < s.size() ? s[i] : ',';
        if (c == ',' || c == ' ') { if (!cur.empty()) { long long x; if (!ParseLL(cur, &x) || x < 0 || x > 100000) throw std::runtime_error("bad page number: " + cur); v.push_back((page_id_t)x); cur.clear(); } }
        else cur += c;
    }
    return v;
}

std::vector<std::string> SplitCsv(const std::string& s) {
    std::vector<std::string> v; std::string cur;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == ',') { if (!cur.empty()) v.push_back(cur); cur.clear(); } else cur += s[i];
    }
    return v;
}

std::string TraceJson(const TraceResult& t) {
    std::ostringstream os;
    os << "{\"policy\":" << Q(t.policy) << ",\"frames\":" << t.frames << ",\"hits\":" << t.hits << ",\"faults\":" << t.faults << ",\"steps\":[";
    for (size_t i = 0; i < t.steps.size(); ++i) {
        const TraceStep& s = t.steps[i];
        if (i) os << ",";
        os << "{\"i\":" << s.index << ",\"p\":" << s.page << ",\"hit\":" << B(s.hit) << ",\"ev\":" << s.evicted << ",\"hand\":" << s.hand << ",\"frames\":[";
        for (size_t k = 0; k < s.frames.size(); ++k) { if (k) os << ","; os << s.frames[k]; }
        os << "],\"ref\":[";
        for (size_t k = 0; k < s.refs.size(); ++k) { if (k) os << ","; os << s.refs[k]; }
        os << "]}";
    }
    os << "]}";
    return os.str();
}

std::string Err(const std::string& m) { return "{\"ok\":false,\"error\":" + Q(m) + "}"; }

std::string TablesJson(Database& db) {
    std::ostringstream os; os << "{\"ok\":true,\"tables\":[";
    bool first = true;
    std::map<std::string, TableEntry>& ts = db.tables();
    for (std::map<std::string, TableEntry>::iterator it = ts.begin(); it != ts.end(); ++it) {
        TableEntry& t = it->second;
        if (!first) os << ",";
        first = false;
        int height = 0, nodes = 0;
        if (t.pk_idx >= 0) { std::vector<BTree::Node> n; t.index.Dump(&n); nodes = (int)n.size(); for (size_t i = 0; i < n.size(); ++i) height = std::max(height, n[i].level + 1); }
        os << "{\"name\":" << Q(t.name) << ",\"id\":" << t.id << ",\"first_page\":" << t.first_page << ",\"root_page\":" << (t.pk_idx >= 0 ? t.index.root() : -1)
           << ",\"heap_pages\":" << db.CountHeapPages(t) << ",\"index_height\":" << height << ",\"index_nodes\":" << nodes << ",\"columns\":[";
        for (size_t i = 0; i < t.cols.size(); ++i) {
            if (i) os << ",";
            os << "{\"name\":" << Q(t.cols[i].name) << ",\"type\":" << Q(t.cols[i].type == COL_INT ? "INT" : "TEXT") << ",\"pk\":" << B(t.cols[i].pk) << "}";
        }
        os << "]}";
    }
    os << "]}";
    return os.str();
}
}  // namespace

std::string ResultToJson(Database& db, const QueryResult& r) {
    std::ostringstream os;
    os << "{\"ok\":" << B(r.ok);
    if (!r.ok) os << ",\"error\":" << Q(r.error);
    os << ",\"kind\":" << Q(r.kind) << ",\"message\":" << Q(r.message) << ",\"plan\":" << Q(r.plan) << ",\"columns\":[";
    for (size_t i = 0; i < r.columns.size(); ++i) { if (i) os << ","; os << Q(r.columns[i]); }
    os << "],\"rows\":[";
    for (size_t i = 0; i < r.rows.size(); ++i) {
        if (i) os << ",";
        os << "[";
        for (size_t c = 0; c < r.rows[i].size(); ++c) { if (c) os << ","; os << ValueJson(r.rows[i][c]); }
        os << "]";
    }
    os << "],\"row_count\":" << r.row_count << ",\"truncated\":" << B(r.truncated) << ",\"elapsed_us\":" << r.elapsed_us;
    os << ",\"io\":{\"hits\":" << r.io.hits << ",\"misses\":" << r.io.misses << ",\"evictions\":" << r.io.evictions
       << ",\"dirty_evictions\":" << r.io.dirty_evictions << ",\"flushes\":" << r.io.flushes << ",\"wal_forces\":" << r.io.wal_forces
       << ",\"new_pages\":" << r.io.new_pages << ",\"disk_reads\":" << r.io.disk_reads << ",\"disk_writes\":" << r.io.disk_writes
       << ",\"wal_bytes\":" << r.io.wal_bytes << ",\"wal_records\":" << r.io.wal_records << "}";
    os << ",\"txn\":" << TxnJson(db) << ",\"events_truncated\":" << B(r.events_truncated) << ",\"events\":[";
    for (size_t i = 0; i < r.events.size(); ++i) {
        const BufEvent& e = r.events[i];
        if (i) os << ",";
        os << "{\"t\":\"" << e.type << "\",\"p\":" << e.page << ",\"f\":" << e.frame << ",\"o\":" << e.other << ",\"d\":" << B(e.dirty) << "}";
    }
    os << "]}";
    return os.str();
}

std::string ResultToText(const QueryResult& r) {
    std::ostringstream os;
    if (!r.ok) { os << "ERROR: " << r.error << "\n"; return os.str(); }
    if (!r.plan.empty()) os << "plan: " << r.plan << "\n";
    if (!r.columns.empty()) {
        std::vector<size_t> w(r.columns.size());
        std::vector<std::vector<std::string> > cells;
        for (size_t c = 0; c < r.columns.size(); ++c) w[c] = r.columns[c].size();
        for (size_t i = 0; i < r.rows.size(); ++i) {
            std::vector<std::string> line;
            for (size_t c = 0; c < r.rows[i].size(); ++c) {
                std::string s = r.rows[i][c].type == COL_INT ? Str(r.rows[i][c].i) : r.rows[i][c].s;
                if (s.size() > w[c]) w[c] = s.size();
                line.push_back(s);
            }
            cells.push_back(line);
        }
        for (size_t c = 0; c < r.columns.size(); ++c) { os << (c ? " | " : "") << r.columns[c] << std::string(w[c] - r.columns[c].size(), ' '); }
        os << "\n";
        for (size_t c = 0; c < r.columns.size(); ++c) os << (c ? "-+-" : "") << std::string(w[c], '-');
        os << "\n";
        for (size_t i = 0; i < cells.size(); ++i) {
            for (size_t c = 0; c < cells[i].size(); ++c) os << (c ? " | " : "") << cells[i][c] << std::string(w[c] - cells[i][c].size(), ' ');
            os << "\n";
        }
        if (r.truncated) os << "... (" << r.row_count << " rows, first 500 shown)\n";
    }
    os << r.message << "   [" << r.elapsed_us << " us | buffer pool: " << r.io.hits << " hits, " << r.io.misses << " misses, "
       << r.io.evictions << " evictions | disk: " << r.io.disk_reads << " reads, " << r.io.disk_writes << " writes]\n";
    return os.str();
}

std::string ReadyBanner(Database& db) {
    std::ostringstream os;
    os << "{\"event\":\"ready\",\"db\":" << Q(db.path()) << ",\"policy\":" << Q(db.policy()) << ",\"frames\":" << db.frames()
       << ",\"recovery\":" << RecoveryJson(db.recovery()) << "}";
    return os.str();
}

std::string HandleCommand(Database& db, const std::string& line, bool* quit) {
    std::string cmd, rest;
    size_t sp = line.find(' ');
    if (sp == std::string::npos) cmd = line; else { cmd = line.substr(0, sp); rest = line.substr(sp + 1); }
    cmd = Lower(cmd);
    while (!rest.empty() && (rest[rest.size() - 1] == '\r' || rest[rest.size() - 1] == '\n')) rest.erase(rest.size() - 1);
    try {
        if (cmd == "quit") { *quit = true; return "{\"ok\":true,\"message\":\"bye\"}"; }
        if (cmd == "sql") return ResultToJson(db, db.Execute(rest));
        if (cmd == "pool") return PoolJson(db);
        if (cmd == "tables") return TablesJson(db);
        if (cmd == "seed") { long long n = 0; if (!ParseLL(rest, &n)) return Err("usage: seed <count>"); return ResultToJson(db, db.Seed((int)n)); }
        if (cmd == "flush") return ResultToJson(db, db.Admin("flush"));
        if (cmd == "checkpoint") return ResultToJson(db, db.Admin("checkpoint"));
        if (cmd == "info") {
            std::ostringstream os;
            os << "{\"ok\":true,\"db\":" << Q(db.path()) << ",\"policy\":" << Q(db.policy()) << ",\"frames\":" << db.frames()
               << ",\"pages\":" << db.disk()->NumPages() << ",\"recovery\":" << RecoveryJson(db.recovery()) << ",\"txn\":" << TxnJson(db) << "}";
            return os.str();
        }
        if (cmd == "config") {
            std::istringstream in(rest); std::string pol; long long n = 0;
            in >> pol >> n;
            db.Reconfigure(pol, (size_t)n);
            return PoolJson(db);
        }
        if (cmd == "crash") {
            std::printf("{\"ok\":true,\"message\":\"process killed without flushing anything\"}\n");
            std::fflush(stdout);
            db.Abandon();
            _exit(0);
        }
        if (cmd == "disk") {
            std::map<page_id_t, std::string> owner = db.OwnerMap();
            std::ostringstream os; os << "{\"ok\":true,\"pages\":[";
            char buf[PAGE_SIZE];
            for (page_id_t id = 0; id < db.disk()->NumPages(); ++id) {
                frame_id_t fr; bool dirty;
                db.bpm()->Peek(id, buf, &fr, &dirty);
                int type = (unsigned char)buf[OFF_TYPE];
                int live = 0, freeb = 0;
                if (type == PT_HEAP) { SlottedPage sp2(buf); live = sp2.LiveRecords(); freeb = sp2.FreeBytes(); }
                std::map<page_id_t, std::string>::iterator o = owner.find(id);
                if (id) os << ",";
                os << "{\"id\":" << id << ",\"type\":" << Q(PageTypeName(type)) << ",\"owner\":" << Q(o == owner.end() ? "" : o->second)
                   << ",\"frame\":" << fr << ",\"dirty\":" << B(dirty) << ",\"lsn\":" << RdI64(buf) << ",\"live\":" << live << ",\"free\":" << freeb << "}";
            }
            os << "]}";
            return os.str();
        }
        if (cmd == "tree") {
            TableEntry* t = db.FindTable(rest);
            if (!t) return Err("no such table: " + rest);
            if (t->pk_idx < 0) return Err("table " + t->name + " has no PRIMARY KEY, so it has no index");
            std::vector<BTree::Node> nodes; t->index.Dump(&nodes);
            std::ostringstream os;
            os << "{\"ok\":true,\"table\":" << Q(t->name) << ",\"root\":" << t->index.root() << ",\"max_keys\":" << (int)BTree::MAX_KEYS << ",\"nodes\":[";
            char buf[PAGE_SIZE];
            for (size_t i = 0; i < nodes.size(); ++i) {
                const BTree::Node& n = nodes[i];
                frame_id_t fr; db.bpm()->Peek(n.id, buf, &fr);
                if (i) os << ",";
                os << "{\"id\":" << n.id << ",\"leaf\":" << B(n.leaf) << ",\"level\":" << n.level << ",\"next\":" << n.next << ",\"frame\":" << fr << ",\"keys\":[";
                for (size_t k = 0; k < n.keys.size(); ++k) { if (k) os << ","; os << n.keys[k]; }
                os << "],\"children\":[";
                for (size_t k = 0; k < n.children.size(); ++k) { if (k) os << ","; os << n.children[k]; }
                os << "],\"rids\":[";
                for (size_t k = 0; k < n.rids.size(); ++k) { if (k) os << ","; os << "[" << n.rids[k].page << "," << n.rids[k].slot << "]"; }
                os << "]}";
            }
            os << "]}";
            return os.str();
        }
        if (cmd == "page") {
            long long id = 0;
            if (!ParseLL(rest, &id) || id < 0 || id >= db.disk()->NumPages()) return Err("page id out of range");
            char buf[PAGE_SIZE]; frame_id_t fr; bool dirty;
            db.bpm()->Peek((page_id_t)id, buf, &fr, &dirty);
            int type = (unsigned char)buf[OFF_TYPE];
            std::map<page_id_t, std::string> owner = db.OwnerMap();
            std::string own = owner.count((page_id_t)id) ? owner[(page_id_t)id] : "";
            std::ostringstream os;
            os << "{\"ok\":true,\"id\":" << id << ",\"type\":" << Q(PageTypeName(type)) << ",\"owner\":" << Q(own) << ",\"frame\":" << fr
               << ",\"dirty\":" << B(dirty) << ",\"lsn\":" << RdI64(buf);
            if (type == PT_HEAP) {
                SlottedPage sp2(buf);
                os << ",\"num_slots\":" << sp2.NumSlots() << ",\"live\":" << sp2.LiveRecords() << ",\"free\":" << sp2.FreeBytes() << ",\"next\":" << sp2.NextPage()
                   << ",\"slots\":" << db.DecodePageRows((page_id_t)id, db.FindTable(own));
            }
            os << ",\"hex\":\"";
            for (int i = 0; i < 96; ++i) { char h[4]; std::sprintf(h, "%02x", (unsigned char)buf[i]); os << h; }
            os << "\"}";
            return os.str();
        }
        if (cmd == "wal") {
            long long n = 40; ParseLL(rest, &n);
            std::vector<LogRecord> recs = db.log()->ReadFrom(0, true);
            size_t from = recs.size() > (size_t)n ? recs.size() - (size_t)n : 0;
            std::ostringstream os;
            os << "{\"ok\":true,\"total\":" << recs.size() << ",\"flushed\":" << db.log()->FlushedLsn() << ",\"end\":" << db.log()->EndOffset() << ",\"records\":[";
            for (size_t i = from; i < recs.size(); ++i) {
                const LogRecord& r = recs[i];
                if (i > from) os << ",";
                os << "{\"lsn\":" << r.lsn << ",\"type\":" << Q(LogTypeName(r.type)) << ",\"txn\":" << r.txn << ",\"table\":" << r.table << ",\"page\":" << r.page
                   << ",\"slot\":" << r.slot << ",\"clr\":" << B((r.flags & LOGF_CLR) != 0) << ",\"len\":" << r.data.size()
                   << ",\"durable\":" << B(r.lsn <= db.log()->FlushedLsn()) << "}";
            }
            os << "]}";
            return os.str();
        }
        if (cmd == "trace") {
            std::istringstream in(rest); std::string pol, refs; long long fr = 0;
            in >> pol >> fr >> refs;
            if (!IsValidPolicy(pol)) return Err("policy must be FIFO, LRU, CLOCK or OPT");
            if (fr < 1 || fr > 32) return Err("frames must be 1..32");
            std::vector<page_id_t> r = ParseRefs(refs);
            if (r.empty() || r.size() > 300) return Err("reference string must have 1..300 pages");
            TraceResult t = RunTrace(pol, (int)fr, r, db.path() + ".trace.tmp");
            return "{\"ok\":true,\"result\":" + TraceJson(t) + "}";
        }
        if (cmd == "belady") {
            std::vector<page_id_t> r = ParseRefs(rest);
            if (r.empty() || r.size() > 300) return Err("reference string must have 1..300 pages");
            static const char* pols[] = {"FIFO", "LRU", "CLOCK", "OPT"};
            std::ostringstream os; os << "{\"ok\":true,\"frames\":[1,2,3,4,5,6,7],\"series\":{";
            for (int p = 0; p < 4; ++p) {
                if (p) os << ",";
                os << "\"" << pols[p] << "\":[";
                for (int f = 1; f <= 7; ++f) {
                    TraceResult t = RunTrace(pols[p], f, r, db.path() + ".trace.tmp");
                    if (f > 1) os << ",";
                    os << t.faults;
                }
                os << "]";
            }
            os << "}}";
            return os.str();
        }
        if (cmd == "bench") {
            std::istringstream in(rest); std::string sizes, wl, pol; long long pages = 0, ops = 0, wp = 0;
            in >> pages >> ops >> wp >> sizes >> wl >> pol;
            BenchConfig cfg;
            if (pages < 50 || pages > 20000) return Err("pages must be 50..20000");
            if (ops < 1000 || ops > 200000) return Err("operations must be 1000..200000");
            if (wp < 0 || wp > 100) return Err("write percentage must be 0..100");
            cfg.pages = (int)pages; cfg.ops = (int)ops; cfg.write_pct = (int)wp;
            std::vector<std::string> sz = SplitCsv(sizes);
            for (size_t i = 0; i < sz.size(); ++i) { long long x; if (!ParseLL(sz[i], &x) || x < 1 || x > 512) return Err("buffer sizes must be 1..512"); cfg.sizes.push_back((int)x); }
            cfg.workloads = SplitCsv(wl); cfg.policies = SplitCsv(pol);
            if (cfg.sizes.empty() || cfg.sizes.size() > 12 || cfg.workloads.empty() || cfg.policies.empty()) return Err("empty or too large benchmark selection");
            for (size_t i = 0; i < cfg.policies.size(); ++i) if (!IsValidPolicy(cfg.policies[i])) return Err("bad policy: " + cfg.policies[i]);
            std::vector<BenchRow> rows = RunBenchmark(cfg, db.path() + ".bench.tmp");
            std::remove((db.path() + ".bench.tmp").c_str());
            std::ostringstream os;
            os << "{\"ok\":true,\"config\":{\"pages\":" << pages << ",\"ops\":" << ops << ",\"write_pct\":" << wp << "},\"results\":[";
            for (size_t i = 0; i < rows.size(); ++i) {
                const BenchRow& r = rows[i];
                if (i) os << ",";
                os << "{\"w\":" << Q(r.workload) << ",\"p\":" << Q(r.policy) << ",\"f\":" << r.frames << ",\"ops\":" << r.ops << ",\"hits\":" << r.hits
                   << ",\"misses\":" << r.misses << ",\"hit_ratio\":" << Num(r.ops ? (double)r.hits / (double)r.ops : 0) << ",\"evictions\":" << r.evictions
                   << ",\"reads\":" << r.disk_reads << ",\"writes\":" << r.disk_writes << ",\"ms\":" << Num(r.total_ms) << "}";
            }
            os << "]}";
            return os.str();
        }
        return Err("unknown command: " + cmd);
    } catch (const std::exception& e) {
        return Err(e.what());
    }
}
