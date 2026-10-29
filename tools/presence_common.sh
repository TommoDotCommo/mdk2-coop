# Shared process-state capture for the presence scenarios. Source after mdk.sh.
# Each command writes a fresh, uniquely named atomic dump from the process itself.
pass=0; fail=0
out="$here/../local/qa/$(date +%Y%m%d-%H%M%S)-presence-${scenario}"
mkdir -p "$out"
lua() { mdk_cmd "$(mdk_pid "$1")" "$2"; }
capture() {
    local tag="$1" i path t
    for i in $(mdk_seq); do
        path="$(mdk_wsl "$(mdk_dir "$i")")/coop/state_${tag}_${i}.txt"
        rm -f "$path"
        lua "$i" "coop_state(\"${tag}_${i}\")" || return 1
        for t in $(seq 1 150); do [ -f "$path" ] && break; sleep .1; done
        cp "$path" "$out/${tag}_${i}.txt" || return 1
    done
}
check() {
    local name="$1"; shift
    if "$@"; then echo "PASS $name"; pass=$((pass+1));
    else echo "FAIL $name"; fail=$((fail+1)); fi
}
finish() { mdk_kill; }
trap finish EXIT
summary() { echo "$pass passed, $fail failed"; echo "process evidence: $out"; [ "$fail" -eq 0 ]; }
