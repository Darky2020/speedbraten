#!/usr/bin/env bash
# Links the packages of a resimdata pack ($RESIMDATA, e.g. resimdata/) into a harness home, so
# the client loads the gold replays' maps and models. Usage: RESIMDATA=<dir> gold_home.sh <home>
set -eu
mkdir -p "$1/packages"
for f in "$RESIMDATA"/packages/*; do ln -sfn "$f" "$1/packages/$(basename "$f")"; done
