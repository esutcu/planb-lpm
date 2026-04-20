// ipv6_util.hpp — shared IPv6 parsing + FIB/trace loading helpers used by
// main.cpp, the test binary, and the Python binding.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "lpm6.hpp"

#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "Ws2_32.lib")
#else
  #include <arpa/inet.h>
#endif

namespace lpm6 {

// Parse an IPv6 literal (optionally with "/len" suffix, which is ignored here)
// and return its upper 64 bits.  Returns UINT64_MAX on parse failure.
inline std::uint64_t parse_ipv6(const char* s) {
    if (!s) return ~std::uint64_t(0);
    char buf[64];
    std::size_t n = 0;
    while (s[n] && s[n] != '/' && n < sizeof(buf) - 1) { buf[n] = s[n]; ++n; }
    buf[n] = '\0';
    struct in6_addr addr;
    if (inet_pton(AF_INET6, buf, &addr) != 1) return ~std::uint64_t(0);
    std::uint64_t hi = 0;
    for (int i = 0; i < 8; ++i) hi = (hi << 8) | addr.s6_addr[i];
    return hi;
}

inline int parse_prefix_len(const char* s) {
    const char* slash = std::strchr(s, '/');
    if (!slash) return -1;
    return std::atoi(slash + 1);
}

// FIB file format: one entry per line,  "<ipv6>/<len> <next_hop>".
// Prefixes with len > 64 are skipped (upper 64 bits only).
inline bool load_fib(const char* path, std::vector<Entry>& out) {
    std::FILE* f = std::fopen(path, "r");
    if (!f) { std::fprintf(stderr, "cannot open FIB: %s\n", path); return false; }
    char word[256];
    int  nh = 0;
    while (std::fscanf(f, "%255s %d", word, &nh) == 2) {
        int len = parse_prefix_len(word);
        if (len < 0 || len > 64) continue;
        std::uint64_t p = parse_ipv6(word);
        if (p == ~std::uint64_t(0)) continue;
        out.push_back({p, len, nh});
    }
    std::fclose(f);
    return true;
}

// Trace file format: one IPv6 address per line.
inline bool load_trace(const char* path, std::vector<std::uint64_t>& out) {
    std::FILE* f = std::fopen(path, "r");
    if (!f) { std::fprintf(stderr, "cannot open trace: %s\n", path); return false; }
    char line[256];
    while (std::fgets(line, sizeof(line), f)) {
        line[std::strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0') continue;
        std::uint64_t a = parse_ipv6(line);
        if (a != ~std::uint64_t(0)) out.push_back(a);
    }
    std::fclose(f);
    return true;
}

} // namespace lpm6
