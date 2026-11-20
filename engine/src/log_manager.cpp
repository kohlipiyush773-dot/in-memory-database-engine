#include "log_manager.h"
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

const char* LogTypeName(int t) {
    switch (t) {
        case LOG_BEGIN: return "BEGIN"; case LOG_COMMIT: return "COMMIT"; case LOG_ABORT: return "ABORT";
        case LOG_INSERT: return "INSERT"; case LOG_DELETE: return "DELETE"; case LOG_ALLOC: return "ALLOC";
        case LOG_CHECKPOINT: return "CHECKPOINT";
    }
    return "?";
}

static uint32_t Fnv(const char* p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i) { h ^= (unsigned char)p[i]; h *= 16777619u; }
    return h;
}

// record: [total_len u32][type u8][flags u8][txn i32][table i32][page i32][slot i32][data_len u32][data][checksum u32]
static const size_t LOG_HDR = 4 + 1 + 1 + 4 * 4 + 4;

int64_t LogManager::ParseInto(const std::string& bytes, int64_t start, int64_t base, std::vector<LogRecord>* out) {
    int64_t pos = start;
    while (pos + (int64_t)LOG_HDR + 4 <= (int64_t)bytes.size()) {
        const char* p = bytes.data() + pos;
        uint32_t total = RdU32(p);
        if (total < LOG_HDR + 4 || pos + (int64_t)total > (int64_t)bytes.size()) break;      // torn tail
        if (Fnv(p, total - 4) != RdU32(p + total - 4)) break;                                // corrupt
        LogRecord r;
        r.lsn = base + pos + 1;
        r.type = (unsigned char)p[4]; r.flags = (unsigned char)p[5];
        r.txn = RdI32(p + 6); r.table = RdI32(p + 10); r.page = RdI32(p + 14); r.slot = RdI32(p + 18);
        uint32_t dl = RdU32(p + 22);
        if (LOG_HDR + dl + 4 != total) break;
        r.data.assign(p + LOG_HDR, dl);
        if (out) out->push_back(r);
        pos += total;
    }
    return pos;
}

LogManager::LogManager(const std::string& path) : f_(0), path_(path), flushed_off_(0), next_off_(0), records_(0), forces_(0) {
    std::string content;
    std::FILE* rf = std::fopen(path.c_str(), "rb");
    if (rf) {
        char tmp[8192]; size_t n;
        while ((n = std::fread(tmp, 1, sizeof(tmp), rf)) > 0) content.append(tmp, n);
        std::fclose(rf);
    }
    std::vector<LogRecord> recs;
    int64_t valid_end = ParseInto(content, 0, 0, &recs);       // ignore any torn tail
    records_ = (long)recs.size();
    f_ = std::fopen(path.c_str(), "r+b");
    if (!f_) f_ = std::fopen(path.c_str(), "w+b");
    if (!f_) throw std::runtime_error("cannot open log file: " + path);
    flushed_off_ = next_off_ = valid_end;
}

LogManager::~LogManager() { if (f_) std::fclose(f_); }       // NOTE: unflushed buf_ is deliberately dropped

lsn_t LogManager::Append(int type, txn_id_t txn, int32_t table, int32_t page, int32_t slot, int flags, const std::string& data) {
    lsn_t lsn = next_off_ + 1;
    uint32_t total = (uint32_t)(LOG_HDR + data.size() + 4);
    std::string rec(total, '\0');
    char* p = &rec[0];
    WrU32(p, total); p[4] = (char)type; p[5] = (char)flags;
    WrI32(p + 6, txn); WrI32(p + 10, table); WrI32(p + 14, page); WrI32(p + 18, slot);
    WrU32(p + 22, (uint32_t)data.size());
    if (!data.empty()) std::memcpy(p + LOG_HDR, data.data(), data.size());
    WrU32(p + total - 4, Fnv(p, total - 4));
    buf_ += rec;
    next_off_ += total;
    ++records_;
    return lsn;
}

void LogManager::Flush() {
    if (buf_.empty()) return;
    std::fseek(f_, (long)flushed_off_, SEEK_SET);
    std::fwrite(buf_.data(), 1, buf_.size(), f_);
    std::fflush(f_);
#ifdef _WIN32
    _commit(_fileno(f_));
#else
    fsync(fileno(f_));
#endif
    flushed_off_ += (int64_t)buf_.size();
    buf_.clear();
    ++forces_;
}

std::vector<LogRecord> LogManager::ReadFrom(int64_t offset, bool include_unflushed) const {
    std::string content;
    std::FILE* rf = std::fopen(path_.c_str(), "rb");
    if (rf) {
        char tmp[8192]; size_t n;
        while ((n = std::fread(tmp, 1, sizeof(tmp), rf)) > 0) content.append(tmp, n);
        std::fclose(rf);
    }
    if ((int64_t)content.size() > flushed_off_) content.resize((size_t)flushed_off_);
    if (include_unflushed) content += buf_;
    std::vector<LogRecord> out;
    if (offset < 0) offset = 0;
    ParseInto(content, offset, 0, &out);
    return out;
}
