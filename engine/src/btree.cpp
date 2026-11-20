#include "btree.h"
#include "page_guard.h"
#include <climits>
#include <deque>

namespace {
enum { OFF_NEXT = 12, OFF_N = 16, OFF_ENT = 24 };
struct LeafEnt { int32_t k, pg, sl; };

void ReadLeaf(const char* d, std::vector<LeafEnt>* e, page_id_t* next) {
    int n = RdI32(d + OFF_N);
    *next = RdI32(d + OFF_NEXT);
    e->resize(n);
    for (int i = 0; i < n; ++i) {
        const char* p = d + OFF_ENT + 12 * i;
        (*e)[i].k = RdI32(p); (*e)[i].pg = RdI32(p + 4); (*e)[i].sl = RdI32(p + 8);
    }
}
void WriteLeaf(char* d, const std::vector<LeafEnt>& e, page_id_t next) {
    d[OFF_TYPE] = (char)PT_BT_LEAF;
    WrI32(d + OFF_NEXT, next);
    WrI32(d + OFF_N, (int32_t)e.size());
    for (size_t i = 0; i < e.size(); ++i) {
        char* p = d + OFF_ENT + 12 * i;
        WrI32(p, e[i].k); WrI32(p + 4, e[i].pg); WrI32(p + 8, e[i].sl);
    }
}
void ReadInternal(const char* d, std::vector<int32_t>* k, std::vector<page_id_t>* c) {
    int n = RdI32(d + OFF_N);
    k->resize(n); c->resize(n + 1);
    (*c)[0] = RdI32(d + OFF_ENT);
    for (int i = 0; i < n; ++i) { (*k)[i] = RdI32(d + OFF_ENT + 4 + 8 * i); (*c)[i + 1] = RdI32(d + OFF_ENT + 8 + 8 * i); }
}
void WriteInternal(char* d, const std::vector<int32_t>& k, const std::vector<page_id_t>& c) {
    d[OFF_TYPE] = (char)PT_BT_INTERNAL;
    WrI32(d + OFF_NEXT, INVALID_PAGE_ID);
    WrI32(d + OFF_N, (int32_t)k.size());
    WrI32(d + OFF_ENT, c[0]);
    for (size_t i = 0; i < k.size(); ++i) { WrI32(d + OFF_ENT + 4 + 8 * i, k[i]); WrI32(d + OFF_ENT + 8 + 8 * i, c[i + 1]); }
}
bool IsLeaf(const char* d) { return (unsigned char)d[OFF_TYPE] == PT_BT_LEAF; }
}  // namespace

page_id_t BTree::CreateEmpty(BufferPoolManager* b) {
    PageGuard g = PageGuard::Create(b);
    std::vector<LeafEnt> e;
    WriteLeaf(g.data(), e, INVALID_PAGE_ID);
    g.MarkDirty();
    return g.id();
}

void BTree::Reset() {
    PageGuard g(bpm_, root_);
    std::memset(g.data(), 0, PAGE_SIZE);
    std::vector<LeafEnt> e;
    WriteLeaf(g.data(), e, INVALID_PAGE_ID);
    g.MarkDirty();
}

bool BTree::Search(int32_t key, RID* rid) {
    page_id_t pid = root_;
    for (;;) {
        PageGuard g(bpm_, pid);
        if (IsLeaf(g.data())) {
            std::vector<LeafEnt> e; page_id_t nx; ReadLeaf(g.data(), &e, &nx);
            for (size_t i = 0; i < e.size(); ++i)
                if (e[i].k == key) { if (rid) *rid = RID(e[i].pg, e[i].sl); return true; }
            return false;
        }
        std::vector<int32_t> ks; std::vector<page_id_t> cs; ReadInternal(g.data(), &ks, &cs);
        size_t ci = 0; while (ci < ks.size() && key >= ks[ci]) ++ci;
        pid = cs[ci];
    }
}

bool BTree::InsertRec(page_id_t pid, int32_t key, const RID& rid, bool* dup, int32_t* up_key, page_id_t* new_pid) {
    PageGuard g(bpm_, pid);
    if (IsLeaf(g.data())) {
        std::vector<LeafEnt> e; page_id_t next; ReadLeaf(g.data(), &e, &next);
        size_t pos = 0; while (pos < e.size() && e[pos].k < key) ++pos;
        if (pos < e.size() && e[pos].k == key) { *dup = true; return false; }
        LeafEnt ne; ne.k = key; ne.pg = rid.page; ne.sl = rid.slot;
        e.insert(e.begin() + pos, ne);
        if ((int)e.size() <= MAX_KEYS) { WriteLeaf(g.data(), e, next); g.MarkDirty(); return false; }
        // ---- leaf split: right half moves to a new page, separator key is COPIED up
        PageGuard ng = PageGuard::Create(bpm_);
        size_t mid = e.size() / 2;
        std::vector<LeafEnt> right(e.begin() + mid, e.end());
        e.resize(mid);
        WriteLeaf(ng.data(), right, next);
        WriteLeaf(g.data(), e, ng.id());
        g.MarkDirty(); ng.MarkDirty();
        *up_key = right[0].k; *new_pid = ng.id();
        return true;
    }
    std::vector<int32_t> ks; std::vector<page_id_t> cs; ReadInternal(g.data(), &ks, &cs);
    size_t ci = 0; while (ci < ks.size() && key >= ks[ci]) ++ci;
    int32_t uk = 0; page_id_t np = INVALID_PAGE_ID;
    if (!InsertRec(cs[ci], key, rid, dup, &uk, &np)) return false;
    ks.insert(ks.begin() + ci, uk);
    cs.insert(cs.begin() + ci + 1, np);
    if ((int)ks.size() <= MAX_KEYS) { WriteInternal(g.data(), ks, cs); g.MarkDirty(); return false; }
    // ---- internal split: middle key MOVES up
    size_t mid = ks.size() / 2;
    *up_key = ks[mid];
    std::vector<int32_t> rk(ks.begin() + mid + 1, ks.end());
    std::vector<page_id_t> rc(cs.begin() + mid + 1, cs.end());
    ks.resize(mid); cs.resize(mid + 1);
    PageGuard ng = PageGuard::Create(bpm_);
    WriteInternal(ng.data(), rk, rc);
    WriteInternal(g.data(), ks, cs);
    g.MarkDirty(); ng.MarkDirty();
    *new_pid = ng.id();
    return true;
}

bool BTree::Insert(int32_t key, const RID& rid) {
    bool dup = false; int32_t uk = 0; page_id_t np = INVALID_PAGE_ID;
    bool split = InsertRec(root_, key, rid, &dup, &uk, &np);
    if (dup) return false;
    if (split) {                                   // the root itself split: tree grows one level
        PageGuard ng = PageGuard::Create(bpm_);
        std::vector<int32_t> ks(1, uk);
        std::vector<page_id_t> cs; cs.push_back(root_); cs.push_back(np);
        WriteInternal(ng.data(), ks, cs);
        ng.MarkDirty();
        root_ = ng.id();
    }
    return true;
}

bool BTree::Remove(int32_t key) {
    page_id_t pid = root_;
    for (;;) {
        PageGuard g(bpm_, pid);
        if (IsLeaf(g.data())) {
            std::vector<LeafEnt> e; page_id_t nx; ReadLeaf(g.data(), &e, &nx);
            for (size_t i = 0; i < e.size(); ++i) {
                if (e[i].k == key) { e.erase(e.begin() + i); WriteLeaf(g.data(), e, nx); g.MarkDirty(); return true; }
            }
            return false;
        }
        std::vector<int32_t> ks; std::vector<page_id_t> cs; ReadInternal(g.data(), &ks, &cs);
        size_t ci = 0; while (ci < ks.size() && key >= ks[ci]) ++ci;
        pid = cs[ci];
    }
}

void BTree::Range(long long lo, long long hi, std::vector<std::pair<int32_t, RID> >* out) {
    if (lo > hi) return;
    int32_t klo = lo < INT_MIN ? INT_MIN : (lo > INT_MAX ? INT_MAX : (int32_t)lo);
    page_id_t pid = root_;
    for (;;) {                                     // descend to the leaf that may contain lo
        PageGuard g(bpm_, pid);
        if (IsLeaf(g.data())) break;
        std::vector<int32_t> ks; std::vector<page_id_t> cs; ReadInternal(g.data(), &ks, &cs);
        size_t ci = 0; while (ci < ks.size() && klo >= ks[ci]) ++ci;
        pid = cs[ci];
    }
    bool done = false;
    while (pid != INVALID_PAGE_ID && !done) {      // walk the leaf chain
        PageGuard g(bpm_, pid);
        std::vector<LeafEnt> e; page_id_t nx; ReadLeaf(g.data(), &e, &nx);
        for (size_t i = 0; i < e.size(); ++i) {
            if (e[i].k > hi) { done = true; break; }
            if (e[i].k >= lo) out->push_back(std::make_pair(e[i].k, RID(e[i].pg, e[i].sl)));
        }
        pid = nx;
    }
}

int BTree::Height() {
    int h = 1; page_id_t pid = root_;
    for (;;) {
        PageGuard g(bpm_, pid);
        if (IsLeaf(g.data())) return h;
        std::vector<int32_t> ks; std::vector<page_id_t> cs; ReadInternal(g.data(), &ks, &cs);
        pid = cs[0]; ++h;
    }
}

void BTree::Dump(std::vector<Node>* out) {
    std::deque<std::pair<page_id_t, int> > q;
    q.push_back(std::make_pair(root_, 0));
    char buf[PAGE_SIZE];
    while (!q.empty()) {
        page_id_t pid = q.front().first; int lvl = q.front().second; q.pop_front();
        bpm_->Peek(pid, buf);
        Node n; n.id = pid; n.level = lvl; n.next = INVALID_PAGE_ID;
        if (IsLeaf(buf)) {
            std::vector<LeafEnt> e; page_id_t nx; ReadLeaf(buf, &e, &nx);
            n.leaf = true; n.next = nx;
            for (size_t i = 0; i < e.size(); ++i) { n.keys.push_back(e[i].k); n.rids.push_back(RID(e[i].pg, e[i].sl)); }
        } else {
            n.leaf = false;
            ReadInternal(buf, &n.keys, &n.children);
            for (size_t i = 0; i < n.children.size(); ++i) q.push_back(std::make_pair(n.children[i], lvl + 1));
        }
        out->push_back(n);
    }
}
