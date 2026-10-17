#!/usr/bin/env bash
# Usage: run.sh <outdir> <runs> <seed> [drawinterval] [script] [extra cubescript]
# Runs the freshly built client <runs> times in a row with a fixed clock, each with a
# fresh home dir, and leaves <outdir>/run-N/dethash.log for compare.py.
# seed 0 uses a random gameplay seed, as in normal play. extra runs before the script.
# Env: FIXED=ms per frame (default 5, 0 = wall clock), STALL=ms to freeze twice mid-run,
# REPLAY=0|1 to replay the current|previous attempt from its recorded input afterwards,
# HOMESETUP=script run with the fresh home dir as its argument before each launch.
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
out=$1; runs=$2; seed=$3; draw=${4:-4}; script=${5:-$here/dmsp_basic.cfg}; extra=${6:-}
app=${SAUER_APP:-/Applications/Sauerbraten.app}
client=$repo/src/sauer_client
expected=6000
timeout=${TIMEOUT:-300}

mkdir -p "$out"
out=$(cd "$out" && pwd)
export DYLD_FRAMEWORK_PATH="$app/Contents/Frameworks"
# keeps the client from taking focus from other apps on macOS (an SDL hint)
export SDL_MAC_BACKGROUND_APP=1
# starts the window minimized; screenshots need SAUER_MINIMIZED=0 (a minimized window is not drawn)
export SAUER_MINIMIZED=${SAUER_MINIMIZED:-1}
cd "$app/Contents/Resources" || exit 1

t0=$(date +%s)
for n in $(seq 1 "$runs"); do
    home=$out/run-$n
    rm -rf "$home"; mkdir -p "$home"
    printf 'fullscreen 0\nscr_w 640\nscr_h 400\nusesound 0\n' > "$home/init.cfg"
    [ -n "${HOMESETUP:-}" ] && "$HOMESETUP" "$home"
    log=$home/dethash.log
    echo "run $n/$runs: start (seed=$seed draw=$draw extra='$extra' fixed=${FIXED:-5} stall=${STALL:-0})" >&2
    rs=$(date +%s)
    "$client" -q"$home" -glog.txt -x"dethash_seed = $seed; dethash_draw = $draw; dethash_fixed = ${FIXED:-5}; dethash_stall = ${STALL:-0}; dethash_replay = \"${REPLAY:-}\"; $extra; exec \"$script\"" &
    pid=$!
    beat=0
    while kill -0 $pid 2>/dev/null; do
        sleep 1
        beat=$(( beat + 1 ))
        el=$(( $(date +%s) - rs ))
        if [ "$el" -gt "$timeout" ]; then echo "run $n/$runs: timeout, killing" >&2; kill $pid; fi
        [ $(( beat % 5 )) -eq 0 ] || continue
        frames=0; [ -f "$log" ] && frames=$(wc -l < "$log" | tr -d ' ')
        if [ "$frames" -gt 1 ]; then
            rate=$(( frames / el ))
            eta=unknown; [ "$rate" -gt 0 ] && eta=$(( (expected - frames) / rate ))s
            echo "run $n/$runs: $frames/~$expected frames in ${el}s, ${rate}/s, eta $eta" >&2
        else
            echo "run $n/$runs: loading, ${el}s" >&2
        fi
    done
    wait $pid; code=$?
    lines=0; [ -f "$log" ] && lines=$(wc -l < "$log" | tr -d ' ')
    echo "run $n/$runs: done in $(( $(date +%s) - rs ))s, exit $code, $lines lines" >&2
done
echo "all runs done in $(( $(date +%s) - t0 ))s" >&2
