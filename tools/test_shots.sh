#!/usr/bin/env bash
# mingw-w64 and Windows executable interop; no game or Steam process is needed.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
make -C "$here/bridge" test-shots
