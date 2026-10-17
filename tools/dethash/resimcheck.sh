#!/usr/bin/env bash
# Usage: resimcheck.sh <outdir> <replay.sbr>
# Re-simulates a replay with sauer_resim and replays it in sauer_client, both with per-tick
# state hash logs, and checks that the two logs are identical for every replayed tick.
# Env: HOMESETUP=script run with each fresh home dir as its argument (e.g. to add map files).
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
mkdir -p "$1"
out=$(cd "$1" && pwd)
sbr=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
app=${SAUER_APP:-/Applications/Sauerbraten.app}
# keeps the client from taking focus from other apps on macOS (an SDL hint)
export SDL_MAC_BACKGROUND_APP=1

home=$out/resim
rm -rf "$home"; mkdir -p "$home"
[ -n "${HOMESETUP:-}" ] && "$HOMESETUP" "$home"
cp "$sbr" "$home/replay.sbr"
echo "resim: start" >&2
t=$(date +%s)
( cd "$app/Contents/Resources" && DYLD_FRAMEWORK_PATH="$app/Contents/Frameworks" \
    "$repo/src/sauer_resim" -q"$home" -o"$repo/data/overrides" -x'dethashstart dethash.log ""' replay.sbr > "$out/resim.json" 2> "$home/log.txt" )
code=$?
echo "resim: exit $code in $(( $(date +%s) - t ))s" >&2
cat "$out/resim.json"
ticks=$(sed -n 's/.*"ticks":\([0-9]*\).*/\1/p' "$out/resim.json")
[ -z "$ticks" ] && { echo "resim produced no result" >&2; exit 2; }

setup=$out/client-home.sh
printf '#!/bin/bash\n%s\ncp "%s" "$1/replay.sbr"\n' "${HOMESETUP:+\"$HOMESETUP\" \"\$1\"}" "$sbr" > "$setup"
chmod +x "$setup"
echo "client: start ($ticks ticks)" >&2
HOMESETUP=$setup TIMEOUT=$(( ticks / 200 + 120 )) "$here/run.sh" "$out/client" 1 0 1000 "$here/dump_replay.cfg" \
    "dethash_load = replay.sbr; dethash_length = $(( ticks * 5 + 1000 ))" 2>&1 | grep -v "frames in" >&2

a=$home/dethash.log; b=$out/client/run-1/dethash.log
n=$(grep -vc '^#' "$a")
if cmp -s <(grep -v '^#' "$a" | cut -d' ' -f2-) <(grep -v '^#' "$b" | head -"$n" | cut -d' ' -f2-); then
    echo "IDENTICAL: resim and client logs match for all $n logged ticks"
else
    echo "DIFFERS: first differing row:"
    diff <(grep -v '^#' "$a" | cut -d' ' -f2-) <(grep -v '^#' "$b" | head -"$n" | cut -d' ' -f2-) | head -4
    exit 1
fi
exit $code
