#!/usr/bin/env bash
# Creates control/.venv with the pinned packages. The system's grpcio stays visible (end-to-end tests
# call the limiter). Python without ensurepip (Ubuntu without python3-venv) gets pip from PyPA, as a
# pinned version checked against its SHA-256 before it runs.
set -euo pipefail
cd "$(dirname "$0")"
if [ ! -x .venv/bin/python ]; then
  if python3 -c "import ensurepip" 2>/dev/null; then
    python3 -m venv --system-site-packages .venv
  else
    python3 -m venv --system-site-packages --without-pip .venv
    tmp=$(mktemp -d)
    curl -sSfL https://bootstrap.pypa.io/pip/zipapp/pip-26.2.2.pyz -o "$tmp/pip.pyz"
    echo "e5ef64ef63ff89a746014ad81658b76c1940385fdcdbc4fcbfa6e3ef1887352d  $tmp/pip.pyz" | sha256sum -c --quiet - ||
      { echo "checksum mismatch: pip-26.2.2.pyz" >&2; exit 1; }
    .venv/bin/python -I "$tmp/pip.pyz" install -q pip==26.2.2
    rm -rf "$tmp"
  fi
fi
.venv/bin/python -m pip install -q -r requirements.txt
.venv/bin/python -c "import fastapi, uvicorn, redis, httpx; print('control/.venv ready: fastapi', fastapi.__version__, 'redis', redis.__version__)"
