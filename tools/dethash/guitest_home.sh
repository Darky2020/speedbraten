#!/usr/bin/env bash
# Puts a copy of complex named guitest into a harness home, with a map cfg that defines a
# map GUI (map GUIs can only come from a map cfg). Usage: guitest_home.sh <home>
set -eu
app=${SAUER_APP:-/Applications/Sauerbraten.app}
mkdir -p "$1/packages/base"
cp "$app/Contents/Resources/packages/base/complex.ogz" "$1/packages/base/guitest.ogz"
cat > "$1/packages/base/guitest.cfg" <<'CFG'
exec "packages/base/complex.cfg"
guitest_clicks = 0
guitest_closed = 0
guitest_boost = [
    guitest_clicks = (+ $guitest_clicks 1)
    sleep 200 [harnessaim 0 30]
]
newgui guitest [
    guionclear [guitest_closed = (+ $guitest_closed 1)]
    guibutton "boost" [guitest_boost]
    guibutton "close" [cleargui]
]
CFG
