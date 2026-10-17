#!/usr/bin/env bash
# Usage: clientgold.sh <client> <data dir> <out dir> <replay.sbr>...
# Plays each replay in the real client (fresh home dir, sound off) from <data dir> and checks
# that it loaded the replay's map, played to the end without a state hash mismatch, and
# finished with the recorded run time. Prints one line per replay; exit 1 if any failed.
# Env: FRAMEWORKS=<dir> sets DYLD_FRAMEWORK_PATH for the client (macOS strips DYLD_* variables
# when it starts this script through /bin/bash).
set -u
[ -n "${FRAMEWORKS:-}" ] && export DYLD_FRAMEWORK_PATH=$FRAMEWORKS
# keeps the client from taking focus from other apps on macOS (an SDL hint)
export SDL_MAC_BACKGROUND_APP=1
here=$(cd "$(dirname "$0")" && pwd)
overrides=$(cd "$here/../../data/overrides" && pwd)
client=$1; data=$2; mkdir -p "$3"; out=$(cd "$3" && pwd); shift 3
timeout=${TIMEOUT:-900}
limit() {
    local secs=$1; shift
    if command -v timeout > /dev/null; then timeout "$secs" "$@"; return; fi
    "$@" & local pid=$!
    ( sleep "$secs"; kill "$pid" 2> /dev/null ) & local watchdog=$!
    wait "$pid"; local code=$?
    kill "$watchdog" 2> /dev/null
    return $code
}
fail=0; n=0; total=$#; t0=$(date +%s)
for sbr in "$@"; do
    n=$((n+1))
    name=$(basename "$sbr" .sbr); map=${name%%_*}
    home=$out/$name
    rm -rf "$home"; mkdir -p "$home/replays"
    cp "$sbr" "$home/replays/"
    printf 'fullscreen 0\nscr_w 320\nscr_h 240\nusesound 0\n' > "$home/init.cfg"
    cp "$here/clientgold.cfg" "$home/"
    echo "[$n/$total] $name: start" >&2
    s=$(date +%s)
    ( cd "$data" && limit "$timeout" "$client" -q"$home" -o"$overrides" -glog.txt -x'exec clientgold.cfg' > /dev/null 2>&1 )
    code=$?
    log=$home/log.txt
    line=$(grep -h '^GOLDCHECK' "$log" 2>/dev/null | tr -d '\r')
    desync=$(sed -n 's/.*desync=\([0-9]*\).*/\1/p' <<<"$line")
    ms=$(sed -n 's/.*runtime_ms=\([0-9]*\).*/\1/p' <<<"$line")
    rec=$(sed -n 's/.*recorded=\([0-9:.]*\).*/\1/p' <<<"$line")
    recms=$(awk -F'[:.]' 'NF==3 {print $1*60000 + $2*1000 + $3}' <<<"$rec")
    problems=""
    [ $(( $(date +%s)-s )) -ge "$timeout" ] && problems+=" timeout-${timeout}s"
    grep -Eq "read map packages[/\\\\]base[/\\\\]$map\.ogz" "$log" 2>/dev/null || problems+=" map-not-loaded"
    grep -q 'input replay finished' "$log" 2>/dev/null || problems+=" replay-did-not-finish"
    [ "$desync" = 0 ] || problems+=" desync=${desync:-?}"
    [ -n "$recms" ] && [ "$ms" = "$recms" ] || problems+=" runtime=${ms:-?}ms-recorded=${rec:-?}"
    status=ok; [ -n "$problems" ] && { status="FAIL:$problems"; fail=1; }
    echo "[$n/$total] $name: $status (client exit $code, $(( $(date +%s)-s ))s, run time ${ms:-?} ms, recorded $rec)"
    [ -n "$problems" ] && { echo "--- last log lines:"; tail -15 "$log" 2>/dev/null | tr -d '\r'; echo "---"; }
done
echo "all $total done in $(( $(date +%s)-t0 ))s" >&2
exit $fail
