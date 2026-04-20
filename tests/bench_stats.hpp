// bench_stats.hpp — disciplined-measurement helpers for benchmarks.
//
// Provides: warmup-then-timed runs, per-run timing collection, distribution
// summary (min/q1/median/q3/max + IQR), and a small environment reporter
// (CPU affinity, CPU governor, Intel turbo state) so results are reproducible.
//
// Not part of the shipping library; used only by bench binaries.

#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#ifdef __linux__
#include <sched.h>
#include <unistd.h>
#endif

namespace bench {

struct Stats {
    double       min    = 0.0;
    double       q1     = 0.0;
    double       median = 0.0;
    double       q3     = 0.0;
    double       max    = 0.0;
    std::size_t  n      = 0;
};

inline Stats compute(std::vector<double> samples) {
    Stats s;
    s.n = samples.size();
    if (samples.empty()) return s;
    std::sort(samples.begin(), samples.end());
    auto pct = [&](double p) {
        double idx = p * double(s.n - 1);
        std::size_t lo = static_cast<std::size_t>(std::floor(idx));
        std::size_t hi = static_cast<std::size_t>(std::ceil(idx));
        if (hi >= s.n) hi = s.n - 1;
        double frac = idx - double(lo);
        return samples[lo] + frac * (samples[hi] - samples[lo]);
    };
    s.min    = samples.front();
    s.q1     = pct(0.25);
    s.median = pct(0.50);
    s.q3     = pct(0.75);
    s.max    = samples.back();
    return s;
}

// Convert per-run seconds stats into MLPS, given ops per run.
// Note: reciprocal swaps min<->max (min time = max throughput).
inline Stats to_mlps(const Stats& secs, std::size_t ops) {
    Stats m;
    m.n      = secs.n;
    auto cv  = [ops](double sec) { return (sec > 0.0) ? static_cast<double>(ops) / sec / 1e6 : 0.0; };
    m.min    = cv(secs.max);
    m.q1     = cv(secs.q3);
    m.median = cv(secs.median);
    m.q3     = cv(secs.q1);
    m.max    = cv(secs.min);
    return m;
}

inline void print_row(const char* label, const Stats& mlps) {
    std::printf("  %-12s  min %8.2f  q1 %8.2f  med %8.2f  q3 %8.2f  max %8.2f   MLPS  (n=%zu, IQR=%.2f)\n",
                label, mlps.min, mlps.q1, mlps.median, mlps.q3, mlps.max,
                mlps.n, mlps.q3 - mlps.q1);
}

// Resident set size in kilobytes, read from /proc/self/status.  Returns 0
// when the file is unavailable (e.g. WSL1, macOS, Windows).  Call it
// before and after a build to compute a real-memory delta; the per-
// structure `footprint_bytes()` methods give the algorithmic cost, this
// gives the allocator-inclusive cost.
inline long read_rss_kb() {
#ifdef __linux__
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            long kb = 0;
            // Parse "VmRSS:   12345 kB"
            for (std::size_t i = 6; i < line.size(); ++i) {
                if (line[i] >= '0' && line[i] <= '9') {
                    kb = kb * 10 + (line[i] - '0');
                }
            }
            return kb;
        }
    }
#endif
    return 0;
}

inline std::string read_trim(const char* path) {
    std::ifstream f(path);
    std::string s;
    if (!f) return {};
    std::getline(f, s);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
    return s;
}

inline void print_env() {
    std::printf("# environment\n");
#ifdef __linux__
    int cpu = sched_getcpu();
    cpu_set_t set;
    CPU_ZERO(&set);
    int n_in_mask = 0;
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        for (int i = 0; i < CPU_SETSIZE; ++i) if (CPU_ISSET(i, &set)) ++n_in_mask;
    }
    std::printf("  running_cpu   : %d\n", cpu);
    std::printf("  affinity_mask : %d cpu%s%s\n", n_in_mask,
                (n_in_mask == 1 ? "" : "s"),
                (n_in_mask == 1 ? "  (pinned)" : "  (not pinned — prefix with `taskset -c N`)"));

    std::string gov = read_trim("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor");
    if (!gov.empty()) {
        std::printf("  cpu0_governor : %s%s\n", gov.c_str(),
                    (gov == "performance") ? "" : "  (warn: consider `cpupower frequency-set -g performance`)");
    }
    std::string nt = read_trim("/sys/devices/system/cpu/intel_pstate/no_turbo");
    if (!nt.empty()) {
        std::printf("  no_turbo      : %s%s\n", nt.c_str(),
                    (nt == "1") ? "  (turbo disabled — steady clock)" : "  (turbo enabled — may introduce variance)");
    }
#else
    std::printf("  (env probe only implemented on linux)\n");
#endif
    std::printf("\n");
}

// Run `fn` once as warmup (result discarded), then `runs` timed runs.
// fn() must return a sink value; caller is expected to accumulate into a
// volatile to prevent DCE.
template <typename Fn>
std::vector<double> time_runs(int runs, Fn&& fn) {
    using clk = std::chrono::steady_clock;
    (void)fn();                         // warmup — primes cache + branch predictor
    std::vector<double> out(static_cast<std::size_t>(runs));
    for (int i = 0; i < runs; ++i) {
        auto t0 = clk::now();
        (void)fn();
        out[static_cast<std::size_t>(i)] =
            std::chrono::duration<double>(clk::now() - t0).count();
    }
    return out;
}

} // namespace bench
