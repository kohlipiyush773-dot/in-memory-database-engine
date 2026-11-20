// common.h - basic types, constants and small helpers used by every module.
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <string>
#include <sstream>
#include <vector>
#include <algorithm>
#include <stdexcept>

// ---------------------------------------------------------------- constants
static const uint32_t PAGE_SIZE = 4096;      // one page = 4 KB (typical OS page size)
typedef int32_t page_id_t;                   // number of a page inside the database file
typedef int32_t frame_id_t;                  // index of a slot (frame) in the buffer pool
typedef int64_t lsn_t;                       // log sequence number (position in the WAL)
typedef int32_t txn_id_t;                    // transaction id
static const page_id_t INVALID_PAGE_ID = -1;

// Every page starts with the same 9-byte header:  [0..7] page LSN, [8] page type
enum PageType { PT_FREE = 0, PT_HEAP = 1, PT_BT_LEAF = 2, PT_BT_INTERNAL = 3, PT_META = 4, PT_RAW = 5 };
enum { OFF_LSN = 0, OFF_TYPE = 8 };

inline const char* PageTypeName(int t) {
    switch (t) {
        case PT_HEAP: return "heap";
        case PT_BT_LEAF: return "index-leaf";
        case PT_BT_INTERNAL: return "index-node";
        case PT_META: return "catalog";
        case PT_RAW: return "raw";
        default: return "free";
    }
}

// Record id: where a record lives = (page, slot).  Every DBMS identifies rows this way.
struct RID {
    page_id_t page; int32_t slot;
    RID() : page(-1), slot(-1) {}
    RID(page_id_t p, int32_t s) : page(p), slot(s) {}
};

// ------------------------------------------------------- tiny helpers
// (std::to_string is avoided on purpose: old MinGW compilers do not ship it)
template <class T> inline std::string Str(const T& v) { std::ostringstream os; os << v; return os.str(); }

inline std::string Lower(std::string s) { for (size_t i = 0; i < s.size(); ++i) s[i] = (char)std::tolower((unsigned char)s[i]); return s; }
inline std::string Upper(std::string s) { for (size_t i = 0; i < s.size(); ++i) s[i] = (char)std::toupper((unsigned char)s[i]); return s; }

inline int32_t  RdI32(const char* p) { int32_t v; std::memcpy(&v, p, 4); return v; }
inline void     WrI32(char* p, int32_t v) { std::memcpy(p, &v, 4); }
inline uint32_t RdU32(const char* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
inline void     WrU32(char* p, uint32_t v) { std::memcpy(p, &v, 4); }
inline uint16_t RdU16(const char* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }
inline void     WrU16(char* p, uint16_t v) { std::memcpy(p, &v, 2); }
inline int64_t  RdI64(const char* p) { int64_t v; std::memcpy(&v, p, 8); return v; }
inline void     WrI64(char* p, int64_t v) { std::memcpy(p, &v, 8); }

inline std::string JsonEscape(const std::string& s) {
    std::string o; o.reserve(s.size() + 8);
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; std::sprintf(b, "\\u%04x", c); o += b; }
                else o += (char)c;
        }
    }
    return o;
}
inline std::string Q(const std::string& s) { return "\"" + JsonEscape(s) + "\""; }
inline const char* B(bool b) { return b ? "true" : "false"; }

inline bool ParseLL(const std::string& s, long long* out) {   // hand-written: no dependency on strtoll
    size_t i = 0; bool neg = false;
    if (s.empty()) return false;
    if (s[0] == '-' || s[0] == '+') { neg = s[0] == '-'; i = 1; }
    if (i >= s.size() || s.size() - i > 18) return false;
    long long v = 0;
    for (; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10 + (s[i] - '0');
    }
    *out = neg ? -v : v;
    return true;
}
