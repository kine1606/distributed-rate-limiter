#!/usr/bin/env bash
# Line and branch coverage of src/ and include/ by every test (unit, script and integration tests;
# the last two need the dev cluster and the test Redis up), measured with gcov.
#   scripts/coverage.sh        -> build/coverage/coverage.txt: per file, then each uncovered line
set -euo pipefail
cd "$(dirname "$0")/.."
cmake --preset coverage >/dev/null
cmake --build build/coverage -j"$(nproc)"
find build/coverage -name '*.gcda' -delete
ctest --test-dir build/coverage --output-on-failure
python3 tools/coverage_report.py build/coverage >build/coverage/coverage.txt
sed -n '1,/^$/p' build/coverage/coverage.txt
echo "full report: build/coverage/coverage.txt"
