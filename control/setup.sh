#!/usr/bin/env bash
# Creates control/.venv with the pinned packages. The system's grpcio stays visible (end-to-end tests
# call the limiter). Python without ensurepip (Ubuntu without python3-venv) gets pip from PyPA.
set -euo pipefail
cd "$(dirname "$0")"
if [ ! -x .venv/bin/python ]; then
  if python3 -c "import ensurepip" 2>/dev/null; then
    python3 -m venv --system-site-packages .venv
  else
    python3 -m venv --system-site-packages --without-pip .venv
    tmp=$(mktemp -d)
    curl -sSfL https://bootstrap.pypa.io/get-pip.py -o "$tmp/get-pip.py"
    .venv/bin/python -I "$tmp/get-pip.py" -q
    rm -rf "$tmp"
  fi
fi
.venv/bin/python -m pip install -q -r requirements.txt
.venv/bin/python -c "import fastapi, uvicorn, redis, httpx; print('control/.venv ready: fastapi', fastapi.__version__, 'redis', redis.__version__)"
