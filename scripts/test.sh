#!/usr/bin/env bash
# Usage: scripts/test.sh [release|asan|tsan]   (default: release)
set -euo pipefail
preset="${1:-release}"
cd "$(dirname "$0")/.."
scripts/build.sh "$preset"
# TSan cannot start in some address layouts that the kernel's randomisation produces ("FATAL:
# ThreadSanitizer: unexpected memory mapping"); without randomisation it always can.
run=()
[ "$preset" = tsan ] && run=(setarch "$(uname -m)" -R)
"${run[@]}" ctest --test-dir "build/$preset" --output-on-failure
