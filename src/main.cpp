// main.cpp — disciplined benchmark for lpm6::Tree.
//
// Runs one warmup pass (discarded), then `runs` timed passes for:
//   * single lookup:  tree.lookup(addr)  for each address
//   * batch<M>:       tree.lookup_batch<M>(addrs, hops_out)
//                     for M in {1, 2, 4, 8, 16, 32}
//
// Reports min/q1/median/q3/max + IQR in MLPS.  Prints CPU affinity and
// governor so numbers are reproducible.  Pin to one core for clean numbers:
//
//     taskset -c 2 ./planb-lpm fib.txt trace.txt

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "ipv6_util.hpp"
#include "lpm6.hpp"
#include "../tests/bench_stats.hpp"

namespace {

template <std::size_t M>
void bench_batchM(const lpm6::Tree& tree,
                  const std::vector<std::uint64_t>& trace,
                  int runs) {
    if (trace.size() < M) {
        std::printf("  batch<%-3zu>   : skipped (trace has <%zu addresses)\n", M, M);
        return;
    }
    const std::size_t lim = (trace.size() / M) * M;
    auto secs = bench::time_runs(runs, [&] {
        int hops_out[M];
        volatile std::uint64_t sink = 0;
        for (std::size_t j = 0; j + M <= lim; j += M) {
            tree.lookup_batch<M>(&trace[j], hops_out);
            for (std::size_t k = 0; k < M; ++k)
                sink += static_cast<unsigned>(hops_out[k]);
        }
        return sink;
    });
    char label[24];
    std::snprintf(label, sizeof(label), "batch<%zu>", M);
    bench::print_row(label, bench::to_mlps(bench::compute(secs), lim));
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
            "usage: %s <fib-file> <trace-file> [runs]\n"
            "  fib-file:   lines of  \"<ipv6>/<len> <next_hop>\"\n"
            "  trace-file: one IPv6 address per line\n"
            "  runs:       timed runs per path (default 20, warmup extra)\n"
            "\n"
            "  Pin to a single core for clean numbers:\n"
            "    taskset -c 2 %s fib.txt trace.txt\n",
            argv[0], argv[0]);
        return 1;
    }
    const int runs = (argc >= 4) ? std::atoi(argv[3]) : 20;

    std::vector<lpm6::Entry>    fib;
    std::vector<std::uint64_t>  trace;
    if (!lpm6::load_fib  (argv[1], fib))   return 1;
    if (!lpm6::load_trace(argv[2], trace)) return 1;

    bench::print_env();
    std::printf("FIB: %zu prefixes   trace: %zu addresses   runs: %d\n",
                fib.size(), trace.size(), runs);

    lpm6::Tree tree;
    {
        using clk = std::chrono::steady_clock;
        auto t0 = clk::now();
        tree.build(fib);
        std::printf("build %.3fs  depth=%d  total_keys=%u  edges=%zu\n\n",
                    std::chrono::duration<double>(clk::now() - t0).count(),
                    tree.depth(), tree.total_keys(), tree.edge_count());
    }

    std::printf("# throughput (first run is warmup, discarded)\n");

    // --- single lookup (non-batched entry point) ---
    {
        auto secs = bench::time_runs(runs, [&] {
            volatile std::uint64_t sink = 0;
            for (auto a : trace) sink += static_cast<unsigned>(tree.lookup(a));
            return sink;
        });
        bench::print_row("single", bench::to_mlps(bench::compute(secs), trace.size()));
    }

    // --- batch<M> sweep on the real API (returns hops, same work as single) ---
    bench_batchM<1> (tree, trace, runs);
    bench_batchM<2> (tree, trace, runs);
    bench_batchM<4> (tree, trace, runs);
    bench_batchM<8> (tree, trace, runs);
    bench_batchM<16>(tree, trace, runs);
    bench_batchM<32>(tree, trace, runs);

    return 0;
}
