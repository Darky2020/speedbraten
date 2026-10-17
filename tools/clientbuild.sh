#!/usr/bin/env bash
# Usage: clientbuild.sh <zig target> <out dir>
# Builds the client with zig for x86_64-windows-gnu, x86_64-linux-gnu, aarch64-linux-gnu,
# x86_64-macos or aarch64-macos, and copies it to <out dir>/speedbraten_<arch>_client[.exe].
# Run from the repo root.
# - Windows links against the SDL2/zlib import libraries in src/windows/lib64.
# - Linux runs natively on the target architecture against the distro's SDL2 dev packages
#   (libsdl2-dev, libsdl2-image-dev, libsdl2-mixer-dev, zlib1g-dev), with glibc 2.31 symbols.
# - macOS links against the frameworks in sauerbraten.app/Contents/Frameworks.
set -eu
target=$1; out=$2
arch=${target%%-*}
jobs=$( (nproc || sysctl -n hw.ncpu) 2>/dev/null || echo 2)
mkdir -p "$out"
out=$(cd "$out" && pwd)
cd src
make clean > /dev/null 2>&1 || true
t0=$(date +%s)
echo "phase client $target: start" >&2
case $target in
    *windows*)
        make -j"$jobs" client USEZIG=1 TARGET="$target"
        cp sauer_client "$out/speedbraten_${arch}_client.exe"
        ;;
    *linux*)
        t=$target.2.31
        lib=/usr/lib/$arch-linux-gnu
        rm -rf sysinc; mkdir -p sysinc/arch
        ln -s /usr/include/SDL2 sysinc/SDL2
        ln -s /usr/include/zlib.h /usr/include/zconf.h /usr/include/X11 sysinc/
        ln -s "/usr/include/$arch-linux-gnu/SDL2" sysinc/arch/SDL2
        make -j"$jobs" client USEZIG=1 TARGET="$t" CFLAGS="--target=$t -O2 -fno-fast-math -ffp-contract=off" \
            INCLUDES="-Ishared -Iengine -Ifpsgame -Ip1xbraten -Ienet/include" \
            CLIENT_INCLUDES="-Isysinc -Isysinc/SDL2 -Isysinc/arch -D_REENTRANT" \
            LIBS="-Lenet -lenet -L$lib -lz" \
            CLIENT_LIBS="-L$lib -lSDL2 -lSDL2_image -lSDL2_mixer -lGL -lX11"
        rm -rf sysinc
        cp sauer_client "$out/speedbraten_${arch}_client"
        ;;
    *macos*)
        make -C enet CC='zig cc' AR='zig ar' CFLAGS="--target=$target -O2 -fno-fast-math -ffp-contract=off -mmacosx-version-min=11.0"
        make -j"$jobs" client USEZIG=1 TARGET="$target" OSXMIN=11.0 \
            INCLUDES="-Ishared -Iengine -Ifpsgame -Ip1xbraten -Ienet/include -idirafter macos/sdk_11.3/usr/include" \
            LIBS="-Lmacos/sdk_11.3/usr/lib -lz -Lenet -lenet"
        cp sauer_client "$out/speedbraten_${arch}_client"
        ;;
    *) echo "unsupported target $target" >&2; exit 1 ;;
esac
echo "phase client $target: done in $(( $(date +%s) - t0 ))s" >&2
