#!/usr/bin/env bash
# A game build the mod does not know must be said in plain words, not only in bridge.log (bridge/dinput8.c
# unsupported_build_notice): one background game on a COPY of the exe patched so that its build signature fails
# (the "copytagmethods" name changed by one byte, so the exe's hash is unknown too).
# Asserted from the game's own files and windows: coop\unsupported-build.txt has the sentence, what to do and the exe's
# real SHA-256; with [test] notice_dialog=1 a real message box of the game process shows the same text (found by its
# window, then closed); the game keeps running and no crash dump appears. On a known build the notice is absent.
#   MUTANT=silent  installs a DLL built with -DCOOP_MUTANT_NO_BUILD_NOTICE (only a log line, the old behaviour):
#                  this script must then FAIL.
#   MDK2_N=1 tools/qa_build_message.sh
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N=1
source "$here/mdk.sh"
MDK2_BACKGROUND=1 "$here/install.sh" >/dev/null || exit 1

pass=0; fail=0; results=()
out="$here/../local/qa/$(date +%Y%m%d-%H%M%S)-$$-build-message"; mkdir -p "$out"
check() { local name="$1"; shift
    if "$@" >/dev/null 2>&1; then results+=("PASS  $name"); pass=$((pass + 1)); else results+=("FAIL  $name"); fail=$((fail + 1)); fi; }
wait_for() { local i; for i in $(seq 1 "$2"); do eval "$1" && return 0; sleep 1; done; return 1; }
d="$(mdk_wsl "$(mdk_dir 1)")"
log="$(mdk_log 1)"
notice="$d/coop/unsupported-build.txt"
ini="$d/coop/coop.ini"

# the original exe, kept to put back (the copy's file is hard-linked from the base: replaced, never written in place)
cp "$d/mdk2hd.exe" "$out/original.exe"
restore() { mdk_kill; rm -f "$d/mdk2hd.exe" && cp "$out/original.exe" "$d/mdk2hd.exe"; }
trap restore EXIT

# 1. known build first: no notice (also removes a stale file from earlier runs)
rm -f "$notice"
mdk_kill; sleep 1
echo "$(mdk_start 1)" > "$MDK2_PIDS"
check "known build: the Lua hook is installed"         wait_for "grep -q 'lua hook installed' '$log' 2>/dev/null" 45
check "known build: no unsupported-build notice"       test ! -e "$notice"
mdk_kill; sleep 2

# 2. patch the copy's exe: change the builtin name by one byte at the file offset of the signature string
python3 - "$out/original.exe" "$out/patched.exe" <<'PY'
import struct, sys
data = bytearray(open(sys.argv[1], 'rb').read())
pe = struct.unpack_from('<I', data, 0x3c)[0]
base = struct.unpack_from('<I', data, pe + 24 + 28)[0]
nsec, opt = struct.unpack_from('<H', data, pe + 6)[0], struct.unpack_from('<H', data, pe + 20)[0]
at = None
for i in range(nsec):
    s = pe + 24 + opt + i * 40
    vsize, va, rawsize, raw = struct.unpack_from('<IIII', data, s + 8)
    for name_va in (0x4d7d34, 0x4d769c):
        rva = name_va - base
        if va <= rva < va + min(vsize, rawsize) and data[raw + rva - va:raw + rva - va + 14] == b'copytagmethods':
            at = raw + rva - va
assert at, 'no known signature string in this exe'
data[at + 13] = ord('z')
open(sys.argv[2], 'wb').write(data)
PY
[ -s "$out/patched.exe" ] || { echo "could not patch the exe copy"; exit 1; }
want_hash="$(sha256sum "$out/patched.exe" | cut -d' ' -f1)"
rm -f "$d/mdk2hd.exe" && cp "$out/patched.exe" "$d/mdk2hd.exe"
check "the patched copy is a different exe"            test "$(sha256sum "$d/mdk2hd.exe" | cut -d' ' -f1)" = "$want_hash" -a "$want_hash" != "$(sha256sum "$out/original.exe" | cut -d' ' -f1)"

if [ "${MUTANT:-}" = silent ]; then
    make -s -B -C "$here/../bridge" CFLAGS="-O2 -Wall -Wextra -Wno-unused-parameter -DCOOP_MUTANT_NO_BUILD_NOTICE" || exit 1
    cp "$here/../bridge/dinput8.dll" "$d/dinput8.dll"
    make -s -B -C "$here/../bridge" >/dev/null    # leave a normal DLL in the tree
fi

# 3. the unknown build: file, dialog, game still alive
grep -q '^\[test\]' "$ini" || printf '\n[test]\n' >> "$ini"
sed -i '0,/^\[test\]/s//[test]\nnotice_dialog=1/' "$ini"     # inside the existing section: a repeated one is not read
rm -f "$notice"
echo "$(mdk_start 1)" > "$MDK2_PIDS"
pid="$(mdk_pid 1)"
check "unknown build: bridge.log says so"              wait_for "grep -q 'unknown game build' '$log' 2>/dev/null" 45
check "unknown build: the Lua hook is not installed"   bash -c "! grep -q 'lua hook installed' '$log'"
check "unknown build: unsupported-build.txt written"   wait_for "[ -s '$notice' ]" 15
text="$(tr -d '\r' < "$notice" 2>/dev/null)"
cp "$notice" "$out/unsupported-build.txt" 2>/dev/null; cp "$log" "$out/bridge.log" 2>/dev/null
check "the notice says co-op is off, in plain words"   grep -q "Co-op is off: this copy of MDK2 HD is a game build the co-op mod does not know" <<<"$text"
check "the notice names the supported builds"          grep -q "Supported: MDK2 HD on Steam (build 3902) and the Overhaul Games build" <<<"$text"
check "the notice says what to do"                     grep -q "Betas and choose None.*Verify integrity of game files" <<<"$(tr '\n' ' ' <<<"$text")"
check "the notice carries the exe's real SHA-256"      grep -q "Game file SHA-256: $want_hash" <<<"$text"
# the box itself: a window of the game process titled MDK2 Co-op with the same words; close it
dialogs="$(wslpath -w "$here/win/dialogs.ps1")"
shot=""
for _ in $(seq 1 40); do
    shot=$(powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$dialogs" -Dirs "$(mdk_dir 1)" 2>/dev/null | tr -d '\r' | grep "|MDK2 Co-op|" || true)
    [ -n "$shot" ] && break
    sleep 0.5
done
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$dialogs" -Dirs "$(mdk_dir 1)" -Close >/dev/null 2>&1     # close our own box before the crash guard looks at windows
echo "$shot" > "$out/dialog-text.txt"
check "a message box of the game shows the sentence"   grep -q "Co-op is off: this copy of MDK2 HD is a game build" <<<"$shot"
check "the game is still running"                      mdk_alive "$pid"
mdk_kill
check "no crash dump or dialog from the unknown build" mdk_crash_report
sed -i '/^notice_dialog=1$/d' "$ini"

printf '%s\n' "${results[@]}"
echo "evidence: $out"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
