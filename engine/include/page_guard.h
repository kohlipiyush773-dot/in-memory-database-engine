// page_guard.h - RAII helper: a fetched page is ALWAYS unpinned, even if an exception is thrown.
#pragma once
#include "buffer_pool_manager.h"

class PageGuard {
public:
    PageGuard(BufferPoolManager* b, page_id_t id) : b_(b), p_(b->FetchPage(id)), dirty_(false) {
        if (!p_) throw std::runtime_error("buffer pool exhausted: every frame is pinned");
    }
    static PageGuard Create(BufferPoolManager* b) { return PageGuard(b); }
    PageGuard(PageGuard&& o) : b_(o.b_), p_(o.p_), dirty_(o.dirty_) { o.p_ = 0; }
    ~PageGuard() { if (p_) b_->UnpinPage(p_->GetPageId(), dirty_); }
    char* data() { return p_->GetData(); }
    page_id_t id() const { return p_->GetPageId(); }
    void MarkDirty() { dirty_ = true; }
    lsn_t lsn() const { return p_->GetLsn(); }
    void SetLsn(lsn_t l) { p_->SetLsn(l); dirty_ = true; }
private:
    explicit PageGuard(BufferPoolManager* b) : b_(b), p_(0), dirty_(true) {
        page_id_t id;
        p_ = b->NewPage(&id);
        if (!p_) throw std::runtime_error("buffer pool exhausted: every frame is pinned");
    }
    PageGuard(const PageGuard&);
    PageGuard& operator=(const PageGuard&);
    BufferPoolManager* b_;
    Page* p_;
    bool dirty_;
};
