#!/usr/bin/env bash
# Usage: run.sh <label> <compiler...>
# Builds fpconform.cpp with each available compiler and writes <label>-<compiler>.txt
# into ./out, prefixed with the CPU model.
set -u
cd "$(dirname "$0")"
mkdir -p out
label=$1; shift

cpu=$( (sysctl -n machdep.cpu.brand_string 2>/dev/null) \
    || (grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2) \
    || (powershell -NoProfile -Command "(Get-CimInstance Win32_Processor).Name" 2>/dev/null) \
    || echo unknown)

for cc in "$@"; do
    exe=out/fp-$label-$cc
    case $cc in
        clang++|g++) cmd="$cc -std=c++11 -O3 -ffp-contract=off fpconform.cpp -o $exe" ;;
        zig)         cmd="zig c++ -std=c++11 -O3 -ffp-contract=off fpconform.cpp -o $exe" ;;
        cl)          cmd="cl -nologo -O2 -fp:precise -EHsc fpconform.cpp -Fe:$exe.exe" ;;
        *) echo "unknown compiler $cc" >&2; continue ;;
    esac
    echo "building: $cmd" >&2
    if ! $cmd >&2; then echo "build failed: $cc" >&2; continue; fi
    [ -f "$exe.exe" ] && exe=$exe.exe
    { echo "# label=$label compiler=$cc cpu=$(echo $cpu | xargs)"; "$exe"; } > "out/$label-$cc.txt"
    echo "done: out/$label-$cc.txt" >&2
done
