#!/bin/bash
# Builds the test images and runs run-backpressure.sh for the given
# sessions (default: all), one after the other, as they measure time:
#
#   sway   wlup, wlroots backend   (xrdp-wlup-dev)
#   xorg   xorgxrdp, for reference (xrdp-wlup-xorgxrdp)
#   gnome  wlup, Mutter backend    (xrdp-wlup-mutter, systemd as PID 1)
#
# The slow link needs the host's sch_netem module. The settings below are
# for CI: limits that catch a regression from what the code does today,
# not targets; see BP_P90_LIMITS in run-backpressure.sh.
#
# Usage: wlup/test/run-backpressure-all.sh [-n] <output dir> [session...]
#   -n  do not rebuild the images
#
# Exit status: 0 if every session ran and passed, 1 otherwise.
set -u

TEST_DIR=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$TEST_DIR/../.." && pwd)

BUILD=1
if [ "${1:-}" = "-n" ]; then
    BUILD=0
    shift
fi
OUT=${1:?output dir}
shift
SESSIONS=${*:-"sway xorg gnome"}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)

# Today: gfx-rfx sends lossless full-screen planar frames, about 2.5-4 s
# over 4 Mbit/s; bitmap is 1-2.5 s with tcp_notsent_lowat
export BP_P90_LIMITS=${BP_P90_LIMITS:-"h264=2000 gfx-rfx=5000 bitmap=4000"}
export BP_NOTSENT_LOWAT=${BP_NOTSENT_LOWAT:-131072}
export BP_REQUIRE_SHAPE=${BP_REQUIRE_SHAPE:-1}

build() { # build <image> <dir>
    [ $BUILD = 1 ] || return 0
    echo "building $1"
    docker build -q -t "$1" "$2" > "$OUT/build-$1.log" 2>&1 ||
        { echo "FAILED to build $1, see $OUT/build-$1.log"; exit 1; }
}
# An array, as the limits have spaces
ENV_ARGS=(-e "BP_P90_LIMITS=$BP_P90_LIMITS" -e "BP_NOTSENT_LOWAT=$BP_NOTSENT_LOWAT"
          -e "BP_REQUIRE_SHAPE=$BP_REQUIRE_SHAPE")

build xrdp-wlup-dev "$TEST_DIR"
status=0
for session in $SESSIONS; do
    out="$OUT/$session"
    mkdir -p "$out"
    case $session in
        sway)
            docker run --rm --cap-add NET_ADMIN "${ENV_ARGS[@]}" \
                -v "$SRC":/src:ro -v "$out":/out xrdp-wlup-dev \
                bash -c 'bash /src/wlup/test/run-backpressure.sh /src /out \
                    > /out/run.log 2>&1; rc=$?; chmod -R a+r /out; exit $rc'
            ;;
        xorg)
            build xrdp-wlup-xorgxrdp "$TEST_DIR/xorgxrdp"
            docker run --rm --cap-add NET_ADMIN -e BP_SESSION=xorg "${ENV_ARGS[@]}" \
                -v "$SRC":/src:ro -v "$out":/out xrdp-wlup-xorgxrdp \
                bash -c 'bash /src/wlup/test/run-backpressure.sh /src /out \
                    > /out/run.log 2>&1; rc=$?; chmod -R a+r /out; exit $rc'
            ;;
        gnome)
            build xrdp-wlup-systemd "$TEST_DIR/systemd"
            build xrdp-wlup-gnome "$TEST_DIR/gnome"
            build xrdp-wlup-mutter "$TEST_DIR/mutter"
            name=wlup-bp-$$
            docker run -d --name $name --privileged --cgroupns=private \
                --tmpfs /run --tmpfs /run/lock \
                -v "$SRC":/src:ro -v "$out":/out xrdp-wlup-mutter \
                /usr/lib/systemd/systemd > /dev/null
            sleep 5
            docker exec -e BP_SESSION=gnome "${ENV_ARGS[@]}" $name \
                bash -c 'bash /src/wlup/test/run-backpressure.sh /src /out \
                    > /out/run.log 2>&1; rc=$?; chmod -R a+r /out; exit $rc'
            docker rm -f $name > /dev/null
            ;;
        *)
            echo "unknown session $session"
            exit 1
            ;;
    esac
    echo "=== $session"
    if [ -f "$out/summary.txt" ]; then
        cat "$out/summary.txt"
        grep -q "^FAIL" "$out/summary.txt" && status=1
    else
        echo "did not finish, see $out/run.log"
        tail -5 "$out/run.log"
        status=1
    fi
done
exit $status
