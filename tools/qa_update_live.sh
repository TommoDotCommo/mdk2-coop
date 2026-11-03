#!/usr/bin/env bash
# The in-game updater on a real game, installed by the new Setup (co-op mod standard J4, J7, J9): one background game.
#  1. builds two local packages from this tree (an old and a new version; nothing is published or downloaded from GitHub)
#  2. installs the old one into the copy with "MDK2-Coop-Setup.exe install <copy> --yes" (Setup's receipts: the updater may
#     replace the files), with a profile folder of the copy standing in for %LOCALAPPDATA% (the real settings.lua is never
#     touched), and checks the exit code and the receipts
#  3. tools/check_launcher_update_live.py --rollback --unchanged: the game starts (T1), the launcher finds the local release,
#     applies it, the restarted game runs the new version, rolls back, the restarted game runs the old one; the game folder
#     ends with the files and hashes it began with, the updater's backups and state are only in <copy>\profile\mdk2-coop\update
#  4. "MDK2-Coop-Setup.exe uninstall <copy> --yes" puts the copy's own files back
# A crash dump or a crash dialog fails the run (tools/mdk.sh crash guard). The T0 twin with more mutants is
# tools/test_update_ownership.py.
#   MUTANT=beside  both packages are built with -DCOOP_MUTANT_UPDATE_BESIDE_DLL (the updater keeps its backups and state
#                  beside the DLL, the old layout): this script must then FAIL.
#   MDK2_N=1 tools/qa_update_live.sh
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N=1
source "$here/mdk.sh"
MDK2_BACKGROUND=1 "$here/install.sh" >/dev/null || exit 1
d="$(mdk_wsl "$(mdk_dir 1)")"
w="$(mdk_dir 1)"
ini="$d/coop/coop.ini"
grep -q '^save_dir=' "$ini" || { echo "FAIL: the copy has no [test] save_dir (MDK2_SAVE_ISOLATE=0?): refusing to run Setup against the real profile"; exit 1; }
out="$here/../local/qa/$(date +%Y%m%d-%H%M%S)-$$-update-live"; mkdir -p "$out"
run="${MDK2_RUN:?set MDK2_RUN to a disposable folder on a Windows filesystem}"; case "$run" in /*) ;; *) run="$(mdk_wsl "$run")" ;; esac; run="$run/update-live-$$"
mkdir -p "$run"
cleanup() { mdk_kill; rm -rf "${run:?}"; }
trap cleanup EXIT

old=v0.9.1-alpha; new=v0.9.2-alpha
mutant_flags=""; [ "${MUTANT:-}" = beside ] && mutant_flags=-DCOOP_MUTANT_UPDATE_BESIDE_DLL
for v in $old $new; do
    COOP_MUTANT_CFLAGS="$mutant_flags" RELEASE_OUT="$out/release" bash "$here/release.sh" "$v" >/dev/null 2>&1 ||
        { echo "FAIL: release $v did not build"; exit 1; }
done
make -s -B -C "$here/../bridge" >/dev/null     # leave a normal DLL in the tree
(cd "$run" && python3 -c 'import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(".")' "$out/release/mdk2-coop-$old.zip")
pkg="$(wslpath -w "$run/mdk2-coop-$old")"
profile="$w\\$(sed -n 's/^save_dir=//p' "$ini" | tr -d '\r')"
setup() {   # verb: Setup.exe <verb> <copy> --yes, with the copy's profile as %LOCALAPPDATA%; prints the exit code
    powershell.exe -NoProfile -Command "\$env:LOCALAPPDATA='$profile'; \$p = Start-Process -FilePath '$pkg\\MDK2-Coop-Setup.exe' -ArgumentList '$1','\"$w\"','--yes' -Wait -PassThru -WindowStyle Hidden -RedirectStandardOutput '$(wslpath -w "$out")\\setup-$1.txt'; \$p.ExitCode" | tr -d '\r'
}
mdk_kill; sleep 1
python3 - "$ini" <<'PY'
import configparser, sys
c = configparser.ConfigParser(interpolation=None); c.optionxform = str; c.read(sys.argv[1])
c['test']['launcher'] = '1'
with open(sys.argv[1], 'w') as f: c.write(f, space_around_delimiters=False)
PY
code=$(setup install)
[ "$code" = 0 ] || { echo "FAIL: Setup install exit $code"; cat "$out/setup-install.txt"; exit 1; }
[ -f "$d/dinput8.dll.coop-receipt.json" ] && [ -f "$d/coop/coop.lua.coop-receipt.json" ] || { echo "FAIL: Setup wrote no receipts"; exit 1; }
echo "PASS  the new Setup installed $old into the copy (exit 0, receipts)"
printf '%s\n' "source '$here/mdk.sh' && p=\$(mdk_start 1) && echo \"\$p\" > \"\$MDK2_PIDS\"" > "$out/start.sh"
printf '%s\n' "source '$here/mdk.sh' && mdk_kill && sleep 2" > "$out/stop.sh"
start_cmd=(bash "$out/start.sh")
stop_cmd=(bash "$out/stop.sh")
python3 "$here/check_launcher_update_live.py" --game-dir "$d" \
    --old-package "$out/release/mdk2-coop-$old.zip" --new-package "$out/release/mdk2-coop-$new.zip" \
    --old-version "$old" --new-version "$new" --output "$out" --rollback --unchanged \
    --start-command "${start_cmd[@]}" --stop-command "${stop_cmd[@]}" 2>&1 | tee "$out/live.txt"
live=${PIPESTATUS[0]}
mdk_kill; sleep 2
code=$(setup uninstall)
[ "$code" = 0 ] && [ ! -e "$d/dinput8.dll.coop-receipt.json" ] || { echo "FAIL: Setup uninstall exit $code"; live=1; }
mdk_crash_report || live=1
echo "evidence: $out"
[ "$live" = 0 ] && echo "PASS: Setup-installed copy starts, updates, rolls back; game folder unchanged" || { echo "FAIL"; exit 1; }
