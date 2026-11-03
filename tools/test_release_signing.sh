#!/usr/bin/env bash
# tools/release.sh code-signing hook with a fake signer (tools/fixtures/fake_sign.py), game-free:
#   signs every exe/dll before the payload is embedded; unsigned build warns; a failing or fake-successful signer aborts.
# Mutant: RELEASE_MUTANT=late-sign (payload files signed after embedding) must fail check_release_package.py --require-signed.
set -uo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
tmp=$(mktemp -d)
trap 'rm -rf "${tmp:?}"' EXIT
fails=0
fail() { echo "FAIL: $*"; fails=$((fails + 1)); }
ver=v0.0.0-signtest
check() { python3 "$here/tools/check_release_package.py" "$RELEASE_OUT/mdk2-coop-$ver.zip" "$@" >/dev/null 2>&1; }
fake="python3 $here/tools/fixtures/fake_sign.py {file}"

export RELEASE_OUT="$tmp/signed"
SIGN_CMD="$fake" bash "$here/tools/release.sh" $ver >"$tmp/log1" 2>&1 || { fail "signed release did not build"; cat "$tmp/log1"; }
check --require-signed || fail "every exe/dll, the payload copies and the single-file Setup must be signed"
check || fail "a signed release must still pass the plain checks"
grep -q WARNING "$tmp/log1" && fail "a signed build must not warn"

export RELEASE_REUSE_DLL=1      # the first build made the DLL; the other scenarios only test signing
export RELEASE_OUT="$tmp/late"
SIGN_CMD="$fake" RELEASE_MUTANT=late-sign bash "$here/tools/release.sh" $ver >"$tmp/log2" 2>&1
check --require-signed && fail "mutant late-sign survived: payload signed after embedding went unnoticed"

export RELEASE_OUT="$tmp/unsigned"
env -u SIGN_CMD bash "$here/tools/release.sh" $ver >"$tmp/log3" 2>&1 || fail "unsigned release did not build"
grep -q 'SIGN_CMD is not set' "$tmp/log3" || fail "an unsigned build must warn"
check || fail "an unsigned release passes the plain checks"
check --require-signed && fail "an unsigned release must fail --require-signed"

export RELEASE_OUT="$tmp/failing"
SIGN_CMD="false {file}" bash "$here/tools/release.sh" $ver >"$tmp/log4" 2>&1 && fail "a failing signer must abort the release"
SIGN_CMD="true {file}" bash "$here/tools/release.sh" $ver >"$tmp/log5" 2>&1 && fail "a signer that signs nothing must abort the release"
SIGN_CMD="python3 $here/tools/fixtures/fake_sign.py" bash "$here/tools/release.sh" $ver >"$tmp/log6" 2>&1 && fail "SIGN_CMD without {file} must be refused"
SIGN_CMD="$fake" SIGN_VERIFY_CMD="false {file}" bash "$here/tools/release.sh" $ver >"$tmp/log7" 2>&1 && fail "a failing SIGN_VERIFY_CMD must abort the release"
# External-action route: no local signer is configured. Pause at each approval boundary.
unset SIGN_CMD SIGN_VERIFY_CMD
export RELEASE_OUT="$tmp/signpath"
bash "$here/tools/release.sh" "$ver" prepare >"$tmp/log8" 2>&1 || { fail "stage A preparation failed"; cat "$tmp/log8"; }
RELEASE_REQUIRE_SIGNED=1 bash "$here/tools/release.sh" "$ver" assemble >"$tmp/log9" 2>&1 && fail "mutant skip-stage-A survived"
python3 "$here/tools/fixtures/fake_signpath.py" A "$RELEASE_OUT" "$tmp/action-a" "$ver" || fail "fake SignPath A failed"
cp "$tmp/action-a/"* "$RELEASE_OUT/mdk2-coop-$ver/"
RELEASE_REQUIRE_SIGNED=1 bash "$here/tools/release.sh" "$ver" assemble >"$tmp/log10" 2>&1 || { fail "signed payload assembly failed"; cat "$tmp/log10"; }
# Keep a copy so an intentional failed finalization cannot consume the stage state.
cp -a "$RELEASE_OUT" "$tmp/skip-b"
RELEASE_OUT="$tmp/skip-b" RELEASE_REQUIRE_SIGNED=1 bash "$here/tools/release.sh" "$ver" finalize >"$tmp/log11" 2>&1 && fail "mutant skip-stage-B survived"
python3 "$here/tools/fixtures/fake_signpath.py" B "$RELEASE_OUT" "$tmp/action-b" "$ver" || fail "fake SignPath B failed"
cp "$tmp/action-b/MDK2-Coop-Setup.exe" "$RELEASE_OUT/"
cp -a "$RELEASE_OUT" "$tmp/changed-a"
printf changed >> "$tmp/changed-a/mdk2-coop-$ver/dinput8.dll"
RELEASE_OUT="$tmp/changed-a" RELEASE_REQUIRE_SIGNED=1 bash "$here/tools/release.sh" "$ver" finalize >"$tmp/log12" 2>&1 && fail "mutant change-stage-A-after-assembly survived"
RELEASE_REQUIRE_SIGNED=1 bash "$here/tools/release.sh" "$ver" finalize >"$tmp/log13" 2>&1 || { fail "external signed release did not finalize"; cat "$tmp/log13"; }
check --require-signed || fail "external two-stage release must pass all signed package checks"

# A fresh unsigned build (without DLL reuse) must produce the same bytes: PE timestamps, ZIP ordering,
# modes and member mtimes cannot depend on the wall clock or output directory.
export RELEASE_OUT="$tmp/repro"
env -u RELEASE_REUSE_DLL bash "$here/tools/release.sh" "$ver" >"$tmp/log14" 2>&1 || { fail "reproducibility rebuild failed"; cat "$tmp/log14"; }
for asset in "mdk2-coop-$ver.zip" MDK2-Coop-Setup.exe; do
    cmp "$tmp/unsigned/$asset" "$RELEASE_OUT/$asset" || fail "unsigned $asset is not reproducible"
done
[ $fails -eq 0 ] && echo "PASS: release signing hooks and fake SignPath A -> payload -> B; late-sign, skipped-stage and changed-input mutants rejected; unsigned builds reproducible" || exit 1
