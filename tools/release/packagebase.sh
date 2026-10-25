#!/usr/bin/env bash
# Usage: tools/release/packagebase.sh <base> <artifacts dir> <dist dir> <version>
# Packs the vanilla or wcng client builds downloaded to <artifacts dir>/client-<platform>-<base> into
# <dist dir>/speedbraten-<version>-<platform>-<base>.{zip,tar.gz}. Run from the repo root on macOS.
# Every file carries the base in its name (binary, launcher, overrides folder), so the packages of
# several bases can be unpacked into one Sauerbraten folder next to the p1xbraten one.
set -eu
base=$1; art=$2; dist=$3; v=$4
case $base in
    vanilla) label="Vanilla" ;;
    wcng) label="wc-ng" ;;
    *) echo "unknown base $base" >&2; exit 1 ;;
esac
repo=$(pwd)
mkdir -p "$dist"; dist=$(cd "$dist" && pwd)
pkg=$(mktemp -d "${TMPDIR:-/tmp}/packagebase.XXXXXX")
trap 'rm -rf "$pkg"' EXIT
overrides=speedbraten_overrides_$base

# speedbraten's file patches, its menus (built into the p1xbraten client, files here) and the base's own files
o=$pkg/overrides
mkdir -p "$o/data/speedbraten"
cp -R data/overrides/. "$o/"
cp data/p1xbraten/speedbraten.cfg "$o/data/speedbraten/"
cp -R "ports/$base/data/." "$o/data/"
if [ $base = wcng ]; then
    # wc-ng's data (CA bundle for HTTPS, G15, sound cfgs) and packages (sounds, country flags), plus its
    # optional menu files for players to copy into their home folder
    commit=$(sed -n 's/^commit=//p' ports/wcng/build.sh)
    git init -q "$pkg/wc-ng"
    git -C "$pkg/wc-ng" fetch -q --depth 1 https://github.com/tpoechtrager/wc-ng "$commit"
    git -C "$pkg/wc-ng" checkout -q FETCH_HEAD
    cp -R "$pkg/wc-ng/data/." "$o/data/"
    cp -R "$pkg/wc-ng/packages" "$o/"
    mkdir -p "$o/wc-ng-menu"; cp -R "$pkg/wc-ng/menu/." "$o/wc-ng-menu/"
    cp "$pkg/wc-ng/COPYING" "$o/wc-ng-COPYING"
fi

w=$pkg/windows; mkdir -p "$w/bin64_speedbraten"
cp "$art/client-windows-x86_64-$base/speedbraten_x86_64_client.exe" "$w/bin64_speedbraten/speedbraten_${base}_x86_64_client.exe"
for d in bin64/*.dll; do case $d in *EOSSDK*) ;; *) cp "$d" "$w/bin64_speedbraten/" ;; esac; done
sed "s|speedbraten_x86_64_client.exe|speedbraten_${base}_x86_64_client.exe -o$overrides|" speedbraten.bat > "$w/speedbraten-$base.bat"
cp -R "$o" "$w/$overrides"
(cd "$w" && zip -qr "$dist/speedbraten-$v-windows-x64-$base.zip" .)

for a in x86_64 aarch64; do
    l=$pkg/linux-$a; mkdir -p "$l/bin_unix"
    cp "$art/client-linux-$a-$base/speedbraten_${a}_client" "$l/bin_unix/speedbraten_${base}_${a}_client"
    chmod +x "$l/bin_unix/"*
    sed -e "s|speedbraten_aarch64_client|speedbraten_${base}_aarch64_client|; s|speedbraten_x86_64_client|speedbraten_${base}_x86_64_client|" \
        -e "s|-glog.txt|-o$overrides -glog.txt|" speedbraten.sh > "$l/speedbraten-$base.sh"
    chmod +x "$l/speedbraten-$base.sh"
    cp -R "$o" "$l/$overrides"
    tar czf "$dist/speedbraten-$v-linux-$a-$base.tar.gz" -C "$l" .
done

name="Speedbraten $label"
app="$pkg/mac/$name.app/Contents"; mkdir -p "$app/MacOS" "$app/Resources"
cp tools/release/macos/speedbraten "$art/client-macos-universal-$base/speedbraten_universal" "$app/MacOS/"
chmod +x "$app/MacOS/"*
cp -R "$o" "$app/Resources/speedbraten_overrides"
tar xf "$art/client-macos-universal-$base/Frameworks.tar" -C "$app"
sed -e "s/VERSION/${v#v}/" -e "s|<string>Speedbraten</string>|<string>$name</string>|g" \
    -e "s|io.github.darky2020.speedbraten|io.github.darky2020.speedbraten.$base|" tools/release/macos/Info.plist > "$app/Info.plist"
codesign --force --deep --sign - "$pkg/mac/$name.app"
(cd "$pkg/mac" && ditto -c -k --keepParent "$name.app" "$dist/speedbraten-$v-macos-$base.zip")
ls -la "$dist" | grep -- "-$base"
