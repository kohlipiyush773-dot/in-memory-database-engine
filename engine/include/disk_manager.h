// disk_manager.h - the "disk": one binary file split into 4 KB pages (OS side).
// Page N lives at byte offset N * 4096.  Uses real OS file I/O (fread / fwrite -> read / write system calls).
#pragma once
#include <cstdio>
#include <string>
#include "common.h"

class DiskManager {
public:
    explicit DiskManager(const std::string& path);
    ~DiskManager();
    void ReadPage(page_id_t id, char* out);            // counted
    void WritePage(page_id_t id, const char* in);      // counted
    bool ReadPageRaw(page_id_t id, char* out);         // NOT counted (used by read-only inspectors)
    page_id_t AllocatePage() { return next_page_id_++; }
    void EnsureAllocated(page_id_t id) { if (id >= next_page_id_) next_page_id_ = id + 1; }
    page_id_t NumPages() const { return next_page_id_; }
    void Sync();                                       // fsync: force the OS cache to the physical disk
    long Reads() const { return reads_; }
    long Writes() const { return writes_; }
    void ResetStats() { reads_ = writes_ = 0; }
    const std::string& path() const { return path_; }
private:
    std::FILE* f_;
    std::string path_;
    page_id_t next_page_id_;
    long reads_, writes_;
};
