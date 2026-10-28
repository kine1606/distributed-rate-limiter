#!/usr/bin/env bash
# The control plane's tests: periods against lua/quota.lua (test Redis 127.0.0.1:6380), the API and the
# notifier against the dev cluster (127.0.0.1:7001), and the P2 gate end to end with
# build/release/rate_limiter_server. Each test skips, not fails, when what it needs is not running.
#   control/run_tests.sh [test modules, e.g. tests.test_api]
set -euo pipefail
cd "$(dirname "$0")"
[ -x .venv/bin/python ] || ./setup.sh
[ $# -gt 0 ] && exec .venv/bin/python -W ignore::DeprecationWarning -m unittest "$@"  # e.g. tests.test_api
exec .venv/bin/python -W ignore::DeprecationWarning -m unittest discover -s tests -t .
