#!/bin/bash
# Builds FreeRDP from source as static libraries into a private prefix, to
# develop and test xrdp on a host whose desktop uses the distribution's
# FreeRDP (gnome-remote-desktop links libfreerdp3.so.3 and libwinpr3.so.3).
#
# A FreeRDP built from git has the same SONAMEs as the packaged one, and
# /usr/local/lib comes before /usr/lib in the loader's search path, so a
# shared "make install" replaces the libraries the other programs load. A
# static build installs no shared objects: the client it builds, and any
# program linked against it, carries its own copy. The copy's symbols are
# hidden (FreeRDP builds with -fvisibility=hidden and exports nothing
# without WINPR_DLL), so it does not clash with a libfreerdp3.so loaded in
# the same process either.
#
#   wlup/build-freerdp-static.sh [options]
#
#   --prefix DIR  install into DIR (default ~/.cache/xrdp-freerdp-static/<commit>);
#                 /usr and /usr/local are refused
#   --commit SHA  the FreeRDP commit (default: the one wlup/test/freerdp-av1 pins)
#   --src DIR     take the commit from this FreeRDP git checkout (git archive)
#                 instead of downloading it from GitHub
#   --av1         build the client with the AV1 decoder (dav1d, required)
#   --no-client   build the libraries only
#
# Then:
#
#   $PREFIX/bin/xfreerdp /v:...      the client, needs no libfreerdp3.so
#   PKG_CONFIG_PATH=$PREFIX/lib/pkgconfig \
#       pkg-config --static --cflags --libs freerdp-server3
#
# Optional features follow what pkg-config finds on the host. The build
# directory is kept next to the prefix for incremental rebuilds. Uses Ninja
# and ccache when they are installed.
set -euo pipefail

SRC=$(cd "$(dirname "$0")/.." && pwd)
COMMIT=$(sed -n 's/^ARG FREERDP_COMMIT=//p' "$SRC/wlup/test/freerdp-av1/Dockerfile")
PREFIX=
FREERDP_SRC=
AV1=no
CLIENT=yes

while [ $# -gt 0 ]; do
    case "$1" in
        --prefix) PREFIX=$2; shift 2 ;;
        --commit) COMMIT=$2; shift 2 ;;
        --src) FREERDP_SRC=$2; shift 2 ;;
        --av1) AV1=yes; shift ;;
        --no-client) CLIENT=no; shift ;;
        -h|--help) sed -n '2,/^set -euo/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $1 (see --help)" >&2; exit 1 ;;
    esac
done

if [ -n "$FREERDP_SRC" ]; then
    COMMIT=$(git -C "$FREERDP_SRC" rev-parse --verify "$COMMIT^{commit}")
fi
PREFIX=${PREFIX:-$HOME/.cache/xrdp-freerdp-static/${COMMIT:0:12}}
mkdir -p "$PREFIX"
PREFIX=$(cd "$PREFIX" && pwd)
case "$PREFIX" in
    /|/usr|/usr/local)
        echo "refusing --prefix $PREFIX: it would shadow the system FreeRDP" >&2
        exit 1 ;;
esac
# the prefix is emptied before installing: only one this script made
if [ -n "$(ls -A "$PREFIX")" ] && [ ! -f "$PREFIX/SOURCE_COMMIT" ]; then
    echo "refusing --prefix $PREFIX: not empty and not made by $0" >&2
    exit 1
fi
WORK=$PREFIX.build

# --- source: exactly $COMMIT, tracked files only
rm -rf "$WORK/src"
mkdir -p "$WORK/src"
if [ -n "$FREERDP_SRC" ]; then
    echo "== source: $FREERDP_SRC @ $COMMIT"
    git -C "$FREERDP_SRC" archive --format=tar "$COMMIT" | tar -x -C "$WORK/src"
else
    echo "== source: github.com/FreeRDP/FreeRDP @ $COMMIT"
    curl -fsSL "https://github.com/FreeRDP/FreeRDP/archive/$COMMIT.tar.gz" | \
        tar -xz --strip-components=1 -C "$WORK/src"
fi

# --- options: features on when pkg-config finds them, off otherwise
OPTS=()
feature() {
    local opt=$1
    shift
    if pkg-config --exists "$@"; then
        OPTS+=("-D$opt=ON")
    else
        OPTS+=("-D$opt=OFF")
    fi
}
feature WITH_KRB5 krb5-gssapi
feature WITH_PULSE libpulse
feature WITH_ALSA alsa
feature WITH_CUPS libcups
feature WITH_PCSC libpcsclite
feature WITH_FUSE fuse3
feature WITH_OPUS opus
feature WITH_FFMPEG libavcodec libavutil
feature WITH_SWSCALE libswscale
feature CHANNEL_URBDRC libusb-1.0
feature WITH_OPENH264 openh264
if [ "$CLIENT" = yes ]; then
    feature WITH_X11 x11 xkbfile
    OPTS+=(-DWITH_CLIENT=ON -DWITH_CLIENT_SDL=OFF)
else
    OPTS+=(-DWITH_CLIENT=OFF -DWITH_X11=OFF)
fi
if [ "$AV1" = yes ]; then
    # WITH_DAV1D must be asked for: WITH_GFX_AV1 alone builds no decoder
    OPTS+=(-DWITH_GFX_AV1=ON -DWITH_DAV1D=ON)
fi
GENERATOR="Unix Makefiles"
if command -v ninja > /dev/null; then
    GENERATOR=Ninja
fi
if command -v ccache > /dev/null; then
    OPTS+=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
fi

echo "== configure ($GENERATOR, static) -> $PREFIX"
cmake -G "$GENERATOR" -S "$WORK/src" -B "$WORK/build" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_INSTALL_LIBDIR=lib -DBUILD_SHARED_LIBS=OFF \
    -DWITH_SERVER=ON -DWITH_SAMPLE=OFF -DWITH_SHADOW=OFF -DWITH_PROXY=OFF \
    -DWITH_MANPAGES=OFF -DBUILD_TESTING=OFF "${OPTS[@]}" > "$WORK/configure.log" 2>&1 || {
    grep -A5 "CMake Error" "$WORK/configure.log" >&2
    echo "configure failed, see $WORK/configure.log" >&2
    exit 1
}
if [ "$AV1" = yes ] && ! grep -q "AV1 decoder: dav1d" "$WORK/configure.log"; then
    echo "--av1: no dav1d decoder configured (libdav1d-dev?), see $WORK/configure.log" >&2
    exit 1
fi

echo "== build"
cmake --build "$WORK/build" > "$WORK/build.log" 2>&1 || {
    grep -m10 -E "error|FAILED" "$WORK/build.log" >&2
    echo "build failed, see $WORK/build.log" >&2
    exit 1
}
# a fresh prefix, so nothing stale from an older build stays in it
# (checked above: empty or made by this script)
find "$PREFIX" -mindepth 1 -delete
cmake --install "$WORK/build" > "$WORK/install.log" 2>&1
echo "$COMMIT" > "$PREFIX/SOURCE_COMMIT"

# --- checks: nothing in the prefix for the loader to find or interpose
echo "== check"
fail=0
shared=$(find "$PREFIX" -name "*.so" -o -name "*.so.*")
if [ -n "$shared" ]; then
    echo "FAIL  shared objects installed:"
    echo "$shared"
    fail=1
fi
for bin in "$PREFIX"/bin/*; do
    [ -x "$bin" ] || continue
    if ldd "$bin" | grep -q -E "lib(freerdp|winpr)"; then
        echo "FAIL  $(basename "$bin") loads a shared FreeRDP:"
        ldd "$bin" | grep -E "lib(freerdp|winpr)"
        fail=1
    fi
    if nm -D --defined-only "$bin" | grep -q -E " (freerdp|winpr)_"; then
        echo "FAIL  $(basename "$bin") exports FreeRDP symbols"
        fail=1
    fi
done

# a program linked through pkg-config, the way xrdp would link it
cat > "$WORK/probe.c" << 'EOF'
#include <stdio.h>
#include <freerdp/freerdp.h>
#include <freerdp/listener.h>

int main(void)
{
    freerdp_listener* listener = freerdp_listener_new();
    printf("FreeRDP %s (%s), listener %s\n", freerdp_get_version_string(),
           freerdp_get_build_revision(), listener ? "ok" : "failed");
    freerdp_listener_free(listener);
    return listener ? 0 : 1;
}
EOF
export PKG_CONFIG_PATH=$PREFIX/lib/pkgconfig
# shellcheck disable=SC2046 # pkg-config prints several words on purpose
if gcc -o "$WORK/probe" "$WORK/probe.c" -w \
        $(pkg-config --static --cflags --libs freerdp-server3) 2> "$WORK/probe.log" &&
    probe_out=$("$WORK/probe"); then
    echo "ok    pkg-config --static link: $probe_out"
else
    echo "FAIL  linking a program through pkg-config, see $WORK/probe.log"
    fail=1
fi
if [ -x "$PREFIX/bin/xfreerdp" ]; then
    "$PREFIX/bin/xfreerdp" /buildconfig > "$PREFIX/buildconfig"
    options=$(tr ' ' '\n' < "$PREFIX/buildconfig" | grep -E '^WITH_(GFX_AV1|GFX_H264|X11|PULSE|FUSE)=')
    echo "ok    xfreerdp: ${options//$'\n'/ }"
    if [ "$AV1" = yes ] && ! grep -qw WITH_GFX_AV1=ON <<< "$options"; then
        echo "FAIL  --av1: xfreerdp is built without WITH_GFX_AV1"
        fail=1
    fi
fi
[ "$fail" = 0 ] || exit 1
echo "== done: $PREFIX (FreeRDP $COMMIT)"
