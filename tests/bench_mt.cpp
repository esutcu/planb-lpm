// bench_mt.cpp — multi-core throughput scaling for lpm6::Tree.
//
// Shared immutable Tree, per-thread slice of the trace, threads pinned to
// distinct logical CPUs.  Measures aggregate and per-thread MLPS at
// T = 1, 2, 4, 8 (caller can override via the thread-list arg) so that
// linear-scaling and memory-bandwidth saturation are both visible.
//
// Usage:
//     ./bench_mt <fib-file> <trace-file> [runs] [threads=1,2,4,8]
//
// Paper reports 3.4 BLPS across 12 cores on a Xeon (§5).  On a 4-core /
// 8-thread laptop we expect linear-ish scaling to 4 and then a knee
// driven by shared L3 / DRAM bandwidth and, if we go into HT, slowdown.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

#include "../src/ipv6_util.hpp"
#include "lpm6.hpp"
#include "bench_stats.hpp"

namespace {

constexpr std::size_t kBatch = 8;

bool pin_to_cpu(std::thread& t, int cpu) {
#ifdef __linux__
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return pthread_setaffinity_np(t.native_handle(), sizeof(set), &set) == 0;
#else
    (void)t; (void)cpu;
    return false;
#endif
}

std::vector<int> parse_thread_list(const char* arg) {
    std::vector<int> out;
    std::string s(arg);
    std::size_t start = 0;
    while (start < s.size()) {
        std::size_t comma = s.find(',', start);
        std::string token = s.substr(start, comma - start);
        if (!token.empty()) out.push_back(std::atoi(token.c_str()));
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

// Runs one scaling point (T threads, `runs` timed passes each) on the shared
// tree and shared trace.  Prints per-thread and aggregate stats.
void run_point(const lpm6::Tree& tree,
               const std::vector<std::uint64_t>& trace,
               int T, int runs) {
    const std::size_t per_thread = (trace.size() / static_cast<std::size_t>(T) / kBatch) * kBatch;
    if (per_thread < kBatch) {
        std::printf("  T=%d  skipped (trace too short for %d threads)\n", T, T);
        return;
    }

    // Shared state: start gate + per-thread per-run seconds.
    std::atomic<int> ready{0};
    std::atomic<int> go{0};
    std::vector<std::vector<double>> per_thread_runs(T, std::vector<double>(runs, 0.0));

    auto worker = [&](int tid) {
        const std::uint64_t* base = &trace[static_cast<std::size_t>(tid) * per_thread];
        // warmup
        {
            int hops_out[kBatch];
            volatile std::uint64_t sink = 0;
            for (std::size_t j = 0; j + kBatch <= per_thread; j += kBatch) {
                tree.lookup_batch<kBatch>(base + j, hops_out);
                for (std::size_t k = 0; k < kBatch; ++k)
                    sink += static_cast<unsigned>(hops_out[k]);
            }
            (void)sink;
        }

        // Synchronize: all threads arrive, then the last one opens the gate.
        // Each timed run still starts/stops locally, but having them release
        // at the same instant keeps the L3/DRAM-pressure regime honest.
        for (int r = 0; r < runs; ++r) {
            ready.fetch_add(1, std::memory_order_acq_rel);
            while (go.load(std::memory_order_acquire) <= r) { /* spin */ }

            using clk = std::chrono::steady_clock;
            auto t0 = clk::now();
            int hops_out[kBatch];
            volatile std::uint64_t sink = 0;
            for (std::size_t j = 0; j + kBatch <= per_thread; j += kBatch) {
                tree.lookup_batch<kBatch>(base + j, hops_out);
                for (std::size_t k = 0; k < kBatch; ++k)
                    sink += static_cast<unsigned>(hops_out[k]);
            }
            auto t1 = clk::now();
            (void)sink;
            per_thread_runs[static_cast<std::size_t>(tid)][static_cast<std::size_t>(r)] =
                std::chrono::duration<double>(t1 - t0).count();
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(T));
    for (int tid = 0; tid < T; ++tid) {
        threads.emplace_back(worker, tid);
        if (!pin_to_cpu(threads.back(), tid)) {
            std::fprintf(stderr, "  warn: failed to pin thread %d to CPU %d\n", tid, tid);
        }
    }

    // Release each run in lockstep.
    for (int r = 0; r < runs; ++r) {
        while (ready.load(std::memory_order_acquire) < (r + 1) * T) { /* spin */ }
        go.fetch_add(1, std::memory_order_acq_rel);
    }
    for (auto& t : threads) t.join();

    // Per-thread MLPS = ops_per_run / seconds, reported as a median across
    // runs.  Aggregate MLPS = sum of per-thread medians (equivalently, the
    // median of per-run-totals is close when per-thread runtimes are
    // tightly matched; sum-of-medians is the conservative readout).
    double agg_median_mlps = 0.0;
    double worst_min_mlps = 1e18, best_max_mlps = 0.0;
    for (int tid = 0; tid < T; ++tid) {
        auto st_secs = bench::compute(per_thread_runs[static_cast<std::size_t>(tid)]);
        auto st_mlps = bench::to_mlps(st_secs, per_thread);
        agg_median_mlps += st_mlps.median;
        if (st_mlps.min < worst_min_mlps) worst_min_mlps = st_mlps.min;
        if (st_mlps.max > best_max_mlps)  best_max_mlps  = st_mlps.max;
    }
    std::printf("  T=%d  per-thread min %6.2f  max %6.2f   aggregate median %7.2f   MLPS\n",
                T, worst_min_mlps, best_max_mlps, agg_median_mlps);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
            "usage: %s <fib-file> <trace-file> [runs] [threads=1,2,4,8]\n",
            argv[0]);
        return 1;
    }
    const int runs = (argc >= 4) ? std::atoi(argv[3]) : 20;
    std::vector<int> thread_list =
        (argc >= 5) ? parse_thread_list(argv[4])
                    : std::vector<int>{1, 2, 4, 8};

    std::vector<lpm6::Entry>    fib;
    std::vector<std::uint64_t>  trace;
    if (!lpm6::load_fib  (argv[1], fib))   return 1;
    if (!lpm6::load_trace(argv[2], trace)) return 1;

    bench::print_env();
    unsigned hw = std::thread::hardware_concurrency();
    std::printf("FIB: %zu prefixes   trace: %zu addresses   runs: %d   hardware_concurrency: %u\n",
                fib.size(), trace.size(), runs, hw);

    lpm6::Tree tree;
    {
        using clk = std::chrono::steady_clock;
        auto t0 = clk::now();
        tree.build(fib);
        std::printf("build %.3fs  depth=%d  total_keys=%u  edges=%zu\n\n",
                    std::chrono::duration<double>(clk::now() - t0).count(),
                    tree.depth(), tree.total_keys(), tree.edge_count());
    }

    std::printf("# throughput per thread-count (batch<%zu>, shared const Tree, threads pinned to CPU 0..T-1)\n",
                kBatch);
    for (int T : thread_list) {
        if (T < 1) continue;
        run_point(tree, trace, T, runs);
    }
    return 0;
}
