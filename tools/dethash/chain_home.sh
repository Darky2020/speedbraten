#!/usr/bin/env bash
# Puts two copies of complex into a harness home: chaina switches to chainb from its own map
# script after 1 s, chainb ends the SP run (endsp) after 0.7 s. Usage: chain_home.sh <home>
set -eu
app=${SAUER_APP:-/Applications/Sauerbraten.app}
mkdir -p "$1/packages/base"
for m in chaina chainb; do cp "$app/Contents/Resources/packages/base/complex.ogz" "$1/packages/base/$m.ogz"; done
printf 'exec "packages/base/complex.cfg"\nsleep 1000 [sp chainb]\n' > "$1/packages/base/chaina.cfg"
printf 'exec "packages/base/complex.cfg"\nsleep 700 [endsp]\n' > "$1/packages/base/chainb.cfg"
