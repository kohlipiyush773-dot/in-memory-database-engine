#include "simulator.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include "buffer_pool_manager.h"

TraceResult RunTrace(const std::string& policy, int frames, const std::vector<page_id_t>& refs, const std::string& tmp_path) {
    TraceResult tr; tr.policy = Upper(policy); tr.frames = frames; tr.hits = tr.faults = 0;
    std::remove(tmp_path.c_str());
    {
        DiskManager dm(tmp_path);
        BufferPoolManager bpm((size_t)frames, &dm, MakeReplacer(policy, (size_t)frames, &refs));
        for (size_t i = 0; i < refs.size(); ++i) {
            bool tr_flag; bpm.DrainEvents(&tr_flag);
            long h0 = bpm.stats().hits;
            Page* p = bpm.FetchPage(refs[i]);
            TraceStep s; s.index = (int)i; s.page = refs[i]; s.hit = bpm.stats().hits > h0; s.evicted = INVALID_PAGE_ID;
            std::vector<BufEvent> ev = bpm.DrainEvents(&tr_flag);
            for (size_t k = 0; k < ev.size(); ++k) if (std::strcmp(ev[k].type, "EVICT") == 0) s.evicted = ev[k].page;
            for (int f = 0; f < frames; ++f) { FrameInfo fi = bpm.Info(f); s.frames.push_back(fi.page); s.refs.push_back(fi.ref); }
            s.hand = bpm.replacer()->Hand();
            if (p) bpm.UnpinPage(refs[i], false);
            if (s.hit) ++tr.hits; else ++tr.faults;
            tr.steps.push_back(s);
        }
    }
    std::remove(tmp_path.c_str());
    return tr;
}

std::vector<page_id_t> MakeWorkload(const std::string& name, int pages, int ops, unsigned seed) {
    std::mt19937 rng(seed);
    std::vector<page_id_t> t; t.reserve(ops);
    std::vector<int> perm(pages);
    for (int i = 0; i < pages; ++i) perm[i] = i;
    std::shuffle(perm.begin(), perm.end(), rng);       // so hot pages are not neighbours
    if (name == "seq") {
        for (int i = 0; i < ops; ++i) t.push_back(i % pages);                 // cyclic table scan
    } else if (name == "random") {
        for (int i = 0; i < ops; ++i) t.push_back((page_id_t)(rng() % pages));
    } else if (name == "zipf") {                                              // skewed: few pages get most accesses
        std::vector<double> cdf(pages); double sum = 0;
        for (int i = 0; i < pages; ++i) { sum += 1.0 / std::pow((double)(i + 1), 0.99); cdf[i] = sum; }
        for (int i = 0; i < ops; ++i) {
            double u = (double)(rng() % 1000000) / 1000000.0 * sum;
            int rank = (int)(std::lower_bound(cdf.begin(), cdf.end(), u) - cdf.begin());
            if (rank >= pages) rank = pages - 1;
            t.push_back(perm[rank]);
        }
    } else if (name == "hotcold") {                                           // 80% of accesses go to 20% of pages
        int hot = std::max(1, pages / 5);
        for (int i = 0; i < ops; ++i) {
            if (rng() % 100 < 80) t.push_back(perm[rng() % hot]);
            else t.push_back(perm[hot + rng() % std::max(1, pages - hot)]);
        }
    } else throw std::runtime_error("unknown workload: " + name);
    return t;
}

std::vector<BenchRow> RunBenchmark(const BenchConfig& cfg, const std::string& tmp_path) {
    std::vector<BenchRow> out;
    std::remove(tmp_path.c_str());
    DiskManager dm(tmp_path);
    {   // create the simulated disk: cfg.pages empty pages
        char buf[PAGE_SIZE]; std::memset(buf, 0, PAGE_SIZE); buf[OFF_TYPE] = (char)PT_RAW;
        for (int i = 0; i < cfg.pages; ++i) { dm.EnsureAllocated(i); dm.WritePage(i, buf); }
    }
    for (size_t w = 0; w < cfg.workloads.size(); ++w) {
        std::vector<page_id_t> trace = MakeWorkload(cfg.workloads[w], cfg.pages, cfg.ops, cfg.seed);
        std::mt19937 wr(cfg.seed + 7);
        std::vector<char> is_write(trace.size());
        for (size_t i = 0; i < trace.size(); ++i) is_write[i] = (int)(wr() % 100) < cfg.write_pct;
        for (size_t s = 0; s < cfg.sizes.size(); ++s) {
            for (size_t p = 0; p < cfg.policies.size(); ++p) {
                size_t frames = (size_t)cfg.sizes[s];
                dm.ResetStats();
                BufferPoolManager bpm(frames, &dm, MakeReplacer(cfg.policies[p], frames, &trace));
                bpm.SetRecording(false);
                std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
                for (size_t i = 0; i < trace.size(); ++i) {
                    Page* pg = bpm.FetchPage(trace[i]);
                    if (is_write[i]) pg->GetData()[100]++;           // modify the page -> it becomes dirty
                    bpm.UnpinPage(trace[i], is_write[i] != 0);
                }
                double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                BenchRow r; r.workload = cfg.workloads[w]; r.policy = Upper(cfg.policies[p]); r.frames = (int)frames;
                r.ops = (long)trace.size(); r.hits = bpm.stats().hits; r.misses = bpm.stats().misses; r.evictions = bpm.stats().evictions;
                r.disk_reads = dm.Reads(); r.disk_writes = dm.Writes(); r.total_ms = ms;
                out.push_back(r);
            }
        }
    }
    return out;
}
