#!/usr/bin/env bash
# Build the release: local/release/ gets
#   MDK2-Coop-Setup.exe (+ .sha256)   the single-file Setup, the main download: the files below are embedded in it
#   mdk2-coop-<version>.zip (+ .sha256)  the same files for a manual install or Nexus: dinput8.dll, coop\coop.lua, the
#                                     scripts, the couch launcher, install-linux.sh, README.txt and LICENSE.txt. The updater in the
#                                     launcher reads this zip; the Setup.exe inside it needs the files beside it.
#   tools/release.sh            version from `git describe --tags --always`
#   tools/release.sh v0.1.0     explicit version
# Code signing: set SIGN_CMD to a command template that signs one file in place; {file} is replaced by the (quoted) path.
#   SIGN_CMD='osslsigncode sign -pkcs12 cert.p12 -pass "$PASS" -h sha256 -t http://timestamp.digicert.com -in {file} -out {file}.signed && mv {file}.signed {file}'
#   SIGN_CMD='signtool.exe sign /fd sha256 /tr http://timestamp.digicert.com /td sha256 /a {file}'
# Every exe and dll shipped is signed BEFORE the payload is hashed and embedded, and the final Setup.exe after. Each signature
# is verified (a certificate must be present; SIGN_VERIFY_CMD adds a full check of your own). A failing sign aborts the build.
# Unset SIGN_CMD builds unsigned files, with a warning. See docs/LAUNCHER-MAINTENANCE.md.
# Publishing is a separate step: gh release create <version> local/release/MDK2-Coop-Setup.exe* local/release/mdk2-coop-<version>.zip*
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
ver="${1:-$(git -C "$here" describe --tags --always --dirty)}"
# Keep archive names and substitutions safe; updater convention is a semver tag.
if [[ ! "$ver" =~ ^[A-Za-z0-9][A-Za-z0-9._+-]*$ ]]; then
    echo 'Version must contain only letters, digits, dot, underscore, plus or hyphen' >&2
    exit 2
fi
name="mdk2-coop-$ver"
out="${RELEASE_OUT:-$here/local/release}"
stage="$out/$name"
work="$out/work-$ver"

# Sign (and verify) the given files in place, when SIGN_CMD is set.
sign_files() {
    [ -n "${SIGN_CMD:-}" ] || return 0
    local f cmd quoted
    for f in "$@"; do
        quoted=$(printf '%q' "$f")
        cmd="${SIGN_CMD//\{file\}/$quoted}"
        if ! bash -c "$cmd"; then echo "release: signing failed for $f" >&2; exit 1; fi
        if [ -n "${SIGN_VERIFY_CMD:-}" ]; then
            cmd="${SIGN_VERIFY_CMD//\{file\}/$quoted}"
            if ! bash -c "$cmd"; then echo "release: signature verification failed for $f" >&2; exit 1; fi
        fi
        python3 "$here/tools/check_release_package.py" --signed "$f" || { echo "release: $f carries no signature after signing" >&2; exit 1; }
    done
}
if [ -z "${SIGN_CMD:-}" ]; then
    echo 'release: WARNING: SIGN_CMD is not set, so the exes and dlls are UNSIGNED (Windows SmartScreen and antivirus will warn players).' >&2
elif [[ "$SIGN_CMD" != *'{file}'* ]]; then
    echo 'release: SIGN_CMD must contain {file}' >&2; exit 2
fi
mutant="${RELEASE_MUTANT:-}"        # tools/test_release_signing.sh only: 'late-sign' signs the payload files after embedding

if [ -z "${RELEASE_REUSE_DLL:-}" ]; then   # tools/test_release_signing.sh only: the signing scenarios do not need a fresh DLL each time
    make -s -B -C "$here/bridge" VERSION="$ver"
fi
rm -rf "$stage" "$work" "$out/$name.zip" "$out/MDK2-Coop-Setup.exe"
mkdir -p "$stage/coop" "$work"
cp "$here/bridge/dinput8.dll" "$stage/"
"$here/tools/build_install_policy.sh" "$stage/install-policy.exe"
sed 's/\r$//; s/$/\r/' "$here/lua/coop_dev.lua" > "$stage/coop/coop.lua"
for f in mdk2-coop.ps1 installer-gui.ps1 install-state.ps1 Couch.cmd mdk2-couch.ps1; do
    sed 's/\r$//; s/$/\r/' "$here/dist/$f" > "$stage/$f"; done
sed 's/\r$//; s/$/\r/; s/@VERSION@/'"$ver"'/' "$here/dist/README.txt" > "$stage/README.txt"
sed 's/\r$//; s/$/\r/' "$here/LICENSE" > "$stage/LICENSE.txt"
sed 's/\r$//' "$here/dist/install-linux.sh" > "$stage/install-linux.sh"
chmod 755 "$stage/install-linux.sh"

# The Setup that sits inside the zip: no payload, it starts the scripts beside it.
(cd "$here/dist" && i686-w64-mingw32-windres installer.rc -o "$work/installer-res.o")
i686-w64-mingw32-gcc -Os -Wall -Wextra -mwindows -o "$stage/MDK2-Coop-Setup.exe" \
    "$here/dist/installer.c" "$work/installer-res.o" -lshell32

# Sign everything that ships, before the payload is hashed and embedded.
if [ "$mutant" != late-sign ]; then
    sign_files "$stage/dinput8.dll" "$stage/install-policy.exe" "$stage/MDK2-Coop-Setup.exe"
fi

# The single-file Setup: every file of the zip except the zip's own Setup.exe and the Linux script, as one verified payload.
payload=(dinput8.dll=dinput8.dll coop/coop.lua=coop/coop.lua mdk2-coop.ps1=mdk2-coop.ps1 installer-gui.ps1=installer-gui.ps1
         install-state.ps1=install-state.ps1 install-policy.exe=install-policy.exe Couch.cmd=Couch.cmd mdk2-couch.ps1=mdk2-couch.ps1
         README.txt=README.txt LICENSE.txt=LICENSE.txt)
args=()
for spec in "${payload[@]}"; do args+=("${spec%%=*}=$stage/${spec#*=}"); done
python3 "$here/tools/payload.py" pack "$work" "$ver" "${args[@]}"
i686-w64-mingw32-windres "$work/setup.rc" -o "$work/setup-res.o"
i686-w64-mingw32-gcc -Os -Wall -Wextra -mwindows -o "$out/MDK2-Coop-Setup.exe" \
    "$here/dist/installer.c" "$work/setup-res.o" -lshell32
if [ "$mutant" = late-sign ]; then
    sign_files "$stage/dinput8.dll" "$stage/install-policy.exe" "$stage/MDK2-Coop-Setup.exe"
fi
sign_files "$out/MDK2-Coop-Setup.exe"
(cd "$out" && sha256sum MDK2-Coop-Setup.exe > MDK2-Coop-Setup.exe.sha256)
rm -rf "$work"

(cd "$out" && python3 -c 'import sys, zipfile, os
z = zipfile.ZipFile(sys.argv[1] + ".zip", "w", zipfile.ZIP_DEFLATED)
for root, _, files in os.walk(sys.argv[1]):
    for f in sorted(files):
        path = os.path.join(root, f)
        info = zipfile.ZipInfo.from_file(path)       # keeps the executable bit of install-linux.sh
        with open(path, "rb") as fh:
            z.writestr(info, fh.read(), compress_type=zipfile.ZIP_DEFLATED)
z.close()' "$name")
(cd "$out" && sha256sum "$name.zip" > "$name.zip.sha256")
echo "$out/MDK2-Coop-Setup.exe ($(du -h "$out/MDK2-Coop-Setup.exe" | cut -f1))"
echo "$out/$name.zip ($(du -h "$out/$name.zip" | cut -f1))"
