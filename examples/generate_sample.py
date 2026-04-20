#!/usr/bin/env python3
"""Generate a small synthetic IPv6 FIB and trace for local testing.

The prefix length distribution roughly matches real backbone tables (RIPE
rrc00, RouteViews): /48 dominant, /32 common, /36..44 filling the rest.
"""

import ipaddress
import random
import sys
from pathlib import Path

LEN_WEIGHTS = [(48, 45), (32, 11), (40, 10), (44, 10), (36, 4),
               (64, 10), (56, 5), (24, 3), (16, 2)]

def random_prefix(rng: random.Random, length: int) -> str:
    bits = rng.getrandbits(length)
    value = bits << (128 - length)
    return str(ipaddress.IPv6Address(value))

def weighted_length(rng: random.Random) -> int:
    total = sum(w for _, w in LEN_WEIGHTS)
    r = rng.uniform(0, total)
    acc = 0.0
    for length, w in LEN_WEIGHTS:
        acc += w
        if r <= acc:
            return length
    return LEN_WEIGHTS[-1][0]

def generate(out_dir: Path, fib_size: int, trace_size: int, seed: int) -> None:
    rng = random.Random(seed)
    fib_lines = ["::/0 1"]
    for i in range(fib_size):
        length = weighted_length(rng)
        prefix = random_prefix(rng, length)
        nh = 2 + rng.randrange(250)
        fib_lines.append(f"{prefix}/{length} {nh}")

    trace_lines = []
    for _ in range(trace_size):
        trace_lines.append(str(ipaddress.IPv6Address(rng.getrandbits(128))))

    (out_dir / "sample_fib.txt").write_text("\n".join(fib_lines) + "\n")
    (out_dir / "sample_trace.txt").write_text("\n".join(trace_lines) + "\n")

if __name__ == "__main__":
    out = Path(__file__).resolve().parent
    fib   = int(sys.argv[1]) if len(sys.argv) > 1 else 200
    trace = int(sys.argv[2]) if len(sys.argv) > 2 else 2000
    seed  = int(sys.argv[3]) if len(sys.argv) > 3 else 42
    generate(out, fib, trace, seed)
    print(f"wrote {out}/sample_fib.txt ({fib+1} prefixes)")
    print(f"wrote {out}/sample_trace.txt ({trace} addresses)")
