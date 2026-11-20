#include "buffer_pool_manager.h"

static const size_t MAX_EVENTS = 4000;

BufferPoolManager::BufferPoolManager(size_t n, DiskManager* disk, std::unique_ptr<Replacer> rep, LogManager* log)
    : pool_size_(n), pages_(new Page[n]), disk_(disk), log_(log), replacer_(std::move(rep)), record_(true), truncated_(false) {
    for (size_t i = 0; i < n; ++i) free_list_.push_back((frame_id_t)i);
}

BufferPoolManager::~BufferPoolManager() { delete[] pages_; }

void BufferPoolManager::Rec(const char* type, page_id_t p, frame_id_t f, page_id_t other, bool dirty) {
    if (!record_) return;
    if (events_.size() >= MAX_EVENTS) { truncated_ = true; return; }
    BufEvent e; e.type = type; e.page = p; e.frame = f; e.other = other; e.dirty = dirty;
    events_.push_back(e);
}

std::vector<BufEvent> BufferPoolManager::DrainEvents(bool* truncated) {
    std::vector<BufEvent> out; out.swap(events_);
    if (truncated) *truncated = truncated_;
    truncated_ = false;
    return out;
}

// The WAL rule: before a dirty page goes to disk, the log records describing its changes must be on disk first.
void BufferPoolManager::WriteFrame(frame_id_t f) {
    Page& p = pages_[f];
    if (log_ && p.GetLsn() > log_->FlushedLsn()) {
        log_->FlushUpTo(p.GetLsn());
        ++stats_.wal_forces;
        Rec("WAL", p.page_id_, f, -1, false);
    }
    disk_->WritePage(p.page_id_, p.data_);
    p.is_dirty_ = false;
}

// 1) take a free frame, else 2) ask the replacer for a victim; a dirty victim is written back first.
bool BufferPoolManager::GetFrame(frame_id_t* f, page_id_t* evicted, bool* evicted_dirty) {
    *evicted = INVALID_PAGE_ID; *evicted_dirty = false;
    if (!free_list_.empty()) { *f = free_list_.front(); free_list_.pop_front(); return true; }
    if (!replacer_->Victim(f)) return false;               // every frame is pinned
    Page& v = pages_[*f];
    *evicted = v.page_id_;
    if (v.is_dirty_) { WriteFrame(*f); ++stats_.dirty_evictions; *evicted_dirty = true; }
    page_table_.erase(v.page_id_);
    ++stats_.evictions;
    return true;
}

Page* BufferPoolManager::FetchPage(page_id_t id) {
    std::unordered_map<page_id_t, frame_id_t>::iterator it = page_table_.find(id);
    if (it != page_table_.end()) {                          // HIT
        ++stats_.hits;
        Page& p = pages_[it->second];
        ++p.pin_count_;
        replacer_->SetEvictable(it->second, false);
        replacer_->Access(it->second, id, true);
        Rec("HIT", id, it->second, -1, false);
        return &p;
    }
    frame_id_t f; page_id_t ev; bool evd;
    if (!GetFrame(&f, &ev, &evd)) return 0;
    ++stats_.misses;                                        // MISS = page fault
    Page& p = pages_[f];
    disk_->ReadPage(id, p.data_);
    p.page_id_ = id; p.pin_count_ = 1; p.is_dirty_ = false;
    page_table_[id] = f;
    replacer_->SetEvictable(f, false);
    replacer_->Access(f, id, false);
    if (ev != INVALID_PAGE_ID) Rec("EVICT", ev, f, -1, evd);
    Rec("MISS", id, f, ev, evd);
    return &p;
}

Page* BufferPoolManager::NewPage(page_id_t* id) {
    frame_id_t f; page_id_t ev; bool evd;
    if (!GetFrame(&f, &ev, &evd)) return 0;
    ++stats_.new_pages;
    *id = disk_->AllocatePage();
    Page& p = pages_[f];
    p.ResetMemory();
    p.page_id_ = *id; p.pin_count_ = 1; p.is_dirty_ = true;
    page_table_[*id] = f;
    replacer_->SetEvictable(f, false);
    replacer_->Access(f, *id, false);
    if (ev != INVALID_PAGE_ID) Rec("EVICT", ev, f, -1, evd);
    Rec("NEW", *id, f, ev, evd);
    return &p;
}

bool BufferPoolManager::UnpinPage(page_id_t id, bool dirty) {
    std::unordered_map<page_id_t, frame_id_t>::iterator it = page_table_.find(id);
    if (it == page_table_.end()) return false;
    Page& p = pages_[it->second];
    if (p.pin_count_ <= 0) return false;
    if (dirty) p.is_dirty_ = true;
    if (--p.pin_count_ == 0) replacer_->SetEvictable(it->second, true);
    return true;
}

bool BufferPoolManager::FlushPage(page_id_t id) {
    std::unordered_map<page_id_t, frame_id_t>::iterator it = page_table_.find(id);
    if (it == page_table_.end()) return false;
    WriteFrame(it->second);
    ++stats_.flushes;
    Rec("FLUSH", id, it->second, -1, false);
    return true;
}

void BufferPoolManager::FlushAllPages() {
    for (std::unordered_map<page_id_t, frame_id_t>::iterator it = page_table_.begin(); it != page_table_.end(); ++it) {
        Page& p = pages_[it->second];
        if (p.is_dirty_) { WriteFrame(it->second); ++stats_.flushes; Rec("FLUSH", it->first, it->second, -1, false); }
    }
}

bool BufferPoolManager::Peek(page_id_t id, char* out, frame_id_t* frame, bool* dirty) {
    std::unordered_map<page_id_t, frame_id_t>::iterator it = page_table_.find(id);
    if (it != page_table_.end()) {
        std::memcpy(out, pages_[it->second].data_, PAGE_SIZE);
        if (frame) *frame = it->second;
        if (dirty) *dirty = pages_[it->second].is_dirty_;
        return true;
    }
    if (frame) *frame = -1;
    if (dirty) *dirty = false;
    disk_->ReadPageRaw(id, out);
    return false;
}

FrameInfo BufferPoolManager::Info(frame_id_t f) const {
    const Page& p = pages_[f];
    FrameInfo i;
    i.frame = f; i.page = p.page_id_; i.pin = p.pin_count_; i.dirty = p.is_dirty_;
    i.ref = p.page_id_ == INVALID_PAGE_ID ? -1 : replacer_->RefBit(f);
    i.lsn = p.page_id_ == INVALID_PAGE_ID ? 0 : p.GetLsn();
    i.type = p.page_id_ == INVALID_PAGE_ID ? -1 : p.GetType();
    return i;
}
