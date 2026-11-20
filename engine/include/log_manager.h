// log_manager.h - Write-Ahead Log (WAL).  Every change to a table page is described in the log
// BEFORE the page may reach the disk.  Commit = force the log to disk (durability).
#pragma once
#include <cstdio>
#include <string>
#include <vector>
#include "common.h"

enum LogType { LOG_BEGIN = 1, LOG_COMMIT = 2, LOG_ABORT = 3, LOG_INSERT = 4, LOG_DELETE = 5, LOG_ALLOC = 6, LOG_CHECKPOINT = 7 };
enum LogFlag { LOGF_CLR = 1 };            // compensation record written while undoing

struct LogRecord {
    lsn_t lsn;            // = byte offset of the record in the log file + 1
    int type;
    int flags;
    txn_id_t txn;
    int32_t table;
    int32_t page;         // page touched (for ALLOC: the NEW page)
    int32_t slot;         // slot touched (for ALLOC: the PREVIOUS page of the chain)
    std::string data;     // record image (after-image for INSERT, before-image for DELETE)
};

const char* LogTypeName(int t);

class LogManager {
public:
    explicit LogManager(const std::string& path);
    ~LogManager();
    lsn_t Append(int type, txn_id_t txn, int32_t table, int32_t page, int32_t slot, int flags, const std::string& data);
    void  Flush();                                    // write buffer to file + fsync
    void  FlushUpTo(lsn_t lsn) { if (lsn > flushed_off_) Flush(); }
    lsn_t FlushedLsn() const { return flushed_off_; }
    lsn_t EndOffset()  const { return next_off_; }
    std::vector<LogRecord> ReadFrom(int64_t offset, bool include_unflushed) const;
    long Records() const { return records_; }
    long Forces()  const { return forces_; }
    const std::string& path() const { return path_; }
    static int64_t ParseInto(const std::string& bytes, int64_t start, int64_t base, std::vector<LogRecord>* out);
private:
    std::FILE* f_;
    std::string path_;
    std::string buf_;          // unflushed tail of the log (lost on a crash!)
    int64_t flushed_off_;      // bytes of log that are safely in the file
    int64_t next_off_;         // offset where the next record will start
    long records_, forces_;
};
