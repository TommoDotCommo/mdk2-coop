#!/usr/bin/env bash
# Validate the current installer against disposable fixtures, reading only a leased executable.
# No game is launched. The source executable and the owner's saves/settings are never changed.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
source "$here/tools/mdk.sh"
: "${MDK2_RUN:?set MDK2_RUN to a disposable directory on a Windows filesystem}"
make -s -C "$here/bridge"
mkdir -p "$here/local/installer"
out=$(mktemp -d "$here/local/installer/run-XXXXXXXX")
# Win32 profile API fixtures need a Windows filesystem.
fixtures=$(mktemp -d "$MDK2_RUN/installer-XXXXXXXX")
printf '%s\n' "$fixtures" > "$out/fixtures.txt"
mkdir -p "$out/package/coop"
cp "$here/bridge/dinput8.dll" "$out/package/"
cp "$here/lua/coop_dev.lua" "$out/package/coop/coop.lua"
cp "$here/dist/mdk2-coop.ps1" "$out/package/"
cp "$here/dist/installer-gui.ps1" "$out/package/"
cp "$here/dist/install-state.ps1" "$out/package/"
"$here/tools/build_install_policy.sh"
cp "$here/dist/install-policy.exe" "$out/package/"
cp "$here/dist/install-policy.exe" "$fixtures/install-policy.exe"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(wslpath -w "$here/tools/test_install_state.ps1")" -Mutants -PolicyPath "$(wslpath -w "$fixtures/install-policy.exe")"
(cd "$here/dist" && i686-w64-mingw32-windres installer.rc -o "$out/installer-res.o")
i686-w64-mingw32-gcc -Os -Wall -Wextra -mwindows -o "$out/package/MDK2-Coop-Setup.exe" "$here/dist/installer.c" "$out/installer-res.o"
i686-w64-mingw32-gcc -O2 -o "$out/package/data_loader_test.exe" "$here/bridge/tests/data_loader_test.c"
cp -a "$out/package" "$fixtures/setup-package"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(wslpath -w "$here/tools/test_installer_startup.ps1")" -PackageDir "$(wslpath -w "$fixtures/setup-package")"
# the single-file Setup: built by the release script, checked, started from a folder with nothing else in it
RELEASE_OUT="$out/release" RELEASE_REUSE_DLL=1 SIGN_CMD="" bash "$here/tools/release.sh" v0.0.0-qa >/dev/null
python3 "$here/tools/check_release_package.py" "$out/release/mdk2-coop-v0.0.0-qa.zip"
mkdir -p "$fixtures/single"
cp "$out/release/MDK2-Coop-Setup.exe" "$fixtures/single/"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(wslpath -w "$here/tools/test_installer_startup.ps1")" -PackageDir "$(wslpath -w "$fixtures/setup-package")" -SingleFile "$(wslpath -w "$fixtures/single/MDK2-Coop-Setup.exe")"
python3 "$here/tools/test_setup_exe.py"
# the scriptable verbs (status --json, install/uninstall --yes, exit codes) of the zip's and the single-file Setup, with mutants
mkdir -p "$fixtures/verbs"
(cd "$fixtures/verbs" && python3 -c 'import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(".")' "$out/release/mdk2-coop-v0.0.0-qa.zip")
python3 "$here/tools/fixtures/make_build_fixtures.py" "$fixtures/verbs/builds" >/dev/null
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(wslpath -w "$here/tools/test_setup_verbs.ps1")" \
    -PackageDir "$(wslpath -w "$fixtures/verbs/mdk2-coop-v0.0.0-qa")" -Fixtures "$(wslpath -w "$fixtures/verbs/builds")" \
    -TestRoot "$(wslpath -w "$fixtures/verbs")" -SingleFile "$(wslpath -w "$fixtures/single/MDK2-Coop-Setup.exe")" -Mutants
# plain build messages at install: synthetic fixture exes, mutants
python3 "$here/tools/fixtures/make_build_fixtures.py" "$fixtures/builds"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(wslpath -w "$here/tools/test_installer_builds.ps1")" \
    -PackageDir "$(wslpath -w "$fixtures/setup-package")" -Fixtures "$(wslpath -w "$fixtures/builds")" -TestRoot "$(wslpath -w "$fixtures")"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(wslpath -w "$here/tools/test_installer.ps1")" \
    -GameExe "$(mdk_dir 1)\\mdk2hd.exe" -PackageDir "$(wslpath -w "$fixtures/setup-package")" \
    -TestRoot "$(wslpath -w "$fixtures")"
echo "installer fixtures: $out"
