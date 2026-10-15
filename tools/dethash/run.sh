#!/usr/bin/env bash
# Usage: run.sh <outdir> <runs> <seed> [drawinterval] [script]
# Runs the freshly built client <runs> times in a row with a fixed clock, each with a
# fresh home dir, and leaves <outdir>/run-N/dethash.log for compare.py.
# seed 0 keeps the game's own time-based RNG seed.
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
out=$1; runs=$2; seed=$3; draw=${4:-4}; script=${5:-$here/dmsp_basic.cfg}
app=${SAUER_APP:-/Applications/Sauerbraten.app}
client=$repo/src/sauer_client
expected=6000
timeout=300

mkdir -p "$out"
out=$(cd "$out" && pwd)
export DYLD_FRAMEWORK_PATH="$app/Contents/Frameworks"
cd "$app/Contents/Resources" || exit 1

t0=$(date +%s)
for n in $(seq 1 "$runs"); do
    home=$out/run-$n
    rm -rf "$home"; mkdir -p "$home"
    log=$home/dethash.log
    echo "run $n/$runs: start (seed=$seed draw=$draw)" >&2
    rs=$(date +%s)
    "$client" -q"$home" -glog.txt -x"dethash_seed = $seed; dethash_draw = $draw; exec \"$script\"" &
    pid=$!
    while kill -0 $pid 2>/dev/null; do
        sleep 5
        el=$(( $(date +%s) - rs ))
        frames=0; [ -f "$log" ] && frames=$(wc -l < "$log" | tr -d ' ')
        if [ "$frames" -gt 1 ]; then
            rate=$(( frames / el ))
            eta=unknown; [ "$rate" -gt 0 ] && eta=$(( (expected - frames) / rate ))s
            echo "run $n/$runs: $frames/~$expected frames in ${el}s, ${rate}/s, eta $eta" >&2
        else
            echo "run $n/$runs: loading, ${el}s" >&2
        fi
        if [ "$el" -gt "$timeout" ]; then echo "run $n/$runs: timeout, killing" >&2; kill $pid; fi
    done
    wait $pid; code=$?
    lines=0; [ -f "$log" ] && lines=$(wc -l < "$log" | tr -d ' ')
    echo "run $n/$runs: done in $(( $(date +%s) - rs ))s, exit $code, $lines lines" >&2
done
echo "all runs done in $(( $(date +%s) - t0 ))s" >&2
