// buffer_pool_manager.h - keeps a fixed number of pages in RAM.  It is the DBMS's own "virtual memory":
//   page table (page -> frame), free list, pin counts, dirty bits, demand loading, replacement,
//   and the WAL rule (log must reach disk before a dirty page does).
#pragma once
#include <list>
#include <memory>
#include <unordered_map>
#include <vector>
#include "disk_manager.h"
#include "log_manager.h"
#include "page.h"
#include "replacer.h"

struct BpmStats {
    long hits, misses, evictions, dirty_evictions, flushes, wal_forces, new_pages;
    BpmStats() : hits(0), misses(0), evictions(0), dirty_evictions(0), flushes(0), wal_forces(0), new_pages(0) {}
    long accesses() const { return hits + misses; }
    double hit_ratio() const { long t = hits + misses; return t ? (double)hits / (double)t : 0.0; }
};

// One thing that happened inside the pool (shown live in the web UI).
struct BufEvent {
    const char* type;     // HIT MISS NEW EVICT FLUSH WAL
    page_id_t page;       // page concerned
    frame_id_t frame;
    page_id_t other;      // MISS/NEW: page evicted to make room (or -1)
    bool dirty;           // MISS/NEW: evicted page had to be written first
};

struct FrameInfo { frame_id_t frame; page_id_t page; int pin; bool dirty; int ref; lsn_t lsn; int type; };

class BufferPoolManager {
public:
    BufferPoolManager(size_t pool_size, DiskManager* disk, std::unique_ptr<Replacer> rep, LogManager* log = 0);
    ~BufferPoolManager();                       // does NOT flush (a crash must not flush!)

    Page* FetchPage(page_id_t id);              // pin; load from disk on a miss (page fault)
    Page* NewPage(page_id_t* id);               // allocate a brand new page, pinned + dirty
    bool  UnpinPage(page_id_t id, bool dirty);
    bool  FlushPage(page_id_t id);
    void  FlushAllPages();
    bool  Peek(page_id_t id, char* out, frame_id_t* frame = 0, bool* dirty = 0);   // read without disturbing the pool

    size_t PoolSize() const { return pool_size_; }
    FrameInfo Info(frame_id_t f) const;
    const BpmStats& stats() const { return stats_; }
    void ResetStats() { stats_ = BpmStats(); }
    Replacer* replacer() { return replacer_.get(); }
    DiskManager* disk() { return disk_; }
    void SetRecording(bool on) { record_ = on; }
    std::vector<BufEvent> DrainEvents(bool* truncated);
private:
    bool GetFrame(frame_id_t* f, page_id_t* evicted, bool* evicted_dirty);
    void WriteFrame(frame_id_t f);              // WAL rule + disk write
    void Rec(const char* type, page_id_t p, frame_id_t f, page_id_t other, bool dirty);

    size_t pool_size_;
    Page* pages_;
    DiskManager* disk_;
    LogManager* log_;
    std::unique_ptr<Replacer> replacer_;
    std::unordered_map<page_id_t, frame_id_t> page_table_;
    std::list<frame_id_t> free_list_;
    BpmStats stats_;
    bool record_;
    bool truncated_;
    std::vector<BufEvent> events_;
};
