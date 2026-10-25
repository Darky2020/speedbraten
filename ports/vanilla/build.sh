#!/usr/bin/env bash
# Usage: ports/vanilla/build.sh <zig target> <out dir> [version]
# Builds speedbraten on plain Sauerbraten (SVN r6880): exports that source, lays the core files
# (src/corefiles.txt) over it, applies ports/vanilla/shell.patch, checks the core with
# tools/corecheck.sh and builds it with tools/clientbuild.sh. Run from the repo root.
# Env: VANILLA_SRC=<dir> copies an existing export of r6880's src/ instead of running svn;
# WORK=<dir> is the build tree (default build/vanilla, replaced on every run).
set -eu
rev=6880
repo=$(pwd)
target=$1; out=$2; version=${3:-}
mkdir -p "$out"; out=$(cd "$out" && pwd)
work=${WORK:-$repo/build/vanilla}
t0=$(date +%s)

rm -rf "$work"; mkdir -p "$work"
echo "phase source: r$rev" >&2
if [ -n "${VANILLA_SRC:-}" ]; then cp -R "$VANILLA_SRC" "$work/src"
else svn export -q -r "$rev" svn://svn.code.sf.net/p/sauerbraten/code/src "$work/src"
fi

echo "phase core + shell" >&2
(cd src && grep -v -e '^#' -e '^$' corefiles.txt | while read -r entry; do
    case "$entry" in
        (*/) git ls-files -- "$entry" ;;
        (*) echo "$entry" ;;
    esac
done) | while read -r f; do
    mkdir -p "$work/src/$(dirname "$f")"
    cp "src/$f" "$work/src/$f"
done
cp src/corefiles.txt "$work/src/"
(cd "$work/src" && patch -p1 --no-backup-if-mismatch -s < "$repo/ports/vanilla/shell.patch")
if [ -n "$version" ]; then
    sed "s|<git-dev>|$version|" "$work/src/engine/main.cpp" > "$work/src/engine/main.cpp.tmp"
    mv "$work/src/engine/main.cpp.tmp" "$work/src/engine/main.cpp"
fi
bash tools/corecheck.sh "$work/src"

# the platform SDKs and libraries clientbuild.sh links against, at the paths the Makefile expects
ln -s "$repo/src/macos" "$work/src/macos"
ln -s "$repo/src/windows" "$work/src/windows"
ln -s "$repo/sauerbraten.app" "$work/sauerbraten.app"
ln -s "$repo/bin64" "$work/bin64"

echo "phase build $target" >&2
(cd "$work" && BASE=vanilla bash "$repo/tools/clientbuild.sh" "$target" "$out")
echo "vanilla build $target: done in $(( $(date +%s) - t0 ))s" >&2
