#!/usr/bin/env bash
# Drive the FIB-size sweep against the already-built binaries.
# Must have run examples/gen_sweep.sh once first.
set -euo pipefail
cd "$(dirname "$0")"/..

TRACE=examples/trace_1m.txt
CORE="${LPM6_CORE:-2}"

echo "=================================================================="
echo "FIB-size sweep  (trace=${TRACE}, pinned to CPU ${CORE})"
echo "=================================================================="

for N in 10000 100000 250000 500000 1000000; do
    FIB="examples/fib_${N}.txt"
    echo ""
    echo "============================ ${N} prefixes ============================"
    echo "--- tree throughput + batch sweep ---"
    taskset -c "${CORE}" ./build/planb-lpm   "${FIB}" "${TRACE}" 20 \
        | grep -E "build|single|batch|FIB"
    echo "--- rebuild + footprint ---"
    taskset -c "${CORE}" ./build/bench_update "${FIB}"            20 \
        | grep -E "FIB|tree rebuild|min|insert|remove|amortized"
    # Skip Patricia + naive for large sizes: allocation + O(N) become
    # impractical here; we only need the tree scaling behaviour.
    if [[ "${N}" -le 250000 ]]; then
        echo "--- tree vs patricia (same FIB) ---"
        taskset -c "${CORE}" ./build/bench_naive "${FIB}" "${TRACE}" 20 \
            | grep -E "build|footprint|RSS|tree |patricia"
    fi
done
