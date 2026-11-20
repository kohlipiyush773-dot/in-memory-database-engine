// simulator.h - OS-lab style experiments that run on the REAL buffer pool code.
//   RunTrace : step-by-step page replacement on a reference string (like the textbook table)
//   RunBenchmark : policies x buffer sizes x workloads
#pragma once
#include <string>
#include <vector>
#include "common.h"

struct TraceStep { int index; page_id_t page; bool hit; page_id_t evicted; std::vector<page_id_t> frames; std::vector<int> refs; int hand; };
struct TraceResult { std::string policy; int frames; std::vector<TraceStep> steps; long hits, faults; };
TraceResult RunTrace(const std::string& policy, int frames, const std::vector<page_id_t>& refs, const std::string& tmp_path);

struct BenchConfig {
    int pages, ops, write_pct; unsigned seed;
    std::vector<int> sizes; std::vector<std::string> workloads, policies;
    BenchConfig() : pages(1000), ops(20000), write_pct(20), seed(42) {}
};
struct BenchRow {
    std::string workload, policy; int frames;
    long ops, hits, misses, evictions, disk_reads, disk_writes; double total_ms;
};
std::vector<BenchRow> RunBenchmark(const BenchConfig& cfg, const std::string& tmp_path);
std::vector<page_id_t> MakeWorkload(const std::string& name, int pages, int ops, unsigned seed);
