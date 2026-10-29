#!/usr/bin/env bash
# The game embeds Lua 3.1: check lua/coop_dev.lua (and the bot's Lua) with the real Lua 3.1 compiler, so a
# Lua 4+ construct (break, local function, ...) fails here instead of as a silently unloaded coop.lua in game.
# The compiler is built once into local/tools/lua31 from the lua.org release (checked against its SHA-256).
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
dir="$root/local/tools/lua31"
luac="$dir/lua/bin/luac"
if [ ! -x "$luac" ]; then
    mkdir -p "$dir"
    curl -fsSL -o "$dir/lua-3.1.tar.gz" https://www.lua.org/ftp/lua-3.1.tar.gz
    echo "797ceabe28df53ce6dd6bfb4397d6e0b2e349b3bd0a1883c9c8ae0212d8cd7ed  $dir/lua-3.1.tar.gz" | sha256sum -c --quiet
    tar xzf "$dir/lua-3.1.tar.gz" -C "$dir"
    make -s -C "$dir/lua" LUA="$dir/lua" >/dev/null 2>&1
fi
check() { local out; out="$("$luac" -p "$1" 2>&1)"; [ -z "$out" ] || { echo "$out"; return 1; }; }
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
echo 'x = 1 while (x < 3) do x = x + 1 break end' > "$tmp/break.lua"
if check "$tmp/break.lua" >/dev/null; then echo "FAIL the Lua 3.1 compiler accepted break"; exit 1; fi
echo "PASS Lua 3.1 compiler rejects Lua 4 syntax (break)"
check "$root/lua/coop_dev.lua"
echo "PASS lua/coop_dev.lua is valid Lua 3.1"
