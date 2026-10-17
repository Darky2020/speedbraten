#!/usr/bin/env bash
# Usage: snapcycle.sh <outdir> [scenario...]
# Savestate check. Each scenario runs once normally and once with snapcycle 1 (the state is
# saved and restored at every tick, then saved again and compared). The hash logs of the two
# runs must be identical and the cycle must report 0 mismatches.
# Scenarios: flagrun race dmsp bots rocketjump settings (default: all).
mkdir -p "$1"; S=$(cd "$1" && pwd); shift
here=$(cd "$(dirname "$0")" && pwd); D=$here; cd "$here/../.."
names=(${@:-flagrun race dmsp bots rocketjump settings})
cfgof() { case $1 in flagrun) echo flagrun_replay;; race) echo race_check;; dmsp) echo dmsp_basic;; bots) echo bots_replay;; rocketjump) echo rocketjump_replay;; settings) echo settings_replay;; esac; }
extof() { [ $1 = rocketjump ] && echo "rj_recfps = 30; rj_repfps = 250"; }
t0=$(date +%s); n=${#names[@]}; k=0
for name in "${names[@]}"; do
  k=$((k+1)); t=$(date +%s)
  rep=""; [ $name = dmsp ] && rep=0
  for mode in base cycle; do
    extra="$(extof $name)"; [ $mode = cycle ] && extra="snapcycle 1; $extra"
    REPLAY=$rep TIMEOUT=150 "$D/run.sh" $S/sc/$name-$mode 1 1234 4 "$D/$(cfgof $name).cfg" "$extra" >/dev/null 2>&1
  done
  res=""
  for log in dethash.log dethash_replay.log; do
    a=$S/sc/$name-base/run-1/$log; b=$S/sc/$name-cycle/run-1/$log
    [ -f "$a" ] || continue
    if cmp -s "$a" "$b"; then res="$res $log identical ($(wc -l < $a | tr -d ' ') lines);"
    else res="$res $log DIFFERS at line $(cmp "$a" "$b" | awk '{print $NF}');"; fi
  done
  cyc=$(grep -ah "snapcycle:" $S/sc/$name-cycle/run-1/log.txt | tr '\n' ' ')
  el=$(( $(date +%s)-t0 ))
  echo "[$k/$n] $name ($(( $(date +%s)-t ))s, total ${el}s, eta $(( el*(n-k)/k ))s):$res $cyc"
done
echo "all done in $(( $(date +%s)-t0 ))s"
