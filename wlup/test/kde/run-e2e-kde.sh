#!/bin/bash
# End-to-end test of a KDE Plasma session through sesman and the wlup KWin
# backend:
#
#   xfreerdp3 (in Xvfb) --RDP--> xrdp + libwlup (backend=kwin)
#       --Wayland/PipeWire--> KWin + Plasma started by sesman
#
# xrdp and sesman run as root. The client logs in as 'tester' with a
# password; sesman runs startplasma.sh for the user (sesman.ini [KDE]),
# which starts KWin on its virtual backend, and KWin starts Plasma.
# sesman passes xrdp connections to KWin's Wayland socket and to the
# user's PipeWire daemon.
#
# Plasma needs systemd and logind, and KWin casts its output only with
# an OpenGL scene. Debian's KWin (6.7) takes a DRM render node for it: on
# a machine without a GPU, load vgem on the host (modprobe vgem). KWin
# 6.8 and later render in software with /dev/udmabuf. From the host:
#
#   docker run -d --name kde-e2e --privileged --cgroupns=private \
#       --tmpfs /run --tmpfs /run/lock -v <xrdp src>:/src:ro -v <out>:/out \
#       xrdp-wlup-kde /usr/lib/systemd/systemd
#   docker exec kde-e2e bash /src/wlup/test/kde/run-e2e-kde.sh /src /out
#   docker rm -f kde-e2e
#
# Usage: run-e2e-kde.sh <xrdp source dir> <output dir>
set -eu

SRC=$1
OUT=$2
W=1280
H=800
PASSWORD=wlup-test-pw
USER_RUN=/run/user/1000
mkdir -p "$OUT"

# Runs a command as the user inside their Plasma session
in_session() {
    local wl
    wl=$(cd $USER_RUN && ls wayland-? 2>/dev/null | head -1)
    su tester -c "export XDG_RUNTIME_DIR=$USER_RUN \
        DBUS_SESSION_BUS_ADDRESS=unix:path=$USER_RUN/bus WAYLAND_DISPLAY=$wl; $*"
}

# Waits for the nth "connected to KWin" in xrdp's log
wait_connected() {
    for _ in $(seq "$2"); do
        [ "$(grep -c "wlup: connected to KWin" /var/log/xrdp.log 2>/dev/null)" -ge "$1" ] &&
            return 0
        sleep 1
    done
    return 0
}

# --- build and install xrdp --------------------------------------------------
cp -a "$SRC" /build
cd /build
./bootstrap > "$OUT/bootstrap.log" 2>&1
./configure --enable-wlup --enable-wlup-kwin --enable-fuse --enable-x264 --prefix=/usr --sysconfdir=/etc \
    --localstatedir=/var CFLAGS="-O2 -g -Wno-error=nonnull" > "$OUT/configure.log" 2>&1
make -j"$(nproc)" > "$OUT/make.log" 2>&1
make install > "$OUT/install.log" 2>&1
ldconfig

# --- the test user -------------------------------------------------------------
echo "tester:$PASSWORD" | chpasswd
# The session has no seat, so logind gives tester no access to the DRM
# devices KWin renders with (vgem's card and render node): their groups.
# The nodes keep the host's group ids, which may have no name here
for node in /dev/dri/card* /dev/dri/renderD*; do
    gid=$(stat -c %g "$node")
    group=$(getent group "$gid" | cut -d: -f1)
    if [ -z "$group" ]; then
        group=dri$gid
        groupadd -g "$gid" "$group"
    fi
    usermod -aG "$group" tester
done

# --- xrdp and sesman, as root --------------------------------------------------
openssl req -x509 -newkey rsa:2048 -nodes -days 1 -subj /CN=wlup-test \
    -keyout /etc/xrdp/key.pem -out /etc/xrdp/cert.pem > /dev/null 2>&1
cat >> /etc/xrdp/xrdp.ini <<EOF

[KDE]
name=KDE Plasma (wlup prototype)
lib=libwlup.so
username=ask
password=ask
port=-1
code=32
backend=kwin
EOF
sed -i 's/^#\?autorun=.*/autorun=KDE/' /etc/xrdp/xrdp.ini
sed -i 's/^LogLevel=.*/LogLevel=DEBUG/' /etc/xrdp/xrdp.ini /etc/xrdp/sesman.ini
xrdp-sesman --nodaemon > "$OUT/sesman.log" 2>&1 &
xrdp --nodaemon > "$OUT/xrdp-stdout.log" 2>&1 &
sleep 2

# --- RDP client in Xvfb -------------------------------------------------------
Xvfb :99 -screen 0 1920x1080x24 > /dev/null 2>&1 &
export DISPLAY=:99
sleep 1
. "$SRC/wlup/test/drive-mic-tests.sh"
drive_mic_setup
connect() {
    ${XFREERDP:-xfreerdp3} /v:127.0.0.1 /u:tester /p:$PASSWORD /cert:ignore /size:${W}x${H} \
        /bpp:${BPP:-24} /dynamic-resolution -grab-keyboard /sound:sys:fake ${2:-} \
        /log-filters:com.freerdp.channels.rdpsnd.client:DEBUG,com.freerdp.channels.audin.client:DEBUG,com.freerdp.channels.drdynvc.client:DEBUG \
        > "$OUT/client$1.log" 2>&1 &
    CLIENT=$!
}
connect "" "$DRIVE_MIC_OPTS"
# sesman starts KWin, then wlup casts its output; Plasma comes up after
wait_connected 1 90
sleep 15
import -window root "$OUT/client-1-connected.png"

# --- who runs what -----------------------------------------------------------
KWIN_PID=$(pgrep -u tester -x kwin_wayland || true)
SESSION_ID=$(loginctl list-sessions --no-legend |
    awk '$3 == "tester" && $0 !~ /manager/ { print $1; exit }')
loginctl show-session "${SESSION_ID:-none}" > "$OUT/loginctl-session.txt" 2>&1 || true
ps -eo user,pid,ppid,cmd > "$OUT/ps.txt"
in_session "wayland-info" > "$OUT/wayland-info.txt" 2>&1 || true

# --- keyboard: type into a terminal -------------------------------------------
in_session "nohup foot --maximized > /tmp/foot.log 2>&1 &"
sleep 4
xdotool mousemove $((W / 2)) $((H / 2)) click 1
sleep 1
xdotool type --delay 80 'echo kwin-typed-$((6*7)) > /tmp/typed.txt'
xdotool key Return
sleep 2
import -window root "$OUT/client-2-typed.png"

# --- clipboard, both ways, while foot has the focus -------------------------
. "$SRC/wlup/test/clipboard-tests.sh"
clipboard_tests
drive_test
in_session "pkill -x foot" || true
sleep 1

# --- pointer: buttons and wheel over wev ------------------------------------
in_session "nohup stdbuf -oL wev > /tmp/wev.log 2>&1 &"
sleep 4
xdotool mousemove $((W / 2)) $((H / 2))
sleep 0.5
su tester -c ": > /tmp/wev.log"
xdotool click 1; sleep 0.3
xdotool click 3; sleep 0.3
xdotool click 2; sleep 0.3
xdotool click 4; sleep 0.3
xdotool click 5; sleep 0.3
sleep 1
import -window root "$OUT/client-3-pointer.png"
cp /tmp/wev.log "$OUT/wev.log"
in_session "pkill -x wev" || true

# --- audio: play into the xrdp sink of the session's PipeWire --------------
for _ in $(seq 20); do
    in_session "pw-cli ls Node" 2>/dev/null | grep -q 'node.name = "xrdp-sink"' && break
    sleep 1
done
in_session "pw-cli ls Node" > "$OUT/pw-nodes.txt" 2>&1 || true
in_session "timeout 3 pw-cat --playback --raw --target xrdp-sink --format s16 \
    --rate 44100 --channels 2 - < /dev/urandom" > "$OUT/pw-cat.log" 2>&1 || true
sleep 2
cp "$OUT/client.log" "$OUT/client-audio.log"
mic_test

# --- resize: make the client window bigger ------------------------------------
# The client resizes its window only once its display control channel is
# open, which a loaded host (all suites at once, KWin rendering on the
# CPU) can delay: ask again until xrdp has resized the output, for up to
# 90 s. The client's log has the time each dynamic channel was created.
WID=$(xdotool search --pid $CLIENT | tail -1)
waited=0
while ! grep -q "virtual monitor resized to 1600x900" /var/log/xrdp.log &&
    [ $waited -lt 90 ]; do
    [ $((waited % 8)) -eq 0 ] && xdotool windowsize "$WID" 1600 900
    sleep 1
    waited=$((waited + 1))
done
echo "resize: done after ${waited}s"
sleep 2
import -window root "$OUT/client-4-resized.png"
in_session "nohup foot --maximized > /tmp/foot.log 2>&1 &"
sleep 4
xdotool mousemove 800 450 click 1
sleep 1
xdotool type --delay 80 'echo resized-$((6*7)) > /tmp/typed-resized.txt'
xdotool key Return
sleep 2
import -window root "$OUT/client-5-typed-after-resize.png"
in_session "pkill -x foot" || true
sleep 1

# --- reconnect: a new connection must reach the same KWin -------------------
kill $CLIENT 2>/dev/null || true
sleep 3
connect "-reconnect"
wait_connected 2 30
sleep 4
KWIN_PID_AFTER=$(pgrep -u tester -x kwin_wayland || true)
import -window root "$OUT/client-6-reconnected.png"
in_session "nohup foot --maximized > /tmp/foot.log 2>&1 &"
sleep 4
xdotool mousemove $((W / 2)) $((H / 2)) click 1
sleep 1
xdotool type --delay 80 'echo again-$((6*7)) > /tmp/typed2.txt'
xdotool key Return
sleep 2
import -window root "$OUT/client-7-typed-after-reconnect.png"

# --- GFX H.264 ---------------------------------------------------------------
kill $CLIENT 2>/dev/null || true
sleep 3
BPP=32 connect "-h264" "/gfx:AVC420"
wait_connected 3 30
sleep 4
KWIN_PID_H264=$(pgrep -u tester -x kwin_wayland || true)
import -window root "$OUT/client-8-h264.png"
xdotool mousemove $((W / 2)) $((H / 2)) click 1
sleep 1
xdotool type --delay 80 'echo h264-$((6*7)) > /tmp/typed4.txt'
xdotool key Return
sleep 2
import -window root "$OUT/client-9-h264-typed.png"

in_session "journalctl --user --no-pager -o cat" > "$OUT/user-journal.log" 2>&1 || true

# --- API contract: what KWin offers against what the backend binds --------
API_OK=0
in_session "python3 -I $SRC/wlup/test/api/api-contract.py kwin \
    $SRC/wlup/test/api/kwin.contract /tmp/api" > "$OUT/api-contract.txt" 2>&1 &&
    API_OK=1
cp /tmp/api/* "$OUT/" 2>/dev/null || true

echo "=== result ==="
pass=0
fail=0
check() {
    if eval "$2"; then
        echo "PASS  $1"; pass=$((pass + 1))
    else
        echo "FAIL  $1"; fail=$((fail + 1))
    fi
}
check "sesman: kwin_wayland runs as the logged-in user" \
    '[ -n "$KWIN_PID" ]'
check "sesman: plasmashell runs as the logged-in user" \
    'pgrep -u tester -x plasmashell > /dev/null'
check "xrdp runs as root, not as the user" \
    '[ "$(ps -o user= -C xrdp | sort -u)" = root ]'
check "logind: remote wayland session of tester" \
    'grep -q "^Name=tester$" "$OUT/loginctl-session.txt" &&
     grep -q "^Remote=yes$" "$OUT/loginctl-session.txt"'
check "kwin: PipeWire stream at the client size" \
    'grep -q "wlup/kwin: stream format BGR[xA] ${W}x${H}" /var/log/xrdp.log'
check "kwin: fake input bound" \
    'grep -q "wlup: connected to KWin, PipeWire node [0-9]*, fake input" /var/log/xrdp.log'
check "api: KWin offers what the backend binds (api/kwin.contract)" \
    '[ $API_OK = 1 ]'
check "kwin: API versions and capabilities logged" \
    'grep -q "wlup/kwin: capabilities:" /var/log/xrdp.log'
check "cursor: KWin's pointer shape sent as an RDP pointer" \
    'grep -q "wlup/kwin: pointer [0-9]*x[0-9]* sent as" /var/log/xrdp.log'
check "picture: client shows more than a flat screen" \
    '[ "$(convert "$OUT/client-1-connected.png" -format %k info:)" -gt 50 ]'
check "keyboard: typed command ran in the Plasma session" \
    '[ "$(cat /tmp/typed.txt 2>/dev/null)" = kwin-typed-42 ]'
clipboard_checks
drive_mic_checks
check "left button press"   'grep -a -q "button: 272.*state: 1" /tmp/wev.log'
check "left button release" 'grep -a -q "button: 272.*state: 0" /tmp/wev.log'
check "right button"        'grep -a -q "button: 273" /tmp/wev.log'
check "middle button"       'grep -a -q "button: 274" /tmp/wev.log'
check "wheel: vertical axis events" 'grep -a -q "axis: 0" /tmp/wev.log'
check "wheel: both directions" \
    'grep -a "axis: 0" /tmp/wev.log | grep -q -- "value: -" &&
     grep -a "axis: 0" /tmp/wev.log | grep -v -q -- "value: -"'
check "audio: chansrv runs in the session" \
    'pgrep -u tester -x xrdp-chansrv > /dev/null'
check "audio: the session's PipeWire has the xrdp sink" \
    'grep -q "node.name = \"xrdp-sink\"" "$OUT/pw-nodes.txt"'
check "audio: the client received sound (10+ Wave PDUs)" \
    '[ "$(grep -c "WaveInfo:" "$OUT/client-audio.log")" -ge 10 ]'
check "resize: output follows client window 1600x900" \
    'grep -q "virtual monitor resized to 1600x900" /var/log/xrdp.log &&
     grep -q "stream format BGR[xA] 1600x900" /var/log/xrdp.log'
check "resize: input still works after resize" \
    '[ "$(cat /tmp/typed-resized.txt 2>/dev/null)" = resized-42 ]'
check "reconnect: same kwin_wayland process" \
    '[ -n "$KWIN_PID" ] && [ "$KWIN_PID" = "$KWIN_PID_AFTER" ]'
check "reconnect: input works in the reconnected session" \
    '[ "$(cat /tmp/typed2.txt 2>/dev/null)" = again-42 ]'
check "h264: the 32 bpp client reaches the same session" \
    '[ -n "$KWIN_PID" ] && [ "$KWIN_PID" = "$KWIN_PID_H264" ]'
check "h264: frames go to the GFX H.264 encoder" \
    'grep -q "wlup: frames go to the GFX H.264 encoder" /var/log/xrdp.log &&
     grep -q "wlup: first H.264 frames sent" /var/log/xrdp.log'
check "h264: every frame acknowledged in time (no ack timeout)" \
    '! grep -q "wlup: no ack for frame" /var/log/xrdp.log'
check "h264: client shows more than a flat screen" \
    '[ "$(convert "$OUT/client-8-h264.png" -format %k info:)" -gt 50 ]'
check "h264: input works" \
    '[ "$(cat /tmp/typed4.txt 2>/dev/null)" = h264-42 ]'
echo "passed $pass, failed $fail"

kill $CLIENT 2>/dev/null || true
cp /var/log/xrdp.log /var/log/xrdp-sesman.log "$OUT/" 2>/dev/null || true
chmod -R a+r "$OUT"
grep -h -E 'wlup' /var/log/xrdp.log | head -30 || true
grep -h -i -E 'kde|plasma|kwin|connect.*as user' /var/log/xrdp-sesman.log | head -20 || true
