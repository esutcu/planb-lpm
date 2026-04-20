// bench_naive.cpp — disciplined throughput comparison:
//   * linear scan         — theoretical worst case (O(N) per lookup)
//   * Patricia radix trie — conventional pointer-chasing LPM
//   * lpm6::Tree          — this library
//
// Measurement discipline:
//   - one warmup pass per structure (discarded)
//   - 20 timed runs per structure (fewer for naive; see below)
//   - reports min / q1 / median / q3 / max + IQR in MLPS
//   - prints CPU affinity + governor + turbo state so numbers are reproducible
//
// Pin to a single core to cut scheduler noise:
//   taskset -c 2 ./bench_naive fib.txt trace.txt
//
// Not a scientific benchmark; it's a disciplined sanity check against a
// conventional trie baseline.  For the paper's baselines (PopTrie, CP-Trie,
// Neurotrie, HBS) see the evaluation plan.

#include <cstdio>
#include <cstdlib>
#include <vector>

#include "../src/ipv6_util.hpp"
#include "bench_stats.hpp"
#include "lpm6.hpp"
#include "patricia.hpp"

namespace {

int lpm_naive(const std::vector<lpm6::Entry>& fib, std::uint64_t addr) {
    int best_len = -1;
    int best_nh  = 0;
    for (const auto& e : fib) {
        std::uint64_t mask = (e.prefix_len == 0)
            ? std::uint64_t(0)
            : (~std::uint64_t(0)) << (64 - e.prefix_len);
        if ((addr & mask) == (e.prefix & mask) && e.prefix_len > best_len) {
            best_len = e.prefix_len;
            best_nh  = e.next_hop;
        }
    }
    return best_nh;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
            "usage: %s fib.txt trace.txt [runs]\n"
            "  runs : timed runs per implementation (default 20; naive uses min(runs,3))\n"
            "\n"
            "  Pin to a single core for clean numbers:\n"
            "    taskset -c 2 %s fib.txt trace.txt\n",
            argv[0], argv[0]);
        return 2;
    }
    const int runs = (argc >= 4) ? std::atoi(argv[3]) : 20;

    std::vector<lpm6::Entry>   fib;
    std::vector<std::uint64_t> trace;
    if (!lpm6::load_fib  (argv[1], fib))   return 1;
    if (!lpm6::load_trace(argv[2], trace)) return 1;

    bench::print_env();
    std::printf("FIB: %zu prefixes   trace: %zu addresses   runs: %d\n\n",
                fib.size(), trace.size(), runs);

    auto mb = [](std::size_t bytes) { return double(bytes) / (1024.0 * 1024.0); };

    const long rss_0 = bench::read_rss_kb();
    lpm6::Tree tree;
    {
        using clk = std::chrono::steady_clock;
        auto t0 = clk::now();
        tree.build(fib);
        std::printf("  tree  build    : %.3fs  footprint %.2f MB  (depth=%d  total_keys=%u  edges=%zu)\n",
                    std::chrono::duration<double>(clk::now() - t0).count(),
                    mb(tree.footprint_bytes()),
                    tree.depth(), tree.total_keys(), tree.edge_count());
    }
    const long rss_1 = bench::read_rss_kb();

    lpm6::Patricia pat;
    {
        using clk = std::chrono::steady_clock;
        auto t0 = clk::now();
        pat.build(fib);
        std::printf("  patricia build : %.3fs  footprint %.2f MB\n",
                    std::chrono::duration<double>(clk::now() - t0).count(),
                    mb(pat.footprint_bytes()));
    }
    const long rss_2 = bench::read_rss_kb();

    if (rss_0 > 0) {
        std::printf("  RSS delta      : tree +%.2f MB   patricia +%.2f MB   (process total %.2f MB)\n",
                    double(rss_1 - rss_0) / 1024.0,
                    double(rss_2 - rss_1) / 1024.0,
                    double(rss_2) / 1024.0);
    }

    std::printf("\n# throughput (first run is warmup, discarded)\n");

    // --- tree ---
    {
        auto secs = bench::time_runs(runs, [&] {
            volatile std::uint64_t sink = 0;
            for (auto a : trace) sink += static_cast<unsigned>(tree.lookup(a));
            return sink;
        });
        bench::print_row("tree", bench::to_mlps(bench::compute(secs), trace.size()));
    }

    // --- patricia ---
    {
        auto secs = bench::time_runs(runs, [&] {
            volatile std::uint64_t sink = 0;
            for (auto a : trace) sink += static_cast<unsigned>(pat.lookup(a));
            return sink;
        });
        bench::print_row("patricia", bench::to_mlps(bench::compute(secs), trace.size()));
    }

    // --- naive: O(N) per lookup, so walk a slice only and use fewer runs ---
    {
        const std::size_t slice = std::min<std::size_t>(trace.size(), 50000);
        const int naive_runs = std::min(runs, 3);
        auto secs = bench::time_runs(naive_runs, [&] {
            volatile std::uint64_t sink = 0;
            for (std::size_t i = 0; i < slice; ++i)
                sink += static_cast<unsigned>(lpm_naive(fib, trace[i]));
            return sink;
        });
        bench::print_row("naive(50K)", bench::to_mlps(bench::compute(secs), slice));
    }

    return 0;
}
