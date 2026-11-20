// replacer.h - page replacement policy interface (OS concept).
// The buffer pool asks the replacer: "all frames are full, which page should leave?"
#pragma once
#include <memory>
#include <string>
#include <vector>
#include "common.h"

class Replacer {
public:
    virtual ~Replacer() {}
    // Called on EVERY page access.  is_hit=false means the page was just loaded into frame f.
    virtual void Access(frame_id_t f, page_id_t p, bool is_hit) = 0;
    // A pinned frame (in use) is not evictable.
    virtual void SetEvictable(frame_id_t f, bool evictable) = 0;
    // Choose the victim among evictable frames.  The replacer forgets the victim.
    virtual bool Victim(frame_id_t* f) = 0;
    virtual const char* Name() const = 0;
    virtual int RefBit(frame_id_t) const { return -1; }   // Clock only
    virtual int Hand() const { return -1; }               // Clock only
};

// policy: FIFO, LRU, CLOCK, OPT.   OPT needs the whole future reference string (offline, theoretical bound).
std::unique_ptr<Replacer> MakeReplacer(const std::string& policy, size_t frames, const std::vector<page_id_t>* trace = 0);
bool IsValidPolicy(const std::string& policy);
