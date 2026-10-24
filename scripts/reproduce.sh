#!/usr/bin/env bash
# One command from a clean clone: build, run every test, and regenerate the evaluation's tables
# (Week 9 reproducibility check).
#   scripts/reproduce.sh           build + all tests (release), then every table from the newest results/
#   scripts/reproduce.sh --rerun   first reruns every benchmark (hours). It FLUSHES the THROWAWAY dev
#                                  cluster (7001-7003) and test Redis (6380); the fault tests need the fault
#                                  cluster (docker/README.md). Never point it at Redis holding real data.
# results/ is git-ignored, so a clean clone has no tables until the benchmarks have run.
set -euo pipefail
cd "$(dirname "$0")/.."
scripts/test.sh release

if [ "${1:-}" = --rerun ]; then
  for s in bench/run_matrix.sh bench/open_loop.sh bench/ablation.sh bench/concurrency_sweep.sh bench/profile.sh \
    bench/redis_functions.sh tests/eval/quota_accuracy.sh tests/eval/burst_accuracy.sh tests/eval/flood_cardinality.sh \
    tests/eval/faults.sh tests/eval/noisy_neighbour.sh; do
    echo "== $s"
    "$s"
  done
fi

newest() { ls -d results/*_"$1" 2>/dev/null | tail -1; }
out=results/tables.md
mkdir -p results
{
  echo "# Tables regenerated $(date -Iseconds) at commit $(git rev-parse --short HEAD)"
  d=$(newest matrix) && [ -n "$d" ] && { echo -e "\n## Benchmark matrix ($d)\n"; tools/matrix_tables.py "$d/summary.txt"; }
  d=$(newest open_loop) && [ -n "$d" ] && { echo -e "\n## Latency at fixed rates ($d)\n"; tools/open_loop_table.py "$d/summary.txt"; }
  d=$(newest concurrency) && [ -n "$d" ] && { echo -e "\n## USL and Little's law ($d)\n"; tools/usl_fit.py "$d/summary.txt"; }
  d=$(newest profile) && [ -n "$d" ] && for f in "$d"/*.folded; do echo -e "\n## Profile, $(basename "$f" .folded) ($d)\n"; tools/profile_summary.py "$f"; done
  echo -e "\n## Extrapolation to 1,000,000 checks/s (estimate)\n"
  tools/extrapolate.py
} >"$out"
for t in tools/matrix_tables.py tools/open_loop_table.py tools/usl_fit.py tools/profile_summary.py tools/extrapolate.py; do
  "$t" --self-check
done
echo "tables: $out (the ablation, fault and accuracy scripts print their own tables when they run)"
