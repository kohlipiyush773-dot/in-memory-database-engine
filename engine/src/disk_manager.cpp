#include "disk_manager.h"
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

DiskManager::DiskManager(const std::string& path) : f_(0), path_(path), next_page_id_(0), reads_(0), writes_(0) {
    f_ = std::fopen(path.c_str(), "r+b");
    if (!f_) f_ = std::fopen(path.c_str(), "w+b");
    if (!f_) throw std::runtime_error("cannot open database file: " + path);
    std::fseek(f_, 0, SEEK_END);
    long size = std::ftell(f_);
    next_page_id_ = (page_id_t)((size + (long)PAGE_SIZE - 1) / (long)PAGE_SIZE);
}

DiskManager::~DiskManager() { if (f_) std::fclose(f_); }

void DiskManager::WritePage(page_id_t id, const char* in) {
    if (id < 0) throw std::invalid_argument("invalid page id");
    std::fseek(f_, (long)id * (long)PAGE_SIZE, SEEK_SET);
    if (std::fwrite(in, 1, PAGE_SIZE, f_) != PAGE_SIZE) throw std::runtime_error("disk write failed");
    std::fflush(f_);                 // hand the data to the OS
    ++writes_;
}

bool DiskManager::ReadPageRaw(page_id_t id, char* out) {
    std::memset(out, 0, PAGE_SIZE);
    if (id < 0) return false;
    std::fseek(f_, 0, SEEK_END);
    long size = std::ftell(f_);
    long off = (long)id * (long)PAGE_SIZE;
    if (off >= size) return false;   // never written yet -> zeros
    std::fseek(f_, off, SEEK_SET);
    size_t got = std::fread(out, 1, PAGE_SIZE, f_);
    if (got < PAGE_SIZE) std::memset(out + got, 0, PAGE_SIZE - got);
    return true;
}

void DiskManager::ReadPage(page_id_t id, char* out) {
    ReadPageRaw(id, out);
    ++reads_;
}

void DiskManager::Sync() {
    std::fflush(f_);
#ifdef _WIN32
    _commit(_fileno(f_));
#else
    fsync(fileno(f_));
#endif
}
