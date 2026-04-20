// patricia.hpp — path-compressed binary Patricia trie for IPv6 /0../64 LPM.
//
// This is a reference baseline for benchmarks, not part of the shipping
// library.  The implementation aims for correctness and clarity, not raw
// throughput; that's the point — it shows what a conventional pointer-
// chasing trie costs vs. the linearized B+-tree.
//
// Bit ordering: we treat the upper 64 bits of the IPv6 address as a
// big-endian bit string with bit 0 = MSB (matches the rest of lpm6).

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "lpm6.hpp"

namespace lpm6 {

class Patricia {
public:
    Patricia() : root_(std::make_unique<Node>()) {}

    void build(const std::vector<Entry>& fib) {
        root_ = std::make_unique<Node>();
        for (const auto& e : fib) {
            if (e.prefix_len < 0 || e.prefix_len > 64) continue;
            insert(e.prefix, e.prefix_len, e.next_hop);
        }
    }

    // Insert / replace a single prefix.
    void insert(std::uint64_t prefix, int len, int nh) {
        prefix = mask_to_len(prefix, len);
        Node* cur = root_.get();
        int   depth = 0;

        while (true) {
            // Common prefix length between `prefix` and `cur->prefix`, capped
            // to min(len, cur->prefix_len).
            int cap = std::min(len, cur->prefix_len);
            int common = common_prefix_len(prefix, cur->prefix, cap);

            if (common < cur->prefix_len) {
                // Current node disagrees with us somewhere inside its own
                // prefix span — split the node at `common`.
                auto  sibling       = std::make_unique<Node>();
                sibling->prefix     = cur->prefix;
                sibling->prefix_len = cur->prefix_len;
                sibling->next_hop   = cur->next_hop;
                sibling->child[0]   = std::move(cur->child[0]);
                sibling->child[1]   = std::move(cur->child[1]);

                cur->prefix     = mask_to_len(cur->prefix, common);
                cur->prefix_len = common;
                cur->next_hop   = -1;
                cur->child[0].reset();
                cur->child[1].reset();

                int sib_bit = bit_at(sibling->prefix, common);
                cur->child[sib_bit] = std::move(sibling);

                if (common == len) {
                    // The new prefix terminates exactly at the split point.
                    cur->next_hop = nh;
                } else {
                    int new_bit = bit_at(prefix, common);
                    cur->child[new_bit] = std::make_unique<Node>();
                    Node* leaf          = cur->child[new_bit].get();
                    leaf->prefix        = prefix;
                    leaf->prefix_len    = len;
                    leaf->next_hop      = nh;
                }
                return;
            }

            // We match `cur` fully up to cur->prefix_len.
            if (len == cur->prefix_len) {
                cur->next_hop = nh;  // insert or update
                return;
            }

            // len > cur->prefix_len: descend into the correct child.
            int b = bit_at(prefix, cur->prefix_len);
            if (!cur->child[b]) {
                cur->child[b] = std::make_unique<Node>();
                Node* leaf        = cur->child[b].get();
                leaf->prefix      = prefix;
                leaf->prefix_len  = len;
                leaf->next_hop    = nh;
                return;
            }
            cur   = cur->child[b].get();
            depth = cur->prefix_len;
            (void)depth;
        }
    }

    // Approximate heap footprint, counted by node.  Undercounts real RSS
    // because each unique_ptr does a separate malloc and allocators add
    // 16-32 bytes of bookkeeping per allocation; the RSS delta captures
    // that overhead, whereas this number tracks the algorithmic cost.
    std::size_t footprint_bytes() const { return count_nodes(root_.get()) * sizeof(Node); }

    int lookup(std::uint64_t addr) const {
        int   best  = 0;
        const Node* cur = root_.get();
        while (cur) {
            // Is `cur`'s prefix a prefix of `addr`?
            if (!prefix_matches(addr, cur->prefix, cur->prefix_len)) break;
            if (cur->next_hop >= 0) best = cur->next_hop;
            if (cur->prefix_len == 64) break;
            int b = bit_at(addr, cur->prefix_len);
            cur = cur->child[b].get();
        }
        return best;
    }

private:
    struct Node {
        std::uint64_t           prefix     = 0;   // bits above prefix_len are zero
        int                     prefix_len = 0;
        int                     next_hop   = -1;  // -1 = internal / branch only
        std::unique_ptr<Node>   child[2];
    };

    static std::uint64_t mask_to_len(std::uint64_t p, int len) {
        if (len <= 0)  return 0;
        if (len >= 64) return p;
        std::uint64_t m = (~std::uint64_t(0)) << (64 - len);
        return p & m;
    }

    static int bit_at(std::uint64_t p, int pos) {
        // pos 0 = MSB
        return int((p >> (63 - pos)) & 1u);
    }

    static int common_prefix_len(std::uint64_t a, std::uint64_t b, int cap) {
        if (cap <= 0) return 0;
        std::uint64_t diff = a ^ b;
        if (diff == 0) return cap;
        int lz = 0;
        while (lz < cap && ((diff >> (63 - lz)) & 1u) == 0) ++lz;
        return lz;
    }

    static std::size_t count_nodes(const Node* n) {
        if (!n) return 0;
        return 1 + count_nodes(n->child[0].get()) + count_nodes(n->child[1].get());
    }

    static bool prefix_matches(std::uint64_t addr,
                               std::uint64_t prefix, int len) {
        if (len <= 0)  return true;
        if (len >  64) return false;
        std::uint64_t m = (len == 64)
            ? ~std::uint64_t(0)
            : ((~std::uint64_t(0)) << (64 - len));
        return (addr & m) == (prefix & m);
    }

    std::unique_ptr<Node> root_;
};

} // namespace lpm6
