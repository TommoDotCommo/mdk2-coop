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
[ $fails -eq 0 ] && echo "PASS: release signing hook (signs before embedding, verifies, aborts on failure) with the late-sign mutant" || exit 1
