// test_engine.cpp - unit + integration tests for every layer (no external framework).
#include <cstdio>
#include <random>
#include <set>
#include "database.h"
#include "simulator.h"

static int g_fail = 0, g_pass = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("   FAIL line %d: %s\n", __LINE__, #cond); ++g_fail; } else ++g_pass; } while (0)
#define SECTION(name) std::printf("[test] %s\n", name)

static void Cleanup(const std::string& p) { std::remove(p.c_str()); std::remove((p + ".wal").c_str()); }
static QueryResult Q_(Database& db, const std::string& sql) { return db.Execute(sql); }
static long Count(Database& db, const std::string& table, const std::string& where = "") {
    QueryResult r = db.Execute("SELECT COUNT(*) FROM " + table + (where.empty() ? "" : " WHERE " + where));
    return r.ok && !r.rows.empty() ? r.rows[0][0].i : -1;
}

// ------------------------------------------------------------------ storage layer
static void TestSlottedPage() {
    SECTION("slotted page: insert, delete, reuse, compaction");
    char buf[PAGE_SIZE]; SlottedPage sp(buf); sp.Init();
    std::string rec(50, 'a'); int n = 0;
    while (sp.CanInsert(50)) { int s = sp.FindFreeSlot(); CHECK(sp.InsertAt(s, rec.data(), 50)); ++n; }
    CHECK(n == 75);
    CHECK(sp.Delete(3) && sp.Delete(10) && sp.Delete(11));
    CHECK(!sp.SlotUsed(3));
    std::string big(140, 'b');
    CHECK(sp.CanInsert(140));
    int s = sp.FindFreeSlot(); CHECK(s == 3);
    CHECK(sp.InsertAt(s, big.data(), 140));                  // needs compaction of the holes
    int len; const char* p = sp.Get(3, &len); CHECK(p && len == 140 && p[0] == 'b');
    p = sp.Get(5, &len); CHECK(p && len == 50 && p[49] == 'a');
    CHECK(!sp.InsertAt(3, rec.data(), 50));                  // slot occupied
    CHECK(sp.LiveRecords() == 73);
}

static void TestDiskAndLog() {
    SECTION("disk manager + write-ahead log file format");
    Cleanup("t_disk.db");
    {
        DiskManager dm("t_disk.db");
        char w[PAGE_SIZE]; std::memset(w, 7, PAGE_SIZE);
        page_id_t a = dm.AllocatePage(), b = dm.AllocatePage(); CHECK(a == 0 && b == 1);
        dm.WritePage(1, w);
        char r[PAGE_SIZE]; dm.ReadPage(1, r); CHECK(r[100] == 7);
        dm.ReadPage(0, r); CHECK(r[100] == 0);               // hole reads as zeros
        CHECK(dm.Writes() == 1 && dm.Reads() == 2);
    }
    {
        DiskManager dm("t_disk.db"); CHECK(dm.NumPages() == 2);   // size survives reopen
    }
    Cleanup("t_disk.db");
    Cleanup("t_log.db");
    {
        LogManager lm("t_log.db.wal");
        lsn_t a = lm.Append(LOG_BEGIN, 1, 0, -1, -1, 0, "");
        lsn_t b = lm.Append(LOG_INSERT, 1, 5, 9, 2, 0, "hello");
        CHECK(a == 1 && b > a);
        CHECK(lm.FlushedLsn() == 0);
        lm.Append(LOG_COMMIT, 1, 0, -1, -1, 0, "");
        lm.FlushUpTo(b);
        CHECK(lm.FlushedLsn() > 0);
        lm.Append(LOG_BEGIN, 2, 0, -1, -1, 0, "");              // never flushed -> lost on "crash"
    }
    {
        LogManager lm("t_log.db.wal");
        std::vector<LogRecord> v = lm.ReadFrom(0, false);
        CHECK(v.size() == 3);
        CHECK(v[1].type == LOG_INSERT && v[1].data == "hello" && v[1].page == 9 && v[1].slot == 2);
    }
    {   // a torn / corrupt tail must be ignored
        std::FILE* f = std::fopen("t_log.db.wal", "ab"); std::fputs("garbage-bytes-torn-write", f); std::fclose(f);
        LogManager lm("t_log.db.wal");
        CHECK(lm.ReadFrom(0, false).size() == 3);
        lm.Append(LOG_BEGIN, 3, 0, -1, -1, 0, ""); lm.Flush();
        LogManager lm2("t_log.db.wal"); CHECK(lm2.ReadFrom(0, false).size() == 4);
    }
    Cleanup("t_log.db");
}

// ------------------------------------------------------------------ OS layer
static void TestReplacementPolicies() {
    SECTION("page replacement: textbook reference strings");
    int ref1[] = {7,0,1,2,0,3,0,4,2,3,0,3,2,1,2,0,1,7,0,1};            // Silberschatz example, 3 frames
    std::vector<page_id_t> r1(ref1, ref1 + 20);
    TraceResult fifo = RunTrace("FIFO", 3, r1, "t_trace.tmp");
    TraceResult lru = RunTrace("LRU", 3, r1, "t_trace.tmp");
    TraceResult opt = RunTrace("OPT", 3, r1, "t_trace.tmp");
    TraceResult clk = RunTrace("CLOCK", 3, r1, "t_trace.tmp");
    CHECK(fifo.faults == 15); CHECK(lru.faults == 12); CHECK(opt.faults == 9);
    CHECK(opt.faults <= clk.faults && clk.faults <= 20);
    std::printf("   FIFO=%ld LRU=%ld CLOCK=%ld OPT=%ld faults (expected 15/12/?/9)\n", fifo.faults, lru.faults, clk.faults, opt.faults);

    SECTION("Belady's anomaly: FIFO gets WORSE with more frames");
    int ref2[] = {1,2,3,4,1,2,5,1,2,3,4,5};
    std::vector<page_id_t> r2(ref2, ref2 + 12);
    long f3 = RunTrace("FIFO", 3, r2, "t_trace.tmp").faults, f4 = RunTrace("FIFO", 4, r2, "t_trace.tmp").faults;
    CHECK(f3 == 9 && f4 == 10);
    long l3 = RunTrace("LRU", 3, r2, "t_trace.tmp").faults, l4 = RunTrace("LRU", 4, r2, "t_trace.tmp").faults;
    CHECK(l4 <= l3);                                                    // LRU is a stack algorithm: no anomaly
    long o3 = RunTrace("OPT", 3, r2, "t_trace.tmp").faults, o4 = RunTrace("OPT", 4, r2, "t_trace.tmp").faults;
    CHECK(o4 <= o3);

    SECTION("clock: second chance clears reference bit before evicting");
    std::unique_ptr<Replacer> c = MakeReplacer("CLOCK", 3);
    for (int f = 0; f < 3; ++f) { c->Access(f, f, false); c->SetEvictable(f, true); }
    CHECK(c->RefBit(0) == 1);
    frame_id_t v; CHECK(c->Victim(&v) && v == 0);                       // all had ref=1: hand sweeps, clears, then evicts frame 0
    CHECK(c->RefBit(1) == 0 && c->RefBit(2) == 0);
    c->Access(0, 9, false); c->SetEvictable(0, true);
    c->Access(1, 1, true);                                              // hit -> ref bit set again, saved from eviction
    CHECK(c->Victim(&v) && v == 2);

    SECTION("pinned frames are never victims");
    std::unique_ptr<Replacer> l = MakeReplacer("LRU", 2);
    l->Access(0, 1, false); l->Access(1, 2, false);
    l->SetEvictable(1, true);
    CHECK(l->Victim(&v) && v == 1);
    CHECK(!l->Victim(&v));
    std::remove("t_trace.tmp");
}

static void TestBufferPool() {
    SECTION("buffer pool: eviction writes dirty pages, nothing is lost");
    Cleanup("t_bp.db");
    DiskManager dm("t_bp.db");
    BufferPoolManager bpm(4, &dm, MakeReplacer("LRU", 4));
    page_id_t id;
    for (int i = 0; i < 20; ++i) {
        Page* p = bpm.NewPage(&id); CHECK(p != 0);
        std::sprintf(p->GetData() + 100, "page-%d", i);
        bpm.UnpinPage(id, true);
    }
    CHECK(bpm.stats().evictions == 16 && bpm.stats().dirty_evictions == 16);
    bool ok = true;
    for (int i = 0; i < 20; ++i) {
        Page* p = bpm.FetchPage(i); char e[32]; std::sprintf(e, "page-%d", i);
        if (!p || std::strcmp(p->GetData() + 100, e) != 0) ok = false;
        if (p) bpm.UnpinPage(i, false);
    }
    CHECK(ok);
    SECTION("buffer pool: pinned pages block eviction");
    Page* a = bpm.FetchPage(0); Page* b = bpm.FetchPage(1); Page* c = bpm.FetchPage(2); Page* d = bpm.FetchPage(3);
    CHECK(a && b && c && d);
    CHECK(bpm.FetchPage(4) == 0);                                       // everything pinned
    bpm.UnpinPage(0, false);
    CHECK(bpm.FetchPage(4) != 0);
    SECTION("buffer pool: clean pages are not written");
    bpm.UnpinPage(1, false); bpm.UnpinPage(2, false); bpm.UnpinPage(3, false); bpm.UnpinPage(4, false);
    bpm.FlushAllPages();
    long w0 = dm.Writes();
    for (int i = 0; i < 10; ++i) { bpm.FetchPage(i); bpm.UnpinPage(i, false); }
    CHECK(dm.Writes() == w0);
    Cleanup("t_bp.db");

    SECTION("WAL rule: log reaches disk BEFORE the dirty page");
    Cleanup("t_wal.db");
    DiskManager dm2("t_wal.db"); LogManager lg("t_wal.db.wal");
    BufferPoolManager b2(2, &dm2, MakeReplacer("LRU", 2), &lg);
    Page* p0 = b2.NewPage(&id);
    lsn_t l = lg.Append(LOG_INSERT, 1, 1, 0, 0, 0, "x");
    p0->SetLsn(l); b2.UnpinPage(id, true);
    CHECK(lg.FlushedLsn() < l);                                        // log still only in memory
    for (int i = 0; i < 3; ++i) { b2.NewPage(&id); b2.UnpinPage(id, false); }   // forces eviction of page 0
    CHECK(lg.FlushedLsn() >= l);                                       // WAL rule enforced on eviction
    CHECK(b2.stats().wal_forces == 1);
    Cleanup("t_wal.db");
}

// ------------------------------------------------------------------ B+ tree
static void TestBTree() {
    SECTION("B+ tree: 3000 random keys, search, range, delete");
    Cleanup("t_bt.db");
    DiskManager dm("t_bt.db"); BufferPoolManager bpm(10, &dm, MakeReplacer("CLOCK", 10));
    BTree t; t.Init(&bpm, BTree::CreateEmpty(&bpm));
    std::mt19937 rng(11); std::set<int> keys;
    bool ok = true;
    for (int i = 0; i < 3000; ++i) { int k = (int)(rng() % 50000); bool dup = keys.count(k) > 0; if (t.Insert(k, RID(k, k % 9)) == dup) ok = false; keys.insert(k); }
    CHECK(ok);
    ok = true; for (std::set<int>::iterator it = keys.begin(); it != keys.end(); ++it) { RID r; if (!t.Search(*it, &r) || r.page != *it) ok = false; }
    CHECK(ok);
    std::vector<std::pair<int32_t, RID> > out; t.Range(1000, 20000, &out);
    size_t exp = 0; for (std::set<int>::iterator it = keys.begin(); it != keys.end(); ++it) if (*it >= 1000 && *it <= 20000) ++exp;
    CHECK(out.size() == exp);
    bool sorted = true; for (size_t i = 1; i < out.size(); ++i) if (out[i - 1].first >= out[i].first) sorted = false;
    CHECK(sorted);
    CHECK(t.Height() >= 4);
    for (std::set<int>::iterator it = keys.begin(); it != keys.end(); ++it) if (*it % 2 == 0) CHECK(t.Remove(*it));
    ok = true; for (std::set<int>::iterator it = keys.begin(); it != keys.end(); ++it) { RID r; if (t.Search(*it, &r) == (*it % 2 == 0)) ok = false; }
    CHECK(ok);
    std::vector<BTree::Node> nodes; t.Dump(&nodes); CHECK(nodes.size() > 100);
    Cleanup("t_bt.db");
}

// ------------------------------------------------------------------ SQL / DBMS
static void TestSql() {
    SECTION("SQL: DDL, DML, planner, constraints");
    Cleanup("t_sql.db");
    Database db; db.Open("t_sql.db", 8, "LRU");
    CHECK(Q_(db, "CREATE TABLE s (id INT PRIMARY KEY, name TEXT, marks INT)").ok);
    CHECK(!Q_(db, "CREATE TABLE s (id INT)").ok);                                      // exists
    CHECK(Q_(db, "INSERT INTO s VALUES (1,'Ann',50),(2,'Bob',70),(3,'O''Neil',90)").ok);
    CHECK(!Q_(db, "INSERT INTO s VALUES (4,'x',1),(2,'dup',1)").ok);                  // duplicate inside statement
    CHECK(Count(db, "s") == 3);                                                        // statement was atomic
    CHECK(!Q_(db, "INSERT INTO s VALUES (9,'x')").ok);                                 // wrong arity
    CHECK(!Q_(db, "INSERT INTO s VALUES ('x','y',3)").ok);                             // wrong type
    CHECK(!Q_(db, "SELEC * FROM s").ok);
    QueryResult r = Q_(db, "SELECT * FROM s WHERE id = 3");
    CHECK(r.ok && r.rows.size() == 1 && r.rows[0][1].s == "O'Neil");
    CHECK(r.plan.find("Index Scan") == 0);
    r = Q_(db, "SELECT * FROM s WHERE id > 1 AND id <= 3");
    CHECK(r.plan.find("Index Range Scan") == 0 && r.rows.size() == 2);
    r = Q_(db, "SELECT * FROM s WHERE marks >= 70");
    CHECK(r.plan.find("Seq Scan") == 0 && r.rows.size() == 2);
    r = Q_(db, "SELECT name FROM s ORDER BY marks DESC LIMIT 2");
    CHECK(r.rows.size() == 2 && r.rows[0][0].s == "O'Neil" && r.rows[1][0].s == "Bob");
    CHECK(Q_(db, "UPDATE s SET marks = 99, name = 'Robert' WHERE id = 2").ok);
    r = Q_(db, "SELECT * FROM s WHERE id = 2"); CHECK(r.rows[0][1].s == "Robert" && r.rows[0][2].i == 99);
    CHECK(!Q_(db, "UPDATE s SET id = 7 WHERE id = 2").ok);                             // PK is immutable
    CHECK(Q_(db, "DELETE FROM s WHERE marks < 60").ok);
    CHECK(Count(db, "s") == 2);
    r = Q_(db, "SELECT * FROM s WHERE id = 1"); CHECK(r.rows.empty());
    CHECK(Q_(db, "CREATE TABLE logs (msg TEXT)").ok);                                  // table without index
    CHECK(Q_(db, "INSERT INTO logs VALUES ('a'),('b')").ok && Count(db, "logs") == 2);
    CHECK(Q_(db, "SHOW TABLES").rows.size() == 2);
    CHECK(Q_(db, "EXPLAIN SELECT * FROM s WHERE id = 3").plan.find("Index Scan") == 0);
    CHECK(!Q_(db, "SELECT * FROM nope").ok);
    db.Close();
    Cleanup("t_sql.db");
}

static void TestBigTable() {
    SECTION("larger-than-pool table: 3000 rows, 8-frame pool");
    Cleanup("t_big.db");
    Database db; db.Open("t_big.db", 8, "CLOCK");
    CHECK(db.Seed(3000).ok);
    CHECK(Count(db, "students") == 3000);
    CHECK(db.disk()->NumPages() > 100);                                               // data is >10x the pool
    bool ok = true;
    for (int id = 1; id <= 3000; id += 97) { QueryResult r = Q_(db, "SELECT * FROM students WHERE id = " + Str(id)); if (!r.ok || r.rows.size() != 1 || r.rows[0][0].i != id) ok = false; }
    CHECK(ok);
    QueryResult a = Q_(db, "SELECT * FROM students WHERE id = 1500");
    QueryResult b = Q_(db, "SELECT * FROM students WHERE marks = 55");
    CHECK(a.io.hits + a.io.misses < 10);                                              // index: a handful of pages
    CHECK(b.io.hits + b.io.misses > 15);                                              // seq scan: every heap page
    std::printf("   index lookup touched %ld pages, full scan touched %ld pages\n", a.io.hits + a.io.misses, b.io.hits + b.io.misses);
    CHECK(Q_(db, "DELETE FROM students WHERE id <= 1000").ok);
    CHECK(Count(db, "students") == 2000);
    CHECK(Q_(db, "INSERT INTO students VALUES (5,'back','CSE',1)").ok);               // freed slot / key reusable
    CHECK(Count(db, "students") == 2001);
    db.Close();
    Database db2; db2.Open("t_big.db", 16, "FIFO");                                   // clean restart, other policy
    CHECK(!db2.recovery().ran);
    CHECK(Count(db2, "students") == 2001);
    db2.Close();
    Cleanup("t_big.db");
}

// ------------------------------------------------------------------ transactions + recovery
static void TestTransactions() {
    SECTION("transactions: COMMIT and ROLLBACK (data + index)");
    Cleanup("t_tx.db");
    Database db; db.Open("t_tx.db", 8, "LRU");
    Q_(db, "CREATE TABLE a (id INT PRIMARY KEY, v TEXT)");
    Q_(db, "INSERT INTO a VALUES (1,'one'),(2,'two'),(3,'three')");
    CHECK(Q_(db, "BEGIN").ok);
    CHECK(!Q_(db, "BEGIN").ok);
    Q_(db, "INSERT INTO a VALUES (4,'four')");
    Q_(db, "UPDATE a SET v = 'TWO' WHERE id = 2");
    Q_(db, "DELETE FROM a WHERE id = 1");
    CHECK(Count(db, "a") == 3);
    CHECK(!Q_(db, "CREATE TABLE z (x INT)").ok);                                       // no DDL in txn
    CHECK(Q_(db, "ROLLBACK").ok);
    CHECK(Count(db, "a") == 3);
    QueryResult r = Q_(db, "SELECT * FROM a WHERE id = 2"); CHECK(r.rows.size() == 1 && r.rows[0][1].s == "two");
    r = Q_(db, "SELECT * FROM a WHERE id = 1"); CHECK(r.rows.size() == 1);            // deleted row is back, index too
    r = Q_(db, "SELECT * FROM a WHERE id = 4"); CHECK(r.rows.empty());
    Q_(db, "BEGIN"); Q_(db, "INSERT INTO a VALUES (10,'ten')"); CHECK(Q_(db, "COMMIT").ok);
    CHECK(Count(db, "a") == 4);
    CHECK(!Q_(db, "COMMIT").ok);
    db.Close();
    Cleanup("t_tx.db");
}

static void TestRecoveryRedo() {
    SECTION("crash recovery: REDO (committed data lived only in the log)");
    Cleanup("t_rd.db");
    {
        Database db; db.Open("t_rd.db", 16, "LRU");
        Q_(db, "CREATE TABLE a (id INT PRIMARY KEY, v TEXT)");
        db.Seed(1);                                                                    // creates 'students' too
        Q_(db, "INSERT INTO a VALUES (1,'one'),(2,'two'),(3,'three')");
        db.Seed(400);                                                                  // several heap pages + splits
        Q_(db, "UPDATE a SET v = 'uno' WHERE id = 1");
        CHECK(db.bpm()->stats().dirty_evictions >= 0);
        db.Abandon();                                                                  // CRASH: dirty pages are lost
    }
    Database db; db.Open("t_rd.db", 16, "LRU");
    CHECK(db.recovery().ran);
    CHECK(db.recovery().redo_applied > 0);
    CHECK(db.recovery().losers == 0);
    CHECK(db.recovery().indexes_rebuilt);
    std::printf("   redo applied=%ld skipped=%ld\n", db.recovery().redo_applied, db.recovery().redo_skipped);
    CHECK(Count(db, "a") == 3);
    QueryResult r = Q_(db, "SELECT * FROM a WHERE id = 1"); CHECK(r.plan.find("Index Scan") == 0 && r.rows.size() == 1 && r.rows[0][1].s == "uno");
    CHECK(Count(db, "students") == 401);
    r = Q_(db, "SELECT * FROM students WHERE id = 250"); CHECK(r.rows.size() == 1);
    db.Close();
    Cleanup("t_rd.db");
}

static void TestRecoveryUndo() {
    SECTION("crash recovery: UNDO (uncommitted change had reached the disk = STEAL)");
    Cleanup("t_ud.db");
    {
        Database db; db.Open("t_ud.db", 16, "LRU");
        Q_(db, "CREATE TABLE a (id INT PRIMARY KEY, v TEXT)");
        Q_(db, "INSERT INTO a VALUES (1,'one'),(2,'two')");
        Q_(db, "BEGIN");
        Q_(db, "INSERT INTO a VALUES (3,'three'),(4,'four')");
        Q_(db, "DELETE FROM a WHERE id = 1");
        Q_(db, "UPDATE a SET v = 'dirty' WHERE id = 2");
        QueryResult f = db.Admin("flush");                                             // uncommitted data written to disk
        CHECK(f.ok && f.io.wal_forces >= 1);
        db.Abandon();                                                                  // CRASH before COMMIT
    }
    Database db; db.Open("t_ud.db", 16, "LRU");
    CHECK(db.recovery().losers == 1);
    CHECK(db.recovery().undo_applied >= 4);
    std::printf("   losers=%ld undo applied=%ld redo applied=%ld\n", db.recovery().losers, db.recovery().undo_applied, db.recovery().redo_applied);
    CHECK(Count(db, "a") == 2);
    QueryResult r = Q_(db, "SELECT * FROM a WHERE id = 1"); CHECK(r.rows.size() == 1 && r.rows[0][1].s == "one");
    r = Q_(db, "SELECT * FROM a WHERE id = 2"); CHECK(r.rows.size() == 1 && r.rows[0][1].s == "two");
    r = Q_(db, "SELECT * FROM a WHERE id = 3"); CHECK(r.rows.empty());
    db.Close();
    Database again; again.Open("t_ud.db", 16, "LRU");                                 // after clean close: no recovery needed
    CHECK(!again.recovery().ran); CHECK(Count(again, "a") == 2);
    again.Close();
    Cleanup("t_ud.db");
}

static void TestRecoveryDoubleCrash() {
    SECTION("crash recovery: crash again right after recovery, and a crash with a rolled-back txn");
    Cleanup("t_dc.db");
    {
        Database db; db.Open("t_dc.db", 16, "CLOCK");
        Q_(db, "CREATE TABLE a (id INT PRIMARY KEY, v TEXT)");
        Q_(db, "INSERT INTO a VALUES (1,'x'),(2,'y')");
        Q_(db, "BEGIN"); Q_(db, "INSERT INTO a VALUES (3,'z')"); Q_(db, "ROLLBACK");
        Q_(db, "BEGIN"); Q_(db, "DELETE FROM a WHERE id = 1"); db.Admin("flush");
        db.Abandon();
    }
    { Database db; db.Open("t_dc.db", 16, "CLOCK"); CHECK(Count(db, "a") == 2); db.Abandon(); }   // crash again immediately
    Database db; db.Open("t_dc.db", 16, "CLOCK");
    CHECK(Count(db, "a") == 2);
    QueryResult r = Q_(db, "SELECT * FROM a WHERE id = 1"); CHECK(r.rows.size() == 1);
    db.Close();
    Cleanup("t_dc.db");
}

static void TestReconfigure() {
    SECTION("switching policy / pool size at runtime keeps data");
    Cleanup("t_rc.db");
    Database db; db.Open("t_rc.db", 8, "LRU");
    db.Seed(500);
    CHECK(Count(db, "students") == 500);
    db.Reconfigure("CLOCK", 32);
    CHECK(db.policy() == "CLOCK" && db.bpm()->PoolSize() == 32);
    CHECK(Count(db, "students") == 500);
    bool threw = false; try { db.Reconfigure("OPT", 16); } catch (...) { threw = true; } CHECK(threw);
    QueryResult r = Q_(db, "SELECT * FROM students WHERE id = 77"); CHECK(r.rows.size() == 1);
    db.Close();
    Cleanup("t_rc.db");
}

static void TestBenchmark() {
    SECTION("benchmark: OPT >= LRU >= FIFO-ish on skewed data, loops hurt LRU");
    BenchConfig cfg; cfg.pages = 200; cfg.ops = 4000; cfg.sizes.push_back(20);
    cfg.workloads.push_back("seq"); cfg.workloads.push_back("zipf");
    cfg.policies.push_back("FIFO"); cfg.policies.push_back("LRU"); cfg.policies.push_back("CLOCK"); cfg.policies.push_back("OPT");
    std::vector<BenchRow> rows = RunBenchmark(cfg, "t_bench.tmp");
    CHECK(rows.size() == 8);
    long h[2][4];
    for (size_t i = 0; i < rows.size(); ++i) h[i / 4][i % 4] = rows[i].hits;
    CHECK(h[0][1] == 0);                                  // cyclic scan bigger than cache: LRU never hits
    CHECK(h[0][3] > 0);                                   // OPT still gets hits
    CHECK(h[1][3] >= h[1][1] && h[1][1] >= h[1][0] - 200);// zipf: OPT best, LRU close to / above FIFO
    for (size_t i = 0; i < rows.size(); ++i) CHECK(rows[i].hits + rows[i].misses == rows[i].ops);
    std::remove("t_bench.tmp");
}

int main() {
    TestSlottedPage(); TestDiskAndLog(); TestReplacementPolicies(); TestBufferPool(); TestBTree();
    TestSql(); TestBigTable(); TestTransactions(); TestRecoveryRedo(); TestRecoveryUndo(); TestRecoveryDoubleCrash();
    TestReconfigure(); TestBenchmark();
    std::printf("\n%d checks passed, %d failed\n", g_pass, g_fail);
    std::printf(g_fail ? "SOME TESTS FAILED\n" : "ALL TESTS PASSED\n");
    return g_fail ? 1 : 0;
}
