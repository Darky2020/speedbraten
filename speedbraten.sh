#!/bin/sh
# Starts speedbraten from a Sauerbraten 2020 folder. Arguments are passed on to the client;
# -q<dir> replaces the default home directory (~/.sauerbraten, shared with Sauerbraten).

cd "$(dirname "$0")"

case "$(uname -m)" in
    aarch64|arm64) client=./bin_unix/speedbraten_aarch64_client ;;
    *) client=./bin_unix/speedbraten_x86_64_client ;;
esac

home="-q${HOME}/.sauerbraten"
for arg in "$@"; do
    case "$arg" in -q*) home= ;; esac
done

if [ "${XDG_SESSION_TYPE}" = wayland ]; then
    export SDL_VIDEODRIVER=wayland
fi

exec "$client" ${home:+"$home"} -glog.txt "$@"
