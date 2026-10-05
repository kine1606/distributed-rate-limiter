#!/usr/bin/env bash
# Usage: scripts/build.sh [release|debug|asan|tsan]   (default: release)
set -euo pipefail
preset="${1:-release}"
cd "$(dirname "$0")/.."
cmake --preset "$preset"
cmake --build "build/$preset" -j"$(nproc)"
