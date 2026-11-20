// database.h - the whole DBMS on top of the buffer pool:
//   catalog (table schemas), heap files of slotted pages, B+ tree primary-key index,
//   SQL execution, transactions (BEGIN/COMMIT/ROLLBACK), write-ahead log and crash recovery.
#pragma once
#include <functional>
#include <map>
#include <memory>
#include <set>
#include "btree.h"
#include "buffer_pool_manager.h"
#include "log_manager.h"
#include "page_guard.h"
#include "slotted_page.h"
#include "sql.h"

enum ColType { COL_INT = 0, COL_TEXT = 1 };
struct Column { std::string name; ColType type; bool pk; };

struct Value {
    ColType type; int32_t i; std::string s;
    Value() : type(COL_INT), i(0) {}
    static Value Int(int32_t v) { Value x; x.type = COL_INT; x.i = v; return x; }
    static Value Text(const std::string& v) { Value x; x.type = COL_TEXT; x.s = v; return x; }
};
typedef std::vector<Value> Row;

struct TableEntry {
    int id; std::string name; std::vector<Column> cols; int pk_idx;
    page_id_t first_page, last_page, root_page;
    BTree index;
    TableEntry() : id(0), pk_idx(-1), first_page(-1), last_page(-1), root_page(-1) {}
    int FindCol(const std::string& n) const {
        for (size_t i = 0; i < cols.size(); ++i) if (cols[i].name == n) return (int)i;
        return -1;
    }
};

std::string EncodeRow(const TableEntry& t, const Row& r);
Row DecodeRow(const TableEntry& t, const char* d, int len);

struct IoStats {
    long hits, misses, evictions, dirty_evictions, flushes, wal_forces, new_pages, disk_reads, disk_writes, wal_bytes, wal_records;
    IoStats() : hits(0), misses(0), evictions(0), dirty_evictions(0), flushes(0), wal_forces(0), new_pages(0),
                disk_reads(0), disk_writes(0), wal_bytes(0), wal_records(0) {}
};

struct QueryResult {
    bool ok; std::string error, kind, message, plan;
    std::vector<std::string> columns;
    std::vector<Row> rows;
    long row_count; bool truncated;
    IoStats io; long elapsed_us;
    std::vector<BufEvent> events; bool events_truncated;
    QueryResult() : ok(true), row_count(0), truncated(false), elapsed_us(0), events_truncated(false) {}
};

struct RecoveryReport {
    bool ran;
    long log_records, txns, committed, aborted, losers, redo_applied, redo_skipped, undo_applied;
    bool indexes_rebuilt;
    RecoveryReport() : ran(false), log_records(0), txns(0), committed(0), aborted(0), losers(0), redo_applied(0),
                       redo_skipped(0), undo_applied(0), indexes_rebuilt(false) {}
};

struct Cond { int col; CmpOp op; Value v; };
struct Plan { int kind; long long key, lo, hi; std::string text; Plan() : kind(0), key(0), lo(0), hi(0) {} };   // 0 seq, 1 index =, 2 index range
struct RowRef { RID rid; Row row; std::string raw; };
struct UndoOp { int type; int table; RID rid; std::string data; };

class Database {
public:
    Database();
    ~Database();
    void Open(const std::string& path, size_t frames, const std::string& policy);
    void Close();                         // clean shutdown (rollback open txn, checkpoint)
    void Abandon();                       // simulate a CRASH: nothing is flushed
    QueryResult Execute(const std::string& sql);
    QueryResult Seed(int n);
    QueryResult Admin(const std::string& what);   // "flush" or "checkpoint"
    void Reconfigure(const std::string& policy, size_t frames);
    void Checkpoint();
    void FlushAllDirty() { bpm_->FlushAllPages(); }

    BufferPoolManager* bpm() { return bpm_.get(); }
    DiskManager* disk() { return disk_.get(); }
    LogManager* log() { return log_.get(); }
    const std::string& policy() const { return policy_; }
    size_t frames() const { return frames_; }
    const std::string& path() const { return path_; }
    bool txn_active() const { return txn_active_; }
    bool txn_explicit() const { return txn_explicit_; }
    txn_id_t txn_id() const { return txn_id_; }
    const RecoveryReport& recovery() const { return recovery_; }
    std::map<std::string, TableEntry>& tables() { return tables_; }
    TableEntry* FindTable(const std::string& name);
    int CountHeapPages(TableEntry& t);                       // non-intrusive
    std::map<page_id_t, std::string> OwnerMap();             // which table owns which page (non-intrusive)
    std::string DecodePageRows(page_id_t pid, TableEntry* t);

private:
    // catalog / meta page
    void MakeBpm();
    void SaveMeta();
    void LoadCatalog(const char* text);
    std::string SerializeCatalog();
    void BindTables();
    void RebuildIndexes();
    // transactions + log
    void StartTxn(bool explicit_);
    void EnsureLogged();
    void CommitTxn();
    void RollbackTxn();
    void EndTxn();
    void WithAutoTxn(const std::function<void()>& body);
    void ApplyUndo(const UndoOp& op);
    lsn_t LogOp(int type, TableEntry& t, page_id_t page, int slot, const std::string& data);
    // recovery
    void Recover();
    bool RedoRecord(const LogRecord& r);
    bool UndoRecord(const LogRecord& r);
    // storage operations (logged)
    RID HeapInsert(TableEntry& t, const std::string& rec);
    RID InsertRow(TableEntry& t, const Row& row);
    void DeleteRow(TableEntry& t, const RID& rid, const Row& row, const std::string& raw);
    void SyncRoot(TableEntry& t);
    TableEntry* FindTableById(int id);
    // query execution
    QueryResult RunWrapped(const std::function<void(QueryResult&)>& body);
    void Dispatch(const Stmt& st, QueryResult& r);
    void DoCreate(const Stmt& st, QueryResult& r);
    void DoDrop(const Stmt& st, QueryResult& r);
    void DoInsert(const Stmt& st, QueryResult& r);
    void DoSelect(const Stmt& st, QueryResult& r);
    void DoUpdate(const Stmt& st, QueryResult& r);
    void DoDelete(const Stmt& st, QueryResult& r);
    void DoShowTables(QueryResult& r);
    void DoDescribe(const Stmt& st, QueryResult& r);
    TableEntry& NeedTable(const std::string& name);
    Value BindLiteral(const Column& c, const Literal& l);
    std::vector<Cond> BindConds(const TableEntry& t, const std::vector<CondAst>& w);
    Plan MakePlan(TableEntry& t, const std::vector<Cond>& conds);
    void CollectRows(TableEntry& t, const std::vector<Cond>& conds, const Plan& plan, long long limit, std::vector<RowRef>* out);
    bool FetchRow(TableEntry& t, const RID& rid, RowRef* out);
    IoStats Snapshot() const;

    std::string path_, policy_;
    size_t frames_;
    std::unique_ptr<DiskManager> disk_;
    std::unique_ptr<LogManager> log_;
    std::unique_ptr<BufferPoolManager> bpm_;
    std::map<std::string, TableEntry> tables_;
    int next_table_id_;
    lsn_t ckpt_lsn_;
    bool clean_flag_;
    bool txn_active_, txn_explicit_, txn_logged_;
    txn_id_t txn_id_, next_txn_id_;
    std::vector<UndoOp> undo_;
    bool crashed_, open_;
    RecoveryReport recovery_;
};
