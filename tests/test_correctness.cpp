// test_correctness.cpp — brute-force LPM reference vs lpm6::Tree.
//
// For each test address we compute the longest-prefix-match directly by
// scanning the FIB and comparing masked prefixes, then we compare against
// lpm6::Tree::lookup.  Any mismatch is reported and the test fails.

#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#include "../src/ipv6_util.hpp"
#include "lpm6.hpp"
#include "patricia.hpp"

namespace {

int lpm_brute(const std::vector<lpm6::Entry>& fib, std::uint64_t addr) {
    int best_len = -1;
    int best_nh  = 0;
    for (const auto& e : fib) {
        if (e.prefix_len < 0 || e.prefix_len > 64) continue;
        std::uint64_t mask = (e.prefix_len == 0)
            ? std::uint64_t(0)
            : (~std::uint64_t(0)) << (64 - e.prefix_len);
        if ((addr & mask) == (e.prefix & mask)) {
            if (e.prefix_len > best_len) { best_len = e.prefix_len; best_nh = e.next_hop; }
        }
    }
    return best_nh;
}

std::vector<lpm6::Entry> make_synthetic_fib(std::mt19937_64& rng, int count) {
    std::vector<lpm6::Entry> fib;
    fib.reserve(count + 1);
    // Default route so "no match" never happens in synthetic tests.
    fib.push_back({0, 0, 1});
    for (int i = 0; i < count; ++i) {
        int len = 16 + int(rng() % 49);                  // 16..64
        std::uint64_t mask = (len == 0)
            ? std::uint64_t(0)
            : (~std::uint64_t(0)) << (64 - len);
        std::uint64_t p = rng() & mask;
        int nh = 2 + int(rng() % 250);
        fib.push_back({p, len, nh});
    }
    return fib;
}

int run_case(const char* name, std::mt19937_64& rng,
             int fib_size, int trace_size) {
    auto fib   = make_synthetic_fib(rng, fib_size);
    std::vector<std::uint64_t> trace(trace_size);
    for (auto& a : trace) a = rng();

    lpm6::Tree tree;
    tree.build(fib);

    lpm6::Patricia pat;
    pat.build(fib);

    int mismatches      = 0;
    int pat_mismatches  = 0;
    for (std::size_t i = 0; i < trace.size(); ++i) {
        int got = tree.lookup(trace[i]);
        int exp = lpm_brute(fib, trace[i]);
        int gotp = pat.lookup(trace[i]);
        if (got != exp) {
            if (mismatches < 5) {
                std::printf("  mismatch %zu: addr=%016llx got=%d exp=%d\n",
                            i, (unsigned long long)trace[i], got, exp);
            }
            ++mismatches;
        }
        if (gotp != exp) {
            if (pat_mismatches < 5) {
                std::printf("  pat_mismatch %zu: addr=%016llx got=%d exp=%d\n",
                            i, (unsigned long long)trace[i], gotp, exp);
            }
            ++pat_mismatches;
        }
    }

    // Batch path: ensure it agrees with single-lookup output.
    constexpr std::size_t M = 8;
    int batch_mismatches = 0;
    for (std::size_t j = 0; j + M <= trace.size(); j += M) {
        int out[M];
        tree.lookup_batch<M>(&trace[j], out);
        for (std::size_t k = 0; k < M; ++k) {
            int exp = lpm_brute(fib, trace[j + k]);
            if (out[k] != exp) ++batch_mismatches;
        }
    }

    std::printf("[%s] fib=%d trace=%d  single=%d  batch=%d  patricia=%d\n",
                name, fib_size, trace_size,
                mismatches, batch_mismatches, pat_mismatches);
    return mismatches + batch_mismatches + pat_mismatches;
}

int test_dynamic() {
    lpm6::Dynamic dyn;
    dyn.load({{0, 0, 1}});
    if (dyn.lookup(0x1234'5678'9abc'def0ULL) != 1) return 1;

    dyn.insert(0x2001'0db8'0000'0000ULL, 32, 7);
    if (dyn.lookup(0x2001'0db8'aaaa'bbbbULL) != 7) return 1;
    if (dyn.lookup(0x3000'0000'0000'0000ULL) != 1) return 1;

    dyn.insert(0x2001'0db8'0001'0000ULL, 48, 9);
    if (dyn.lookup(0x2001'0db8'0001'beefULL) != 9) return 1;
    if (dyn.lookup(0x2001'0db8'0002'0000ULL) != 7) return 1;

    if (!dyn.remove(0x2001'0db8'0001'0000ULL, 48)) return 1;
    if (dyn.lookup(0x2001'0db8'0001'beefULL) != 7) return 1;

    if (!dyn.update(0x2001'0db8'0000'0000ULL, 32, 42)) return 1;
    if (dyn.lookup(0x2001'0db8'aaaa'bbbbULL) != 42) return 1;

    std::printf("[dynamic] insert/remove/update/lookup: ok\n");
    return 0;
}

} // namespace

int main() {
    std::mt19937_64 rng(0xC0FFEEULL);
    int fails = 0;
    fails += run_case("tiny",   rng,    5,   200);
    fails += run_case("small",  rng,   50,  2000);
    fails += run_case("medium", rng,  500,  5000);
    fails += run_case("large",  rng, 5000, 10000);
    fails += test_dynamic();
    if (fails == 0) {
        std::printf("\nall tests passed\n");
        return 0;
    }
    std::printf("\n%d failures\n", fails);
    return 1;
}
