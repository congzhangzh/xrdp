#!/bin/bash
# Builds the test images and runs every wlup end-to-end suite, in
# parallel, each in its own container:
#
#   sway-no-systemd  sway in a plain container (no init system, no
#                    logind, no PipeWire), with /dev/fuse for the drive test
#   sway-systemd     sway with systemd as PID 1: real PAM -> logind
#                    session, microphone
#   gnome-systemd    headless GNOME through the Mutter backend, with
#                    systemd as PID 1 (GNOME needs it)
#   kde-systemd      KDE Plasma through the KWin backend, systemd as PID 1.
#                    KWin casts only with OpenGL, which takes a DRM render
#                    node: skipped when the host has none (without a GPU:
#                    sudo modprobe vgem)
#
# Usage: wlup/test/run-all.sh [-n] [output dir]
#   -n  do not rebuild the images
#
# Every suite connects with the FreeRDP of wlup/test/freerdp-av1: by
# default the upstream commit it pins, or with
#   FREERDP_SRC=<FreeRDP git checkout> [FREERDP_REF=<ref>, default HEAD]
# that commit of the checkout (taken with git archive, so it need not be
# pushed). Debian's client is kept for the check of a client without AV1.
#
# Exit status: 0 if every suite ran and passed, 1 otherwise. Logs and
# screenshots of each suite are kept in <output dir>/<suite>/.
set -u

TEST_DIR=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$TEST_DIR/../.." && pwd)

BUILD=1
if [ "${1:-}" = "-n" ]; then
    BUILD=0
    shift
fi
OUT=${1:-$(mktemp -d /tmp/wlup-test.XXXXXX)}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
TAG=wlup-test-$$
CLIENT_ENV=(-e XFREERDP=/opt/freerdp-av1/bin/xfreerdp)

# The FreeRDP tree for the client image, if not the pinned one
FREERDP_ARGS=()
if [ -n "${FREERDP_SRC:-}" ]; then
    if [ $BUILD = 0 ]; then
        echo "FREERDP_SRC needs the images rebuilt: drop -n"
        exit 1
    fi
    FREERDP_SHA=$(git -C "$FREERDP_SRC" rev-parse --verify "${FREERDP_REF:-HEAD}^{commit}") ||
        exit 1
    rm -rf "$OUT/freerdp-src"
    mkdir -p "$OUT/freerdp-src"
    git -C "$FREERDP_SRC" archive "$FREERDP_SHA" | tar -x -C "$OUT/freerdp-src" || exit 1
    FREERDP_ARGS=(--build-context "freerdp_src=$OUT/freerdp-src"
                  --build-arg "FREERDP_COMMIT=$FREERDP_SHA")
fi

# --- images (each one is built on the previous) -------------------------------
if [ $BUILD = 1 ]; then
    for img in "xrdp-wlup-dev $TEST_DIR" \
               "xrdp-wlup-freerdp-av1 $TEST_DIR/freerdp-av1" \
               "xrdp-wlup-systemd $TEST_DIR/systemd" \
               "xrdp-wlup-kde $TEST_DIR/kde" \
               "xrdp-wlup-gnome $TEST_DIR/gnome" \
               "xrdp-wlup-mutter $TEST_DIR/mutter"; do
        set -- $img
        args=()
        [ "$1" = xrdp-wlup-freerdp-av1 ] && args=("${FREERDP_ARGS[@]}")
        echo "building $1"
        if ! docker build -q "${args[@]}" -t "$1" "$2" > "$OUT/build-$1.log" 2>&1; then
            echo "FAILED to build $1, see $OUT/build-$1.log"
            exit 1
        fi
    done
    rm -rf "$OUT/freerdp-src"
fi
echo "client: FreeRDP $(docker run --rm xrdp-wlup-freerdp-av1 cat /opt/freerdp-av1/SOURCE_COMMIT)" \
    "${FREERDP_SRC:+(${FREERDP_REF:-HEAD} of $FREERDP_SRC)}" | tee "$OUT/client.txt"

cleanup() {
    docker rm -f "$TAG-sway-systemd" "$TAG-gnome-systemd" "$TAG-kde-systemd" \
        > /dev/null 2>&1
}
trap cleanup EXIT

# Runs a suite in a container whose PID 1 is systemd
# run_systemd <suite> <image> <script inside /src>
run_systemd() {
    docker run -d --name "$TAG-$1" --privileged --cgroupns=private \
        --tmpfs /run --tmpfs /run/lock \
        -v "$SRC":/src:ro -v "$OUT/$1":/out "$2" /usr/lib/systemd/systemd \
        > /dev/null &&
    sleep 5 &&
    docker exec "${CLIENT_ENV[@]}" "$TAG-$1" bash -c \
        "bash $3 /src /out > /out/run.log 2>&1; chmod -R a+r /out"
    docker rm -f "$TAG-$1" > /dev/null 2>&1
}

SUITES="sway-no-systemd sway-systemd gnome-systemd kde-systemd"
for suite in $SUITES; do
    mkdir -p "$OUT/$suite"
done
echo "running the suites in parallel, output in $OUT"

docker run --rm --device /dev/fuse --cap-add SYS_ADMIN \
    --security-opt apparmor:unconfined "${CLIENT_ENV[@]}" \
    -v "$SRC":/src:ro -v "$OUT/sway-no-systemd":/out xrdp-wlup-freerdp-av1 \
    bash -c 'bash /src/wlup/test/run-e2e.sh /src /out > /out/run.log 2>&1;
             chmod -R a+r /out' &
run_systemd sway-systemd xrdp-wlup-systemd /src/wlup/test/run-e2e.sh &
run_systemd gnome-systemd xrdp-wlup-mutter /src/wlup/test/mutter/run-e2e-mutter.sh &
if ls /dev/dri/renderD* > /dev/null 2>&1; then
    run_systemd kde-systemd xrdp-wlup-kde /src/wlup/test/kde/run-e2e-kde.sh &
else
    echo "skipped: no DRM render node on the host, which KWin needs to" \
        "cast its output (without a GPU: sudo modprobe vgem)" \
        > "$OUT/kde-systemd/skipped.txt"
fi
wait

# --- summary --------------------------------------------------------------
status=0
for suite in $SUITES; do
    log="$OUT/$suite/run.log"
    if [ -f "$OUT/$suite/skipped.txt" ]; then
        printf '%-16s %s\n' "$suite" "$(cat "$OUT/$suite/skipped.txt")"
        continue
    fi
    result=$(grep -E '^passed [0-9]+, failed [0-9]+' "$log" 2>/dev/null | tail -1)
    printf '%-16s %s\n' "$suite" "${result:-did not finish, see $log}"
    grep -E '^FAIL' "$log" 2>/dev/null | sed 's/^/                 /'
    case "$result" in
        *"failed 0") ;;
        *) status=1 ;;
    esac
done
exit $status
