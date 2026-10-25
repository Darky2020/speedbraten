#!/usr/bin/env bash
# Usage: corecheck.sh <src dir of a build tree>
# Fails unless every file listed in src/corefiles.txt is present in the build tree and identical
# to this repo's copy. Run it on a port's tree after the core has been laid over the base client.
set -u
repo=$(cd "$(dirname "$0")/.." && pwd)
tree=${1:?usage: corecheck.sh <src dir of a build tree>}
[ -d "$tree" ] || { echo "corecheck: no such directory: $tree" >&2; exit 2; }

files=$(cd "$repo/src" && grep -v -e '^#' -e '^$' corefiles.txt | while read -r entry; do
    case "$entry" in
        (*/) git ls-files -- "$entry" ;;
        (*) echo "$entry" ;;
    esac
done)

checked=0; bad=0
for f in $files; do
    checked=$((checked+1))
    if [ ! -f "$repo/src/$f" ]; then echo "corecheck: listed but not in this repo: $f" >&2; bad=$((bad+1))
    elif [ ! -f "$tree/$f" ]; then echo "corecheck: missing: $f" >&2; bad=$((bad+1))
    elif ! cmp -s "$repo/src/$f" "$tree/$f"; then echo "corecheck: differs: $f" >&2; bad=$((bad+1))
    fi
done
echo "corecheck: $checked core files, $bad problems"
[ "$bad" -eq 0 ]
