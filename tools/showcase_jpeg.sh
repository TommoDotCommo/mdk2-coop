#!/usr/bin/env bash
# showcase_jpeg.sh <in.png> <out.jpg> [quality] [x,y,w,h;x,y,w,h] [width]: a showcase PNG as a JPEG, with the listed
# rectangles pixelated (see tools/showcase_jpeg.ps1). Quality 82 gives about the size of the first showcase set.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
in="$(wslpath -w "$(realpath "$1")")"; out="$(wslpath -w "$(realpath -m "$2")")"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(wslpath -w "$here/showcase_jpeg.ps1")" -In "$in" -Out "$out" -Quality "${3:-82}" -Hide "${4:-}" -Width "${5:-0}"
