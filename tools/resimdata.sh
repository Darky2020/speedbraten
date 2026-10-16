#!/usr/bin/env bash
# Usage: resimdata.sh <game dir> <out.tar.gz> <map>...
# Packs the game data sauer_resim needs to re-simulate replays on the given maps: data/, every
# file under packages/ except images, sounds and maps, plus packages/base/<map>.* for each
# map. sauer_resim never decodes images or plays sounds, so those are left out.
# <game dir> is the folder holding data/ and packages/ (on macOS:
# /Applications/Sauerbraten.app/Contents/Resources).
set -eu
src=$(cd "$1" && pwd); out=$2; shift 2
[ $# -gt 0 ] || { echo "no maps given" >&2; exit 1; }
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
root=$tmp/resimdata
mkdir -p "$root/packages/base"
cp -R "$src/data" "$root/"
( cd "$src" && find packages -type f \
    ! -iname '*.png' ! -iname '*.jpg' ! -iname '*.jpeg' ! -iname '*.tga' ! -iname '*.dds' ! -iname '*.bmp' \
    ! -iname '*.ogg' ! -iname '*.wav' ! -iname '*.ogz' ! -path 'packages/base/*' -print0 \
    | while IFS= read -r -d '' f; do mkdir -p "$root/$(dirname "$f")"; cp "$f" "$root/$f"; done )
for m in "$@"; do
    ls "$src/packages/base/$m".ogz >/dev/null
    for f in "$src/packages/base/$m".*; do
        case $f in *.jpg|*.png) ;; *) cp "$f" "$root/packages/base/" ;; esac
    done
done
tar czf "$out" -C "$tmp" resimdata
echo "$out: $(du -h "$out" | cut -f1), $(find "$root" -type f | wc -l | tr -d ' ') files, maps: $*" >&2
