// page.h - one FRAME of the buffer pool: a 4 KB block of RAM plus its bookkeeping bits.
// In OS terms these bits are the page-table-entry bits: valid (page_id_ != -1),
// modify/dirty, and the pin (lock) count.
#pragma once
#include "common.h"
#include <cstdio>

class Page {
public:
    Page() { ResetMemory(); }
    char*       GetData()       { return data_; }
    const char* GetData() const { return data_; }
    page_id_t GetPageId()   const { return page_id_; }
    int       GetPinCount() const { return pin_count_; }
    bool      IsDirty()     const { return is_dirty_; }
    lsn_t     GetLsn()      const { return RdI64(data_ + OFF_LSN); }   // LSN of last log record applied to this page
    void      SetLsn(lsn_t v)     { WrI64(data_ + OFF_LSN, v); }
    int       GetType()     const { return (unsigned char)data_[OFF_TYPE]; }
private:
    friend class BufferPoolManager;
    void ResetMemory() { std::memset(data_, 0, PAGE_SIZE); }
    char      data_[PAGE_SIZE];
    page_id_t page_id_   = INVALID_PAGE_ID;
    int       pin_count_ = 0;       // >0 : somebody is using it, it can not be evicted
    bool      is_dirty_  = false;   // modified in RAM, disk copy is stale
};
