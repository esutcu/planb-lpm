#!/usr/bin/env python3
# mrt_to_fib.py — minimal MRT (RFC 6396) → planb-lpm FIB converter.
#
# Reads a TABLE_DUMP_V2 / RIB_IPV6_UNICAST dump (as produced by RIPE RIS,
# e.g. http://data.ris.ripe.net/rrc00/latest-bview.gz) and writes lines of
#
#     <ipv6-prefix>/<len> <next-hop>
#
# Next-hop is synthesised as a sequence number; the benchmark only needs the
# prefix set, not the actual AS path.  Plain and gzip input both accepted.
#
# Usage:
#     python3 mrt_to_fib.py <input.mrt[.gz]> <output.txt>
#
# Only the pieces we need are implemented:
#   * MRT common header parsing
#   * TABLE_DUMP_V2 subtypes: 1 (PEER_INDEX_TABLE, skipped), 4 (RIB_IPV6_UNICAST)
#   * IPv6 prefix decoding (variable-length, zero-padded to 16 bytes)
#
# Anything else (TABLE_DUMP, BGP4MP, RIB_IPV4_*) is silently skipped — so the
# same script works on mixed-AFI dumps and on files that contain a peer-index
# preamble but then carry only IPv4 RIB entries.

import gzip
import ipaddress
import struct
import sys

MRT_TYPE_TABLE_DUMP_V2 = 13
SUBTYPE_RIB_IPV6_UNICAST = 4


def open_maybe_gz(path):
    """Return a binary file object, transparently gunzipping .gz files."""
    if path.endswith(".gz"):
        return gzip.open(path, "rb")
    return open(path, "rb")


def decode_prefix_v6(plen: int, raw: bytes) -> str:
    """Pad `raw` (ceil(plen/8) bytes) out to 16 and render as an IPv6 string."""
    padded = raw + b"\x00" * (16 - len(raw))
    return str(ipaddress.IPv6Address(padded))


def iter_rib_ipv6_records(fp):
    """Yield (prefix_str, plen) tuples for every RIB_IPV6_UNICAST entry."""
    header_fmt = ">IHHI"  # timestamp, type, subtype, length
    header_size = struct.calcsize(header_fmt)

    while True:
        header = fp.read(header_size)
        if not header:
            return
        if len(header) < header_size:
            raise ValueError("truncated MRT header")

        _, mtype, subtype, length = struct.unpack(header_fmt, header)
        body = fp.read(length)
        if len(body) < length:
            raise ValueError("truncated MRT body")

        if mtype != MRT_TYPE_TABLE_DUMP_V2:
            continue
        if subtype != SUBTYPE_RIB_IPV6_UNICAST:
            continue

        # RIB_IPV6_UNICAST body:
        #   sequence(4) | plen(1) | prefix(ceil(plen/8)) | entry_count(2) | entries...
        if len(body) < 7:
            continue
        plen = body[4]
        pbytes = (plen + 7) // 8
        if len(body) < 5 + pbytes + 2:
            continue
        prefix_raw = body[5 : 5 + pbytes]
        # We only want the prefix itself; RIB entries (attributes, AS path,
        # next-hop) are ignored — we synthesise a next-hop below.
        yield decode_prefix_v6(plen, prefix_raw), plen


def main(argv):
    if len(argv) != 3:
        sys.stderr.write(
            "usage: mrt_to_fib.py <input.mrt[.gz]> <output.txt>\n"
        )
        return 1

    in_path, out_path = argv[1], argv[2]
    seen = {}  # (prefix, plen) -> next_hop
    next_hop = 1

    with open_maybe_gz(in_path) as fp:
        for prefix, plen in iter_rib_ipv6_records(fp):
            key = (prefix, plen)
            if key in seen:
                continue
            seen[key] = next_hop
            next_hop += 1

    with open(out_path, "w") as out:
        for (prefix, plen), nh in seen.items():
            out.write(f"{prefix}/{plen} {nh}\n")

    sys.stderr.write(f"wrote {len(seen)} unique IPv6 prefixes to {out_path}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
