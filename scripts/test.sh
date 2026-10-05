#!/usr/bin/env bash
# Usage: scripts/test.sh [release|asan|tsan]   (default: release)
set -euo pipefail
preset="${1:-release}"
cd "$(dirname "$0")/.."
scripts/build.sh "$preset"
ctest --test-dir "build/$preset" --output-on-failure
