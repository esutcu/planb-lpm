// bench_update.cpp — measures rebuild / update latency for lpm6::Tree and
// lpm6::Dynamic.
//
// PlanB treats updates as batch rebuilds (see §3.6 of the paper): N pending
// prefix changes are coalesced into one full reconstruction, so the
// operationally relevant number is the rebuild time for a given FIB size.
//
//   ./bench_update fib.txt [runs]
//
// Reports:
//   * Tree::build() wall time distribution over `runs` repetitions
//   * Dynamic::insert() + Dynamic::remove() amortized cost (rebuild on each
//     call) so users can see what a "per-update" request costs in the
//     worst case vs. the paper's batched number.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#include "../src/ipv6_util.hpp"
#include "bench_stats.hpp"
#include "lpm6.hpp"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s fib.txt [runs]\n", argv[0]);
        return 2;
    }
    const int runs = (argc >= 3) ? std::atoi(argv[2]) : 20;

    std::vector<lpm6::Entry> fib;
    if (!lpm6::load_fib(argv[1], fib)) return 1;

    bench::print_env();
    std::printf("FIB: %zu prefixes   runs: %d\n\n", fib.size(), runs);

    // Rebuild timing: lpm6::Tree::build() from scratch.
    {
        auto secs = bench::time_runs(runs, [&] {
            lpm6::Tree t;
            t.build(fib);
            return t.edge_count();
        });
        auto stats = bench::compute(secs);
        std::printf("# tree rebuild (full build from scratch, 20 runs + warmup)\n");
        std::printf("  min %.1f   q1 %.1f   med %.1f   q3 %.1f   max %.1f   ms  (IQR %.2f)\n\n",
                    stats.min * 1e3, stats.q1 * 1e3, stats.median * 1e3,
                    stats.q3 * 1e3, stats.max * 1e3, (stats.q3 - stats.q1) * 1e3);
    }

    // Single-update latency via Dynamic: each insert/remove triggers a full
    // rebuild in the current implementation, so this is an upper bound on
    // what a naive "update one prefix now" call costs.  Paper's model is
    // batched, so the realistic per-update cost is (rebuild / batch_size).
    {
        lpm6::Dynamic dyn;
        dyn.load(fib);
        std::mt19937_64 rng(0xD1CE);
        const int n_ops = std::min(runs, 10);    // each op costs a rebuild

        std::vector<double> insert_sec(static_cast<std::size_t>(n_ops));
        std::vector<double> remove_sec(static_cast<std::size_t>(n_ops));
        using clk = std::chrono::steady_clock;
        for (int i = 0; i < n_ops; ++i) {
            const std::uint64_t p  = rng();
            const int           nh = 1 + int(rng() % 100);
            auto t0 = clk::now();
            dyn.insert(p, 48, nh);
            insert_sec[static_cast<std::size_t>(i)] =
                std::chrono::duration<double>(clk::now() - t0).count();
            auto t1 = clk::now();
            dyn.remove(p, 48);
            remove_sec[static_cast<std::size_t>(i)] =
                std::chrono::duration<double>(clk::now() - t1).count();
        }
        auto ins = bench::compute(insert_sec);
        auto rem = bench::compute(remove_sec);
        std::printf("# dynamic single-update (rebuild on each op, %d ops)\n", n_ops);
        std::printf("  insert  min %.1f   med %.1f   max %.1f   ms\n",
                    ins.min * 1e3, ins.median * 1e3, ins.max * 1e3);
        std::printf("  remove  min %.1f   med %.1f   max %.1f   ms\n",
                    rem.min * 1e3, rem.median * 1e3, rem.max * 1e3);
        std::printf("\n");
        std::printf("# amortized per-update under batching\n");
        std::printf("  if 1K updates coalesce into 1 rebuild : ~%.1f us/update\n",
                    ins.median * 1e6 / 1000.0);
        std::printf("  if 10K updates coalesce into 1 rebuild: ~%.2f us/update\n",
                    ins.median * 1e6 / 10000.0);
    }

    return 0;
}
