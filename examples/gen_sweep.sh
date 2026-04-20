#!/usr/bin/env bash
# Generate a FIB-size sweep and a single shared 1M trace.
# All files land in examples/ with names fib_<N>.txt and trace_1m.txt.
set -euo pipefail
cd "$(dirname "$0")"

TRACE=1000000
for N in 10000 100000 250000 500000 1000000; do
    echo "=== generating ${N} prefixes ==="
    python3 generate_sample.py "${N}" "${TRACE}" 42
    mv sample_fib.txt   "fib_${N}.txt"
    if [[ ! -f trace_1m.txt ]]; then
        mv sample_trace.txt trace_1m.txt
    else
        rm sample_trace.txt
    fi
done
ls -la fib_*.txt trace_1m.txt
