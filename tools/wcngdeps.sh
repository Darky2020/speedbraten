#!/usr/bin/env bash
# Usage: wcngdeps.sh <zig target> <out dir>
# Builds the static libraries wc-ng's client links (libmaxminddb, LibreSSL, libcurl, and dlfcn-win32
# on Windows; the versions wc-ng-libs uses) with zig for x86_64-windows-gnu, x86_64-linux-gnu,
# aarch64-linux-gnu, x86_64-macos or aarch64-macos, into <out dir>/include and <out dir>/lib.
# Env: ZIG=<zig binary> (default zig); DEPS_CACHE=<dir> keeps the downloaded sources (default
# build/wcngdeps-src); a build log per library goes to <out dir>/logs.
set -eu
target=$1; out=$2
zig=${ZIG:-zig}
repo=$(cd "$(dirname "$0")/.." && pwd)
cache=${DEPS_CACHE:-$repo/build/wcngdeps-src}
mkdir -p "$out" "$cache"; out=$(cd "$out" && pwd); cache=$(cd "$cache" && pwd)
jobs=$( (nproc || sysctl -n hw.ncpu) 2>/dev/null || echo 2)

maxminddb=(libmaxminddb-1.12.2 https://github.com/maxmind/libmaxminddb/releases/download/1.12.2/libmaxminddb-1.12.2.tar.gz 1bfbf8efba3ed6462e04e225906ad5ce5fe958aa3d626a1235b2a2253d600743)
libressl=(libressl-4.0.0 https://ftp.openbsd.org/pub/OpenBSD/LibreSSL/libressl-4.0.0.tar.gz 4d841955f0acc3dfc71d0e3dd35f283af461222350e26843fea9731c0246a1e4)
dlfcn=(dlfcn-win32-1.0.0 https://github.com/dlfcn-win32/dlfcn-win32/archive/v1.0.0.tar.gz 36f2e7ef1f1ba04f6ce682a71937eaddd3d6994f09e29df2c7578ec524e47450)
curl=(curl-8.13.0 https://curl.se/download/curl-8.13.0.tar.gz c261a4db579b289a7501565497658bbd52d3138fdbaccf1490fa918129ab45bc)

ztarget=$target
case $target in
    x86_64-windows-gnu) host=x86_64-w64-mingw32; os=windows ;;
    x86_64-linux-gnu)   host=x86_64-pc-linux-gnu; os=linux; ztarget=$target.2.31 ;;
    aarch64-linux-gnu)  host=aarch64-unknown-linux-gnu; os=linux; ztarget=$target.2.31 ;;
    x86_64-macos)       host=x86_64-apple-darwin; os=macos; ztarget=$target.11.0 ;;
    aarch64-macos)      host=aarch64-apple-darwin; os=macos; ztarget=$target.11.0 ;;
    *) echo "unsupported target $target" >&2; exit 1 ;;
esac
# without a GNU-compatible linker libtool assumes MSVC's tools (link, lib) for a mingw target
[ $os = windows ] && export LD="$zig ld.lld" NM=nm
# curl's configure insists on a resource compiler, used only for its DLL
[ $os = windows ] && export RC=true
export CC="$zig cc -target $ztarget" AR="$zig ar" RANLIB="$zig ranlib"
export CFLAGS="-O2 -fPIC" CPPFLAGS="-I$out/include" LDFLAGS="-L$out/lib"
# zig fortifies glibc calls at -O2 with checks (__inet_pton_chk) newer than the glibc 2.31 we target
[ $os = linux ] && export CFLAGS="$CFLAGS -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0"
# --build differs from --host, so configure never runs what it compiles
common=(--host=$host --build=none-unknown-none --prefix="$out" --disable-shared --enable-static)

fetch() {
    local name=$1 url=$2 sum=$3 file="$cache/$1.tar.gz"
    if [ ! -f "$file" ]; then curl -fsSL -o "$file.tmp" "$url" && mv "$file.tmp" "$file"; fi
    echo "$sum  $file" | (sha256sum -c - 2>/dev/null || shasum -a 256 -c -) > /dev/null || { echo "checksum mismatch: $file" >&2; exit 1; }
    rm -rf "$work/$name"; tar xzf "$file" -C "$work"
}

phase() { echo "phase $1 $target: $2${3:+ in $3s}" >&2; }

work=$(mktemp -d "${TMPDIR:-/tmp}/wcngdeps.XXXXXX")
trap 'rm -rf "$work"' EXIT
mkdir -p "$out/logs"
t0=$(date +%s)

t=$(date +%s); phase maxminddb start
fetch "${maxminddb[@]}"
(cd "$work/${maxminddb[0]}" && ./configure "${common[@]}" --disable-tests && \
 printf '#undef MMDB_UINT128_USING_MODE\n#undef MMDB_UINT128_IS_BYTE_ARRAY\n#define MMDB_UINT128_USING_MODE 0\n#define MMDB_UINT128_IS_BYTE_ARRAY 1\n' >> include/maxminddb_config.h && \
 make -C src -j"$jobs" install && cp include/maxminddb_config.h "$out/include/") > "$out/logs/maxminddb.log" 2>&1 || { tail -30 "$out/logs/maxminddb.log" >&2; exit 1; }
phase maxminddb done $(( $(date +%s) - t ))

t=$(date +%s); phase libressl start
fetch "${libressl[@]}"
(cd "$work/${libressl[0]}" && ./configure "${common[@]}" --disable-asm --disable-tests && \
 make -C crypto -j"$jobs" install && make -C ssl -j"$jobs" install && make -C include install) > "$out/logs/libressl.log" 2>&1 || { tail -30 "$out/logs/libressl.log" >&2; exit 1; }
phase libressl done $(( $(date +%s) - t ))

t=$(date +%s); phase curl start
fetch "${curl[@]}"
curllibs=""; [ $os = windows ] && curllibs="-lbcrypt -lws2_32 -lcrypt32"
# with IPv6, curl links Apple frameworks on macOS that zig has no stubs for
curlopts=(); [ $os = macos ] && curlopts=(--disable-ipv6)
(cd "$work/${curl[0]}" && LIBS="$curllibs" ./configure "${common[@]}" --with-openssl="$out" --enable-threaded-resolver \
    --disable-manual --disable-ldap --disable-ldaps --disable-rtsp --disable-docs --without-libssh2 --without-libidn2 \
    --without-librtmp --without-brotli --without-winidn --without-libpsl --without-zstd --without-nghttp2 --without-zlib ${curlopts[@]+"${curlopts[@]}"} && \
 make -C lib -j"$jobs" install && make -C include install) > "$out/logs/curl.log" 2>&1 || { tail -30 "$out/logs/curl.log" >&2; exit 1; }
phase curl done $(( $(date +%s) - t ))

if [ $os = windows ]; then
    t=$(date +%s); phase dlfcn start
    fetch "${dlfcn[@]}"
    (cd "$work/${dlfcn[0]}" && $CC $CFLAGS -c dlfcn.c -o dlfcn.o && $AR rcs "$out/lib/libdl.a" dlfcn.o && cp dlfcn.h "$out/include/") \
        > "$out/logs/dlfcn.log" 2>&1 || { tail -30 "$out/logs/dlfcn.log" >&2; exit 1; }
    phase dlfcn done $(( $(date +%s) - t ))
fi

echo "wcngdeps $target: $(ls "$out/lib"/*.a | xargs -n1 basename | tr '\n' ' ')in $(( $(date +%s) - t0 ))s" >&2
