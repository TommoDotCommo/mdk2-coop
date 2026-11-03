#!/usr/bin/env bash
# Workspace-local install; never execute the upstream downloader script.
set -euo pipefail
version=1.7.7
out="${1:?usage: install-actionlint.sh output-directory}"
mkdir -p "$out"
out=$(cd "$out" && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
base="https://github.com/rhysd/actionlint/releases/download/v$version"
archive="actionlint_${version}_linux_amd64.tar.gz"
curl --fail --silent --show-error --location "$base/$archive" -o "$tmp/$archive"
curl --fail --silent --show-error --location "$base/actionlint_${version}_checksums.txt" -o "$tmp/checksums.txt"
(cd "$tmp" && sha256sum --check --ignore-missing checksums.txt)
tar -xzf "$tmp/$archive" -C "$out" actionlint
"$out/actionlint" -version
