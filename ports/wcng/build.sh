#!/usr/bin/env bash
# Usage: ports/wcng/build.sh <zig target> <out dir> [version]
# Builds speedbraten on wc-ng (github.com/tpoechtrager/wc-ng at a pinned commit): fetches that
# source, lays the core files (src/corefiles.txt) over it, applies ports/wcng/shell.patch, checks
# the core with tools/corecheck.sh and builds it with tools/clientbuild.sh. Run from the repo root.
# Env: WCNG_SRC=<dir> copies an existing checkout's src/ instead of fetching; WCNGDEPS=<dir> uses
# libraries already built by tools/wcngdeps.sh for this target (default: builds them into
# build/wcngdeps/<target>); WORK=<dir> is the build tree (default build/wcng, replaced every run).
set -eu
commit=09834ca6b91d79f8929629296d2d069f7a78d8b1
# wc-ng's version is 1.0.<commits up to and including $commit>
commitcount=289
repo=$(pwd)
target=$1; out=$2; version=${3:-}
mkdir -p "$out"; out=$(cd "$out" && pwd)
work=${WORK:-$repo/build/wcng}
t0=$(date +%s)

rm -rf "$work"; mkdir -p "$work"
echo "phase source: wc-ng ${commit:0:7}" >&2
if [ -n "${WCNG_SRC:-}" ]; then cp -R "$WCNG_SRC" "$work/src"
else
    git init -q "$work/wc-ng"
    git -C "$work/wc-ng" fetch -q --depth 1 https://github.com/tpoechtrager/wc-ng "$commit"
    git -C "$work/wc-ng" checkout -q FETCH_HEAD
    mv "$work/wc-ng/src" "$work/src"; rm -rf "$work/wc-ng"
fi
sed -e "s/__GIT_REVISION__/${commit:0:7}/g" -e "s/__GIT_COMMIT_COUNT__/$commitcount/g" \
    "$work/src/mod/wcversion.h.in" > "$work/src/mod/wcversion.h"

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
(cd "$work/src" && patch -p1 --no-backup-if-mismatch -s < "$repo/ports/wcng/shell.patch")
if [ -n "$version" ]; then
    sed "s|<git-dev>|$version|" "$work/src/engine/main.cpp" > "$work/src/engine/main.cpp.tmp"
    mv "$work/src/engine/main.cpp.tmp" "$work/src/engine/main.cpp"
fi
bash tools/corecheck.sh "$work/src"

if [ -z "${WCNGDEPS:-}" ]; then
    WCNGDEPS=$repo/build/wcngdeps/$target
    [ -f "$WCNGDEPS/lib/libcurl.a" ] || bash tools/wcngdeps.sh "$target" "$WCNGDEPS"
fi
export WCNGDEPS

# the platform SDKs and libraries clientbuild.sh links against, at the paths the Makefile expects
ln -s "$repo/src/macos" "$work/src/macos"
ln -s "$repo/src/windows" "$work/src/windows"
ln -s "$repo/sauerbraten.app" "$work/sauerbraten.app"
ln -s "$repo/bin64" "$work/bin64"

echo "phase build $target" >&2
(cd "$work" && BASE=wcng bash "$repo/tools/clientbuild.sh" "$target" "$out")
echo "wc-ng build $target: done in $(( $(date +%s) - t0 ))s" >&2
