#!/usr/bin/env bash
# WSL + mingw-w64; no game or Steam process is needed.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
make -C "$here/bridge" test-shots
