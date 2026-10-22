#!/usr/bin/env bash
# Usage: race_home.sh <home dir>
# HOMESETUP for race_check.cfg: copies the race map nadefly into the home dir. It is not part of
# the game; RACE_MAPDIR (default: the macOS Sauerbraten home's packages/base) has to hold it.
set -eu
src=${RACE_MAPDIR:-"$HOME/Library/Application Support/Sauerbraten/packages/base"}
mkdir -p "$1/packages/base"
cp "$src"/nadefly.ogz "$1/packages/base/"
for f in "$src"/nadefly.cfg "$src"/nadefly.wpt; do [ -f "$f" ] && cp "$f" "$1/packages/base/"; done
exit 0
