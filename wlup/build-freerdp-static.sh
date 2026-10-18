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
#   --tree DIR    build this FreeRDP checkout as it is now: its tracked files,
#                 uncommitted changes included (default prefix
#                 ~/.cache/xrdp-freerdp-static/tree-<dir name>); for an
#                 edit-build-test loop without committing
#   --av1         build the client with the AV1 decoder (dav1d, required)
#   --no-client   build the libraries only
#   --debug       CMAKE_BUILD_TYPE=Debug instead of RelWithDebInfo
#   --asan        AddressSanitizer build; whatever links it needs
#                 -fsanitize=address too
#
# --debug and --asan add -debug / -asan to the default prefix, so each
# variant has its own prefix and build directory.
#
# Then:
#
#   $PREFIX/bin/xfreerdp /v:...      the client, needs no libfreerdp3.so
#   PKG_CONFIG_PATH=$PREFIX/lib/pkgconfig \
#       pkg-config --static --cflags --libs freerdp-server3
#
# Optional features follow what pkg-config finds on the host. The build
# directory is kept next to the prefix for incremental rebuilds: source files
# whose content did not change keep their timestamps, so a rebuild compiles
# only what changed. Uses Ninja, ccache and mold when they are installed.
#
# Link-time optimization is off. FreeRDP enables it by default, and GCC's LTO
# objects then hold only bytecode for the exact GCC that wrote them: clang,
# or another distribution's GCC, can not link such an archive ("plugin needed
# to handle lto object"). The check below fails on any LTO object.
set -euo pipefail

SRC=$(cd "$(dirname "$0")/.." && pwd)
COMMIT=$(sed -n 's/^ARG FREERDP_COMMIT=//p' "$SRC/wlup/test/freerdp-av1/Dockerfile")
PREFIX=
FREERDP_SRC=
TREE=
AV1=no
CLIENT=yes
BUILD_TYPE=RelWithDebInfo
ASAN=no
VARIANT=

while [ $# -gt 0 ]; do
    case "$1" in
        --prefix) PREFIX=$2; shift 2 ;;
        --commit) COMMIT=$2; shift 2 ;;
        --src) FREERDP_SRC=$2; shift 2 ;;
        --tree) TREE=$2; shift 2 ;;
        --av1) AV1=yes; shift ;;
        --no-client) CLIENT=no; shift ;;
        --debug) BUILD_TYPE=Debug; VARIANT=$VARIANT-debug; shift ;;
        --asan) ASAN=yes; VARIANT=$VARIANT-asan; shift ;;
        -h|--help) sed -n '2,/^set -euo/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $1 (see --help)" >&2; exit 1 ;;
    esac
done

if [ -n "$TREE" ] && [ -n "$FREERDP_SRC" ]; then
    echo "--tree and --src exclude each other" >&2
    exit 1
fi
if [ -n "$TREE" ]; then
    TREE=$(cd "$TREE" && pwd)
    COMMIT=$(git -C "$TREE" rev-parse HEAD)
    LABEL=$COMMIT
    if [ -n "$(git -C "$TREE" status --porcelain --untracked-files=no)" ]; then
        LABEL="$COMMIT + uncommitted changes"
    fi
    PREFIX=${PREFIX:-$HOME/.cache/xrdp-freerdp-static/tree-$(basename "$TREE")$VARIANT}
else
    if [ -n "$FREERDP_SRC" ]; then
        COMMIT=$(git -C "$FREERDP_SRC" rev-parse --verify "$COMMIT^{commit}")
    fi
    LABEL=$COMMIT
    PREFIX=${PREFIX:-$HOME/.cache/xrdp-freerdp-static/${COMMIT:0:12}$VARIANT}
fi
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

# --- source: exactly $COMMIT (or the --tree), tracked files only, unpacked
# into src.new and then synced into src
rm -rf "$WORK/src.new"
mkdir -p "$WORK/src.new"
if [ -n "$TREE" ]; then
    echo "== source: $TREE ($LABEL)"
    # tracked files that still exist: a file deleted but not yet committed is gone
    git -C "$TREE" ls-files -z | while IFS= read -r -d '' f; do
        if [ -e "$TREE/$f" ] || [ -L "$TREE/$f" ]; then
            printf '%s\0' "$f"
        fi
    done | tar -C "$TREE" --null -T - -cf - | tar -x -C "$WORK/src.new"
elif [ -n "$FREERDP_SRC" ]; then
    echo "== source: $FREERDP_SRC @ $COMMIT"
    git -C "$FREERDP_SRC" archive --format=tar "$COMMIT" | tar -x -C "$WORK/src.new"
else
    echo "== source: github.com/FreeRDP/FreeRDP @ $COMMIT"
    curl -fsSL "https://github.com/FreeRDP/FreeRDP/archive/$COMMIT.tar.gz" | \
        tar -xz --strip-components=1 -C "$WORK/src.new"
fi
# Only files whose content changed are rewritten, and they get the current
# time; unchanged files keep theirs, so Ninja rebuilds only what changed. The
# archive's own timestamps (the commit time) must not be kept: going back to
# an older commit would leave changed files older than their objects, and
# Ninja would not rebuild them.
if command -v rsync > /dev/null; then
    mkdir -p "$WORK/src"
    rsync -rlp --checksum --delete "$WORK/src.new/" "$WORK/src/"
    rm -rf "$WORK/src.new"
else
    rm -rf "$WORK/src"
    mv "$WORK/src.new" "$WORK/src"
    find "$WORK/src" -exec touch -h {} +
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
# CMAKE_LINKER_TYPE needs CMake 3.29
cmake_version=$(cmake --version | sed -n '1s/^cmake version //p')
if command -v mold > /dev/null && printf '3.29\n%s\n' "$cmake_version" | sort -V -C; then
    OPTS+=(-DCMAKE_LINKER_TYPE=MOLD)
fi
if [ "$ASAN" = yes ]; then
    OPTS+=(-DWITH_SANITIZE_ADDRESS=ON)
fi

echo "== configure ($GENERATOR, static, $BUILD_TYPE${VARIANT:+, ${VARIANT#-}}) -> $PREFIX"
t0=$SECONDS
cmake -G "$GENERATOR" -S "$WORK/src" -B "$WORK/build" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_INSTALL_LIBDIR=lib -DBUILD_SHARED_LIBS=OFF \
    -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF \
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
echo "   configured and built in $((SECONDS - t0)) s"
# a fresh prefix, so nothing stale from an older build stays in it
# (checked above: empty or made by this script)
find "$PREFIX" -mindepth 1 -delete
cmake --install "$WORK/build" > "$WORK/install.log" 2>&1
echo "$LABEL" > "$PREFIX/SOURCE_COMMIT"

# --- checks: nothing in the prefix for the loader to find or interpose
echo "== check"
fail=0
shared=$(find "$PREFIX" -name "*.so" -o -name "*.so.*")
if [ -n "$shared" ]; then
    echo "FAIL  shared objects installed:"
    echo "$shared"
    fail=1
fi
# archives of machine code, not LTO bytecode only one GCC version can read
for lib in "$PREFIX"/lib/*.a; do
    if readelf -SW "$lib" 2> /dev/null | grep -q '\.gnu\.lto_'; then
        echo "FAIL  $(basename "$lib") holds LTO objects"
        fail=1
    fi
done
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
# with clang too when it is installed: the archives must not depend on the
# compiler that built them. ASan runtimes differ between GCC and clang, so an
# --asan build is probed with GCC only.
PROBE_CC=(gcc)
PROBE_FLAGS=()
if [ "$ASAN" = yes ]; then
    PROBE_FLAGS=(-fsanitize=address)
elif command -v clang > /dev/null; then
    PROBE_CC+=(clang)
fi
for cc in "${PROBE_CC[@]}"; do
    # shellcheck disable=SC2046 # pkg-config prints several words on purpose
    if "$cc" -o "$WORK/probe-$cc" "$WORK/probe.c" -w "${PROBE_FLAGS[@]}" \
            $(pkg-config --static --cflags --libs freerdp-server3) 2> "$WORK/probe-$cc.log" &&
        probe_out=$("$WORK/probe-$cc"); then
        echo "ok    pkg-config --static link ($cc): $probe_out"
    else
        echo "FAIL  linking a program through pkg-config with $cc, see $WORK/probe-$cc.log"
        fail=1
    fi
done
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
echo "== done: $PREFIX (FreeRDP $LABEL)"
