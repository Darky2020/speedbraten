#!/usr/bin/env bash
# Usage: resimbuild.sh <zig target> <out dir>
# Builds sauer_resim for a zig target (x86_64-linux-gnu, aarch64-linux-gnu, x86_64-windows-gnu,
# x86_64-macos, aarch64-macos) with zig as the only toolchain: zlib and enet are compiled from
# source for the target, so no platform libraries are needed. Run from the repo root.
set -eu
target=$1; out=$2
repo=$(pwd)
ZLIB_VERSION=1.3.1
ZLIB_SHA256=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
jobs=$( (nproc || sysctl -n hw.ncpu) 2>/dev/null || echo 2)

mkdir -p "$out"
out=$(cd "$out" && pwd)
deps=$out/deps-$target
mkdir -p "$deps/include" "$deps/lib" "$deps/build"
t0=$(date +%s)

echo "phase zlib: start" >&2
if [ ! -f "$deps/lib/libz.a" ]; then
    tarball=$out/zlib-$ZLIB_VERSION.tar.gz
    [ -f "$tarball" ] || curl -fsSL -o "$tarball" "https://zlib.net/fossils/zlib-$ZLIB_VERSION.tar.gz"
    echo "$ZLIB_SHA256  $tarball" | (sha256sum -c - 2>/dev/null || shasum -a 256 -c -) >&2
    tar xzf "$tarball" -C "$deps/build"
    zsrc=$deps/build/zlib-$ZLIB_VERSION
    for f in adler32 compress crc32 deflate gzclose gzlib gzread gzwrite infback inffast inflate inftrees trees uncompr zutil; do
        zig cc -target "$target" -O2 -c "$zsrc/$f.c" -o "$deps/build/z_$f.o"
    done
    zig ar rcs "$deps/lib/libz.a" "$deps"/build/z_*.o
    cp "$zsrc/zlib.h" "$zsrc/zconf.h" "$deps/include/"
fi
echo "phase zlib: done in $(( $(date +%s) - t0 ))s" >&2

echo "phase enet: start" >&2
case $target in
    *windows*) enetdefs="" ;;
    *linux*) enetdefs="-DHAS_GETADDRINFO -DHAS_GETNAMEINFO -DHAS_GETHOSTBYADDR_R -DHAS_GETHOSTBYNAME_R -DHAS_POLL -DHAS_FCNTL -DHAS_INET_PTON -DHAS_INET_NTOP -DHAS_MSGHDR_FLAGS -DHAS_SOCKLEN_T" ;;
    *) enetdefs="-DHAS_GETADDRINFO -DHAS_GETNAMEINFO -DHAS_POLL -DHAS_FCNTL -DHAS_INET_PTON -DHAS_INET_NTOP -DHAS_MSGHDR_FLAGS -DHAS_SOCKLEN_T" ;;
esac
rm -f "$deps"/build/enet_*.o
for f in callbacks compress host list packet peer protocol unix win32; do
    zig cc -target "$target" -O2 -fno-fast-math -ffp-contract=off $enetdefs -I"$repo/src/enet/include" -c "$repo/src/enet/$f.c" -o "$deps/build/enet_$f.o"
done
zig ar rcs "$deps/lib/libenet.a" "$deps"/build/enet_*.o
echo "phase enet: done in $(( $(date +%s) - t0 ))s" >&2

echo "phase resim: start ($jobs jobs)" >&2
make -C src -j"$jobs" resim USEZIG=1 TARGET="$target" RESIM_DEPS="$deps" OSXMIN=11.0 > "$out/build-$target.log" 2>&1 || {
    grep -E "error|undefined" "$out/build-$target.log" | head -40 >&2
    echo "phase resim: FAILED, log: $out/build-$target.log" >&2
    exit 1
}
for f in src/sauer_resim src/sauer_resim.exe; do [ -f "$f" ] && mv "$f" "$out/sauer_resim-$target${f##*sauer_resim}"; done
rm -rf src/headless
echo "phase resim: done in $(( $(date +%s) - t0 ))s" >&2
ls -la "$out"/sauer_resim-"$target"* >&2
