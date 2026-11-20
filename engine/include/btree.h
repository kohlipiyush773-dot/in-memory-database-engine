// btree.h - B+ tree index on an INT primary key (DBMS concept).  Every node is one 4 KB page
// that is read and written THROUGH the buffer pool, so index lookups also cause hits / misses.
//
//  leaf    : [hdr 24][ (key, rid.page, rid.slot) x n ]   + pointer to next leaf (range scans)
//  internal: [hdr 24][child0][ (key_i, child_i+1) x n ]
// MAX_KEYS is deliberately small (8) so that splits and several levels are visible in the demo.
#pragma once
#include <utility>
#include "buffer_pool_manager.h"

class BTree {
public:
    enum { MAX_KEYS = 8 };
    struct Node { page_id_t id; bool leaf; std::vector<int32_t> keys; std::vector<page_id_t> children; std::vector<RID> rids; page_id_t next; int level; };

    BTree() : bpm_(0), root_(INVALID_PAGE_ID) {}
    void Init(BufferPoolManager* b, page_id_t root) { bpm_ = b; root_ = root; }
    page_id_t root() const { return root_; }
    static page_id_t CreateEmpty(BufferPoolManager* b);      // allocates the root leaf page
    void Reset();                                            // empty tree again (used to rebuild after a crash)

    bool Search(int32_t key, RID* rid);
    bool Insert(int32_t key, const RID& rid);                // false = duplicate key
    bool Remove(int32_t key);                                // lazy delete: no rebalancing
    void Range(long long lo, long long hi, std::vector<std::pair<int32_t, RID> >* out);
    int  Height();
    void Dump(std::vector<Node>* out);                       // non-intrusive (uses Peek)
private:
    bool InsertRec(page_id_t pid, int32_t key, const RID& rid, bool* dup, int32_t* up_key, page_id_t* new_pid);
    BufferPoolManager* bpm_;
    page_id_t root_;
};
