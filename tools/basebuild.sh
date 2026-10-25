#!/usr/bin/env bash
# Usage: basebuild.sh <base> <zig target> <out dir> [version]
# Builds the client on one base client: p1xbraten (this repo's tree) or vanilla (ports/vanilla).
# The version replaces "<git-dev>" in the build's version variable. Run from the repo root.
set -eu
base=$1; target=$2; out=$3; version=${4:-}
case $base in
    p1xbraten)
        if [ -n "$version" ]; then
            sed "s|<git-dev>|$version|" src/p1xbraten/version.cpp > src/p1xbraten/version.cpp.tmp
            mv src/p1xbraten/version.cpp.tmp src/p1xbraten/version.cpp
        fi
        bash tools/clientbuild.sh "$target" "$out"
        ;;
    vanilla) bash ports/vanilla/build.sh "$target" "$out" "$version" ;;
    *) echo "unknown base $base" >&2; exit 1 ;;
esac
