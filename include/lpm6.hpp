// lpm6.hpp — IPv6 longest-prefix-match via linearized B+-tree
//
// Algorithm follows the PlanB paper (arxiv:2604.14650, NSDI '26) by Zhang et al.,
// reimplemented as a portable header-only C++17 library with runtime fallback
// when AVX-512 is unavailable and a lock-free dynamic update path.

#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <new>
#include <stack>
#include <stdexcept>
#include <utility>
#include <vector>

#if defined(__AVX512F__) && defined(__AVX512DQ__)
  #include <immintrin.h>
  #define LPM6_HAS_AVX512 1
#else
  #define LPM6_HAS_AVX512 0
#endif

namespace lpm6 {

inline void* aligned_alloc64(std::size_t bytes) {
#if defined(_WIN32)
    return _aligned_malloc(bytes, 64);
#else
    void* p = nullptr;
    if (posix_memalign(&p, 64, bytes) != 0) return nullptr;
    return p;
#endif
}

inline void aligned_free(void* p) {
#if defined(_WIN32)
    _aligned_free(p);
#else
    std::free(p);
#endif
}

// B+-tree parameters. Each node holds FANOUT=8 sorted 64-bit keys, yielding a
// 9-ary tree whose node fits one 64-byte cache line. Keys are stored breadth-
// first across levels, the whole tree living in one flat aligned array.
constexpr int FANOUT = 8;
constexpr int ARITY  = FANOUT + 1;

// LEVEL_SIZE[i] = number of keys at level i = FANOUT * ARITY^i
constexpr std::uint32_t LEVEL_SIZE[7] = {
    8, 72, 648, 5832, 52488, 472392, 4251528
};
// LEVEL_START[i] = first key index of level i = sum of LEVEL_SIZE[0..i-1]
constexpr std::uint32_t LEVEL_START[8] = {
    0, 8, 80, 728, 6560, 59048, 531440, 4782968
};

/**
 * @brief A single FIB entry.
 *
 * Only the upper 64 bits of an IPv6 address are stored; prefix lengths up
 * to /64 are supported (longer prefixes are silently dropped at build
 * time, matching the scope of the PlanB paper).
 */
struct Entry {
    std::uint64_t prefix;      ///< Upper 64 bits of the prefix, zero-padded on the right.
    int           prefix_len;  ///< Prefix length, 0..64 inclusive.
    int           next_hop;    ///< Opaque next-hop identifier returned by lookup.
};

// How many of the 8 keys at `base` are <= x.  This is the inner search step
// of a B+-tree node: with B=8 the popcount of the GE-mask is exactly the
// child index to descend into.
inline std::uint32_t count_ge(const std::uint64_t* base, std::uint64_t x) {
#if LPM6_HAS_AVX512
    __m512i vx  = _mm512_set1_epi64(static_cast<long long>(x));
    __m512i vk  = _mm512_loadu_si512(base);
    __mmask8 m  = _mm512_cmp_epu64_mask(vx, vk, _MM_CMPINT_GE);
    return static_cast<std::uint32_t>(__builtin_popcount(m));
#else
    std::uint32_t c = 0;
    for (int i = 0; i < FANOUT; ++i) c += (x >= base[i]);
    return c;
#endif
}

// Flat-array child position: parent at absolute index `node` (multiple of
// FANOUT), child c in [0, ARITY).
inline std::uint32_t child_pos(std::uint32_t node, std::uint32_t c) {
    return ARITY * node + (c + 1) * FANOUT;
}

/**
 * @brief Static longest-prefix-match tree over IPv6 /0../64 prefixes.
 *
 * After build() has been called once, lookup() is thread-safe and
 * wait-free; concurrent rebuilds are not supported — use Dynamic for
 * that.  The tree holds a 64-byte-aligned flat key array; copying or
 * assigning a Tree is disabled to avoid accidental deep copies.
 */
class Tree {
public:
    Tree() = default;
    Tree(const Tree&)            = delete;
    Tree& operator=(const Tree&) = delete;
    ~Tree() { if (keys_) aligned_free(keys_); }

    /**
     * @brief Build the tree from a FIB.
     *
     * Entries with prefix_len outside [0,64] are silently skipped, so the
     * caller does not have to pre-filter.  The input vector may contain
     * duplicate or overlapping prefixes; longer prefixes win at lookup.
     *
     * @param fib  Flat list of FIB entries.  Order does not matter.
     * @throws std::runtime_error if the resulting edge set would not fit in
     *         the 7-level tree (roughly >2.1 M unique prefixes).
     * @throws std::bad_alloc on allocation failure.
     */
    void build(const std::vector<Entry>& fib) {
        // 1. Expand each (prefix,len) into a [start,end) pair of interval
        //    edges.  Each prefix contributes 2 edges, so worst-case leaf
        //    usage is 2*fib.size().
        using Edge = std::pair<std::pair<std::uint64_t,int>, int>;
        std::vector<Edge> edges;
        edges.reserve(fib.size() * 2);

        for (const auto& f : fib) {
            if (f.prefix_len < 0 || f.prefix_len > 64) continue;
            std::uint64_t start = f.prefix;
            std::uint64_t end;
            if (f.prefix_len == 0)       end = ~std::uint64_t(0);
            else if (f.prefix_len == 64) end = start + 1;
            else                         end = start + (std::uint64_t(1) << (64 - f.prefix_len));
            edges.push_back({{start, f.prefix_len},  f.next_hop});
            edges.push_back({{end,   f.prefix_len}, -1});
        }

        std::sort(edges.begin(), edges.end());

        // 2. Sweep to resolve overlaps: stack holds currently-active next-hops
        //    in nesting order; end-edges inherit the enclosing route.
        {
            std::stack<int> active;
            for (auto& e : edges) {
                if (e.second >= 0) {
                    active.push(e.second);
                } else {
                    if (active.empty()) continue;  // defensive, shouldn't happen
                    active.pop();
                    e.second = active.empty() ? -1 : active.top();
                }
            }
        }

        // 3. Pick tree depth that fits edges at the leaf level.
        depth_ = -1;
        for (int i = 0; i < 7; ++i) {
            if (LEVEL_SIZE[i] >= edges.size()) { depth_ = i + 1; break; }
        }
        if (depth_ < 0) {
            throw std::runtime_error("lpm6: FIB too large for 7-level tree");
        }

        // 4. Allocate the flat key array.
        if (keys_) { aligned_free(keys_); keys_ = nullptr; }
        total_ = LEVEL_START[depth_];
        keys_  = static_cast<std::uint64_t*>(
                    aligned_alloc64(total_ * sizeof(std::uint64_t)));
        if (!keys_) throw std::bad_alloc();

        const std::uint32_t leaf_start = LEVEL_START[depth_ - 1];
        const std::uint32_t leaf_size  = LEVEL_SIZE [depth_ - 1];

        // 5. Fill leaves.  `hops_[j]` is the next-hop active starting at
        //    edges[j]; sentinel positions get ~0 so count_ge naturally
        //    ignores them.
        hops_.assign(edges.size(), -1);
        std::size_t count = std::min(edges.size(), std::size_t(leaf_size));
        for (std::size_t j = 0; j < count; ++j) {
            keys_[leaf_start + j] = edges[j].first.first;
            hops_[j]              = edges[j].second;
        }
        for (std::uint32_t i = leaf_start + static_cast<std::uint32_t>(count);
             i < total_; ++i) {
            keys_[i] = ~std::uint64_t(0);
        }

        // 6. Build internal levels bottom-up.  At level L each node's 8 keys
        //    are separators copied from the leaf level at stride G*j.
        int G = FANOUT;
        for (int level = depth_ - 1; level > 0; --level) {
            std::uint32_t pos = LEVEL_START[level - 1];
            for (int i = 0; i < int(leaf_size); i += G * ARITY) {
                for (int j = 1; j <= FANOUT; ++j) {
                    std::uint32_t src = leaf_start + i + G * j;
                    if (src < total_ && pos < total_) keys_[pos++] = keys_[src];
                }
            }
            G *= ARITY;
        }
    }

    /**
     * @brief Look up the next-hop for one IPv6 address (upper 64 bits).
     *
     * Wait-free and safe to call concurrently on a single built tree.
     *
     * @param addr  Upper 64 bits of the IPv6 address to look up.
     * @return The next-hop of the longest matching prefix, or 0 if no
     *         prefix matches (i.e. "drop / no route").
     */
    int lookup(std::uint64_t addr) const {
        if (depth_ < 0 || hops_.empty()) return 0;
        std::uint32_t node = 0;
        for (int i = depth_ - 1; i > 0; --i) {
            node = child_pos(node, count_ge(keys_ + node, addr));
        }
        std::uint32_t c          = count_ge(keys_ + node, addr);
        std::uint32_t leaf_start = LEVEL_START[depth_ - 1];
        // Total count of edges <= addr equals (node + c) - leaf_start; the
        // predecessor's hop lives at index (count - 1).
        int idx = static_cast<int>(node + c) - static_cast<int>(leaf_start) - 1;
        if (idx < 0) return 0;
        if (std::size_t(idx) >= hops_.size()) idx = int(hops_.size()) - 1;
        int nh = hops_[idx];
        return nh < 0 ? 0 : nh;
    }

    /**
     * @brief Batched lookup of M addresses in parallel.
     *
     * On AVX-512 hardware this is typically 2-3× the throughput of single
     * lookup; on the scalar fallback the two paths perform similarly.  M
     * is a compile-time constant so the compiler can fully unroll the
     * inner loop — common values are 8, 16, 32.
     *
     * @tparam M         Batch size.
     * @param  addrs     Pointer to M input addresses.
     * @param  hops_out  Pointer to M output slots; overwritten in place.
     */
    template <std::size_t M>
    void lookup_batch(const std::uint64_t* addrs, int* hops_out) const {
        if (depth_ < 0 || hops_.empty()) {
            for (std::size_t j = 0; j < M; ++j) hops_out[j] = 0;
            return;
        }
        std::uint32_t nodes[M]{};
        for (int i = depth_ - 1; i > 0; --i) {
            #pragma GCC unroll 16
            for (std::size_t j = 0; j < M; ++j) {
                nodes[j] = child_pos(nodes[j], count_ge(keys_ + nodes[j], addrs[j]));
            }
        }
        const std::uint32_t leaf_start = LEVEL_START[depth_ - 1];
        const int           hops_sz    = static_cast<int>(hops_.size());
        for (std::size_t j = 0; j < M; ++j) {
            std::uint32_t c = count_ge(keys_ + nodes[j], addrs[j]);
            int idx = static_cast<int>(nodes[j] + c)
                    - static_cast<int>(leaf_start) - 1;
            if (idx < 0)           { hops_out[j] = 0; continue; }
            if (idx >= hops_sz)    idx = hops_sz - 1;
            int nh = hops_[idx];
            hops_out[j] = nh < 0 ? 0 : nh;
        }
    }

    // Benchmark helper: same traversal path but returns a checksum so the
    // optimizer cannot dead-code the inner loop.
    template <std::size_t M>
    __attribute__((always_inline))
    std::uint64_t lookup_batch_checksum(const std::uint64_t* addrs) const {
        if (depth_ < 0) return 0;
        std::uint32_t nodes[M]{};
        #pragma GCC unroll 16
        for (int i = depth_ - 1; i > 0; --i) {
            #pragma GCC unroll 16
            for (std::size_t j = 0; j < M; ++j) {
                nodes[j] = child_pos(nodes[j], count_ge(keys_ + nodes[j], addrs[j]));
            }
        }
        std::uint64_t sum = 0;
        #pragma GCC unroll 16
        for (std::size_t j = 0; j < M; ++j) {
            nodes[j] += count_ge(keys_ + nodes[j], addrs[j]);
            sum += nodes[j];
        }
        return sum;
    }

    /// @return Tree depth (number of levels).  -1 if build() has not been called.
    int           depth()      const { return depth_; }
    /// @return Total number of 64-bit keys stored in the flat key array.
    std::uint32_t total_keys() const { return total_; }
    /// @return Number of resolved interval edges (roughly 2× unique prefixes).
    std::size_t   edge_count() const { return hops_.size(); }

    /**
     * @brief Approximate heap footprint of the lookup structure, in bytes.
     *
     * Counts the flat 64-byte-aligned key array plus the next-hop array.
     * Does not include allocator metadata or `std::vector` capacity slack
     * beyond its `size()`; use @c bench::read_rss_kb for process-level
     * memory measurement.
     */
    std::size_t footprint_bytes() const {
        return std::size_t(total_) * sizeof(std::uint64_t)
             + hops_.size() * sizeof(int);
    }

private:
    std::uint64_t*    keys_  = nullptr;
    std::vector<int>  hops_;
    std::uint32_t     total_ = 0;
    int               depth_ = -1;
};

/**
 * @brief Dynamic FIB with wait-free lookup and rebuild-and-swap updates.
 *
 * Implements the paper's rebuild-and-swap model: lookups load a shared
 * atomic snapshot and are strictly wait-free; insert/remove/update build
 * a fresh Tree in-place and swap the pointer atomically.  The old tree
 * is freed automatically when the last concurrent reader drops its
 * shared_ptr.
 *
 * Concurrent readers are fully supported.  Concurrent writers are **not**
 * — external synchronization (a mutex or single writer thread) is
 * required if updates can race with each other.  Each write rebuilds
 * the whole tree, which is O(N log N) in the FIB size; the model is
 * aimed at control-plane update rates, not per-packet writes.
 */
class Dynamic {
public:
    Dynamic() : tree_(std::make_shared<Tree>()) {}

    /// @brief Replace the FIB with @p fib and rebuild the tree.
    void load(const std::vector<Entry>& fib) {
        fib_.clear();
        for (const auto& e : fib) {
            if (e.prefix_len < 0 || e.prefix_len > 64) continue;
            fib_[{e.prefix, e.prefix_len}] = e.next_hop;
        }
        rebuild();
    }

    /// @brief Insert or replace a prefix.  Silently no-ops for invalid lengths.
    void insert(std::uint64_t prefix, int len, int next_hop) {
        if (len < 0 || len > 64) return;
        fib_[{prefix, len}] = next_hop;
        rebuild();
    }

    /// @brief Remove a prefix.  @return true if the prefix was present.
    bool remove(std::uint64_t prefix, int len) {
        if (fib_.erase({prefix, len}) == 0) return false;
        rebuild();
        return true;
    }

    /// @brief Change the next-hop of an existing prefix.
    /// @return false if the prefix was not present; the FIB is left unchanged.
    bool update(std::uint64_t prefix, int len, int next_hop) {
        auto it = fib_.find({prefix, len});
        if (it == fib_.end()) return false;
        it->second = next_hop;
        rebuild();
        return true;
    }

    /// @brief Wait-free lookup against the current snapshot.
    int lookup(std::uint64_t addr) const {
        auto t = std::atomic_load_explicit(&tree_, std::memory_order_acquire);
        return t->lookup(addr);
    }

    /// @return Number of prefixes currently in the FIB.
    std::size_t size() const { return fib_.size(); }

private:
    void rebuild() {
        std::vector<Entry> fib;
        fib.reserve(fib_.size());
        for (const auto& kv : fib_) {
            fib.push_back({kv.first.first, kv.first.second, kv.second});
        }
        auto next = std::make_shared<Tree>();
        next->build(fib);
        std::atomic_store_explicit(&tree_, next, std::memory_order_release);
    }

    std::map<std::pair<std::uint64_t,int>, int> fib_;
    std::shared_ptr<Tree>                        tree_;
};

} // namespace lpm6
