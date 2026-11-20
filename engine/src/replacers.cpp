#include "replacer.h"
#include <list>
#include <unordered_map>
#include <climits>

// ============================================================ FIFO
// Evict the page that has been in memory the longest, no matter how often it was used.
// Simple, but suffers from Belady's anomaly.
namespace {
class FifoReplacer : public Replacer {
public:
    explicit FifoReplacer(size_t n) : in_q_(n, 0), evictable_(n, 0) {}
    void Access(frame_id_t f, page_id_t, bool is_hit) override {
        if (!is_hit && !in_q_[f]) { q_.push_back(f); in_q_[f] = 1; }
    }
    void SetEvictable(frame_id_t f, bool e) override { evictable_[f] = e ? 1 : 0; }
    bool Victim(frame_id_t* out) override {
        for (std::list<frame_id_t>::iterator it = q_.begin(); it != q_.end(); ++it) {
            if (evictable_[*it]) { *out = *it; in_q_[*it] = 0; evictable_[*it] = 0; q_.erase(it); return true; }
        }
        return false;
    }
    const char* Name() const override { return "FIFO"; }
private:
    std::list<frame_id_t> q_;
    std::vector<char> in_q_, evictable_;
};

// ============================================================ LRU
// Evict the page not used for the longest time (temporal locality).  Exact LRU: list + O(1) move-to-front.
class LruReplacer : public Replacer {
public:
    explicit LruReplacer(size_t n) : pos_(n), in_(n, 0), evictable_(n, 0) {}
    void Access(frame_id_t f, page_id_t, bool) override {
        if (in_[f]) l_.splice(l_.begin(), l_, pos_[f]);
        else { l_.push_front(f); pos_[f] = l_.begin(); in_[f] = 1; }
    }
    void SetEvictable(frame_id_t f, bool e) override { evictable_[f] = e ? 1 : 0; }
    bool Victim(frame_id_t* out) override {
        for (std::list<frame_id_t>::reverse_iterator it = l_.rbegin(); it != l_.rend(); ++it) {
            if (evictable_[*it]) {
                frame_id_t f = *it;
                l_.erase(pos_[f]); in_[f] = 0; evictable_[f] = 0; *out = f; return true;
            }
        }
        return false;
    }
    const char* Name() const override { return "LRU"; }
private:
    std::list<frame_id_t> l_;                                    // front = most recently used
    std::vector<std::list<frame_id_t>::iterator> pos_;
    std::vector<char> in_, evictable_;
};

// ============================================================ CLOCK (Second Chance)
// Cheap approximation of LRU used by real kernels: one reference bit per frame and a rotating hand.
// ref=1 -> clear it and give a second chance;  ref=0 -> evict.
class ClockReplacer : public Replacer {
public:
    explicit ClockReplacer(size_t n) : n_(n), present_(n, 0), ref_(n, 0), evictable_(n, 0), hand_(0) {}
    void Access(frame_id_t f, page_id_t, bool) override { present_[f] = 1; ref_[f] = 1; }
    void SetEvictable(frame_id_t f, bool e) override { evictable_[f] = e ? 1 : 0; }
    bool Victim(frame_id_t* out) override {
        for (size_t step = 0; step < 2 * n_ + 1; ++step) {
            size_t f = hand_;
            hand_ = (hand_ + 1) % n_;
            if (!present_[f] || !evictable_[f]) continue;
            if (ref_[f]) { ref_[f] = 0; continue; }          // second chance
            present_[f] = 0; evictable_[f] = 0;
            *out = (frame_id_t)f; return true;
        }
        return false;
    }
    const char* Name() const override { return "CLOCK"; }
    int RefBit(frame_id_t f) const override { return present_[f] ? ref_[f] : -1; }
    int Hand() const override { return (int)hand_; }
private:
    size_t n_;
    std::vector<char> present_, ref_, evictable_;
    size_t hand_;
};

// ============================================================ OPT (Belady's optimal)
// Evict the page whose next use is farthest in the future.  Needs the future, so it is only a
// theoretical lower bound used to judge the real algorithms.
class OptReplacer : public Replacer {
public:
    OptReplacer(size_t n, const std::vector<page_id_t>& trace) : nu_(n, INT_MAX), present_(n, 0), evictable_(n, 0), pos_(0) {
        next_use_.assign(trace.size(), INT_MAX);
        std::unordered_map<page_id_t, int> last;
        for (int i = (int)trace.size() - 1; i >= 0; --i) {
            std::unordered_map<page_id_t, int>::iterator it = last.find(trace[i]);
            next_use_[i] = (it == last.end()) ? INT_MAX : it->second;
            last[trace[i]] = i;
        }
    }
    void Access(frame_id_t f, page_id_t, bool) override {
        present_[f] = 1;
        nu_[f] = pos_ < next_use_.size() ? next_use_[pos_] : INT_MAX;
        ++pos_;
    }
    void SetEvictable(frame_id_t f, bool e) override { evictable_[f] = e ? 1 : 0; }
    bool Victim(frame_id_t* out) override {
        int best = -1;
        for (size_t f = 0; f < nu_.size(); ++f) {
            if (!present_[f] || !evictable_[f]) continue;
            if (best < 0 || nu_[f] > nu_[best]) best = (int)f;
        }
        if (best < 0) return false;
        present_[best] = 0; evictable_[best] = 0; *out = best; return true;
    }
    const char* Name() const override { return "OPT"; }
private:
    std::vector<int> next_use_, nu_;
    std::vector<char> present_, evictable_;
    size_t pos_;
};
}  // namespace

bool IsValidPolicy(const std::string& p) {
    std::string u = Upper(p);
    return u == "FIFO" || u == "LRU" || u == "CLOCK" || u == "OPT";
}

std::unique_ptr<Replacer> MakeReplacer(const std::string& policy, size_t frames, const std::vector<page_id_t>* trace) {
    std::string u = Upper(policy);
    if (u == "FIFO") return std::unique_ptr<Replacer>(new FifoReplacer(frames));
    if (u == "LRU") return std::unique_ptr<Replacer>(new LruReplacer(frames));
    if (u == "CLOCK") return std::unique_ptr<Replacer>(new ClockReplacer(frames));
    if (u == "OPT") {
        if (!trace) throw std::runtime_error("OPT needs the full reference string (offline algorithm)");
        return std::unique_ptr<Replacer>(new OptReplacer(frames, *trace));
    }
    throw std::runtime_error("unknown policy: " + policy);
}
