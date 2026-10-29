#!/usr/bin/env bash
# Combat HUD screenshots from the host's view with three background games: floating numbers and
# the hitmarker mid-fight, the collapsed one-line meter, the expanded meter, and a kill with its
# ticker line. Saved under local/showcase/<time>-combat/. Not a test: nothing is checked.
#   MDK2_N=3 tools/combat_showcase.sh
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N=3
source "$here/mdk.sh"
MDK2_BACKGROUND=1 MDK2_START="${SHOWCASE_START:-1 1}" "$here/install.sh" >/dev/null || exit 1
trap mdk_kill EXIT
out="$here/../local/showcase/$(date +%Y%m%d-%H%M%S)-combat"; mkdir -p "$out"
lua()  { mdk_cmd "$(mdk_pid "$1")" "$2"; }
shot() { sleep "${2:-0.1}"; mdk_shot "$(mdk_pid 1)" "$out/$1.png" && echo "$1.png"; }
# n hits of dmg by player <i> on the enemy <gob>, in one command so they land together
hits() { local i="$1" gob="$2" dmg="$3" n="$4" c="" k
         for k in $(seq 1 "$n"); do c="$c mdkDealDamage(mdkGetPlayerGob(),$gob,$dmg,DAMAGE_SNIPER,-1)"; done
         lua "$i" "$c"; }

mdk_boot
lua 1 'coop_set_name("Hank")'; lua 2 'coop_set_name("Kurt Fan")'; lua 3 'coop_set_name("Doc")'
for i in 1 2 3; do lua "$i" 'God(1) coop_combat_reset()'; done
lua 1 'comfuncs.fkey[COM_NEXTSECTION]()'; sleep 1
lua 1 'comfuncs.fkey[COM_NEXTSECTION]()'; sleep 2
# a fight: three players on the first dog
hits 1 coop_dog1 6 5; hits 2 coop_dog1 9 4; hits 3 coop_dog1 14 3
shot fight_collapsed 0.2
sleep 1
hits 1 coop_dog2 7 4; hits 2 coop_dog2 11 3; hits 3 coop_dog2 5 2
lua 1 'coop_press(29,500) coop_press(50,250)'
shot fight_expanded 0.6
hits 2 coop_dog1 999 1
shot kill 0.15
sleep 1
shot ticker 0.3
sleep 1
hits 1 coop_dog2 999 1
shot hitmarker 0
sleep 12
shot after_fight 0.1
echo "shots: $out"
