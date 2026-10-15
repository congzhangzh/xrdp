#!/bin/bash
# End-to-end test of the wlup prototype, run inside the xrdp-wlup-dev image:
#
#   xfreerdp3 (in Xvfb) --RDP--> xrdp + libwlup --Wayland--> headless sway
#
# 1. starts xrdp-sesman and xrdp with a [Wayland] session (code=30) that
#    loads libwlup
# 2. connects xfreerdp3 at 1280x800 as user 'tester' with a password:
#    sesman authenticates the user with PAM, starts sway headless at the
#    client's size through startwayland.sh, and passes xrdp a connection
#    to it. The user's sway config runs foot (terminal) and wev (prints
#    every input event it gets) side by side
# 3. checks sway's output has the client's size
# 4. types a command into foot and checks that it ran in the sway session
# 5. clicks and scrolls over wev and checks the events wev received
# 6. resizes the client window to 1600x900: sway's output must follow,
#    and input must still land where the client points
# Also the clipboard (clipboard-tests.sh), drive redirection and the
# microphone (drive-mic-tests.sh), and audio, logind and the microphone
# with systemd as PID 1 (wlup/test/systemd).
#
# Drive redirection needs /dev/fuse. Run the plain container with
#   --device /dev/fuse --cap-add SYS_ADMIN --security-opt apparmor:unconfined
# (the systemd one is privileged anyway); without it these checks are
# skipped.
#
# Usage: run-e2e.sh <xrdp source dir> <output dir>
set -eu

SRC=$1
OUT=$2

mkdir -p "$OUT"

# --- build and install xrdp --------------------------------------------------
cp -a "$SRC" /build
cd /build
./bootstrap > "$OUT/bootstrap.log" 2>&1
# -Wno-error=nonnull: GCC 16 flags existing code in xrdp/xrdp_mm.c
# AV1 (FreeRDP's GFX extension) when libaom is there
AV1_OPT=$(pkg-config --exists aom && echo --enable-av1)
./configure --enable-wlup --enable-fuse --enable-x264 $AV1_OPT --prefix=/usr --sysconfdir=/etc --localstatedir=/var \
    CFLAGS="-O2 -g -Wno-error=nonnull" > "$OUT/configure.log" 2>&1
make -j"$(nproc)" > "$OUT/make.log" 2>&1
make install > "$OUT/install.log" 2>&1
ldconfig

# --- the test user and their sway config -------------------------------------
echo 'tester:wlup-test-pw' | chpasswd
# Focus only changes on a click, so the click test cannot pass by pointer
# motion alone
mkdir -p /home/tester/.config/sway
cat > /home/tester/.config/sway/config <<'SWAYCONF'
output * bg #204a87 solid_color
focus_follows_mouse no
exec foot
exec sh -c 'sleep 1; exec stdbuf -oL wev > /tmp/wev.log 2>&1'
SWAYCONF
chown -R tester /home/tester/.config

# sway's IPC: swaymsg talks to sway over this socket, which is found once
# sesman has started the session
SWAYSOCK=
swaymsg_() {
    su tester -c "swaymsg -s $SWAYSOCK $*"
}
output_size() {
    swaymsg_ -t get_outputs | jq -r '.[0].current_mode | "\(.width)x\(.height)"'
}
centre() {
    swaymsg_ -t get_tree |
        jq -r --arg id "$1" '.. | objects | select(.app_id? == $id) |
            "\(.rect.x + .rect.width / 2 | floor) \(.rect.y + .rect.height / 2 | floor)"'
}

# --- xrdp ---------------------------------------------------------------------
openssl req -x509 -newkey rsa:2048 -nodes -days 1 -subj /CN=wlup-test \
    -keyout /etc/xrdp/key.pem -out /etc/xrdp/cert.pem > /dev/null 2>&1
cat >> /etc/xrdp/xrdp.ini <<EOF

[Wayland]
name=Wayland (wlup prototype)
lib=libwlup.so
username=ask
password=ask
port=-1
code=30
xkb_layout=us
EOF
# Skip the login screen and go straight to the Wayland session
sed -i 's/^#\?autorun=.*/autorun=Wayland/' /etc/xrdp/xrdp.ini
sed -i 's/^LogLevel=.*/LogLevel=DEBUG/' /etc/xrdp/xrdp.ini
sed -i 's/^LogLevel=.*/LogLevel=DEBUG/' /etc/xrdp/sesman.ini
xrdp-sesman --nodaemon > "$OUT/sesman.log" 2>&1 &
xrdp --nodaemon > "$OUT/xrdp.log" 2>&1 &
sleep 2

# --- RDP client in Xvfb -------------------------------------------------------
Xvfb :99 -screen 0 1920x1080x24 > /dev/null 2>&1 &
export DISPLAY=:99
sleep 1
# BPP=32 makes xrdp compress bitmaps with planar. FreeRDP's client does
# not resize its planar decoder on a deactivate-reactivate resize
# (connection.c only calls DesktopResize and cache_resize), so growing
# the desktop then disconnects it ("planar->maxWidth 1280 < nSrcWidth
# 1600"). 24 bpp uses interleaved RLE, which is not affected
BPP=${BPP:-24}
. "$SRC/wlup/test/drive-mic-tests.sh"
drive_mic_setup
# XFREERDP: client binary, e.g. a FreeRDP built from source
${XFREERDP:-xfreerdp3} /v:127.0.0.1 /u:tester /p:wlup-test-pw /cert:ignore /size:1280x800 \
    /bpp:$BPP /dynamic-resolution -grab-keyboard /sound:sys:fake $DRIVE_MIC_OPTS \
    /log-filters:com.freerdp.channels.rdpsnd.client:DEBUG > "$OUT/client.log" 2>&1 &
CLIENT=$!
sleep 8
SWAYSOCK=$(find /run /tmp -name 'sway-ipc.*.sock' 2>/dev/null | head -1)
echo "sway IPC socket: $SWAYSOCK"
SIZE_CONNECTED=$(output_size)
import -window root "$OUT/client-1-connected.png"

# --- keyboard: type a command into foot ---------------------------------------
read -r FOOT_X FOOT_Y < <(centre foot)
read -r WEV_X WEV_Y < <(centre wev)
echo "foot at $FOOT_X,$FOOT_Y  wev at $WEV_X,$WEV_Y"
xdotool mousemove "$FOOT_X" "$FOOT_Y" click 1
sleep 0.5
xdotool type --delay 80 'echo wlup-typed-$((6*7)) > /tmp/typed.txt'
xdotool key Return
sleep 2
import -window root "$OUT/client-2-typed.png"

# --- clipboard, both ways, while foot has the focus -------------------------
. "$SRC/wlup/test/clipboard-tests.sh"
clipboard_tests

# --- drive redirection, from the same terminal ------------------------------
drive_test

# --- audio, when the user has a PipeWire daemon (systemd as PID 1) ----------
if [ "$(cat /proc/1/comm)" = systemd ] && command -v pw-cat > /dev/null; then
    as_session() {
        su tester -c "export XDG_RUNTIME_DIR=/run/user/$(id -u tester); $*"
    }
    for _ in $(seq 20); do
        as_session "pw-cli ls Node" 2>/dev/null | grep -q 'node.name = "xrdp-sink"' && break
        sleep 1
    done
    as_session "pw-cli ls Node" > "$OUT/pw-nodes.txt" 2>&1 || true
    as_session "timeout 3 pw-cat --playback --raw --target xrdp-sink \
        --format s16 --rate 44100 --channels 2 - < /dev/urandom" \
        > "$OUT/pw-cat.log" 2>&1 || true
    sleep 2
    cp "$OUT/client.log" "$OUT/client-audio.log"
    mic_test
fi

# --- pointer: buttons and wheel over wev ------------------------------------
xdotool mousemove "$WEV_X" "$WEV_Y"
sleep 0.5
su tester -c ": > /tmp/wev.log"   # only keep events from here on
xdotool click 1; sleep 0.3   # left   -> BTN_LEFT   272
xdotool click 3; sleep 0.3   # right  -> BTN_RIGHT  273
xdotool click 2; sleep 0.3   # middle -> BTN_MIDDLE 274
xdotool click 4; sleep 0.3   # wheel up
xdotool click 5; sleep 0.3   # wheel down
sleep 1
cp /tmp/wev.log "$OUT/wev.log"

# --- resize: make the client window bigger ------------------------------------
WID=$(xdotool search --pid $CLIENT | tail -1)
xdotool windowsize "$WID" 1600 900
sleep 6
SIZE_RESIZED=$(output_size)
import -window root "$OUT/client-3-resized.png"

# Input after the resize: the pointer extent must follow the new size
read -r FOOT_X FOOT_Y < <(centre foot)
xdotool mousemove "$FOOT_X" "$FOOT_Y" click 1
sleep 0.5
xdotool type --delay 80 'echo resized-$((6*7)) > /tmp/typed2.txt'
xdotool key Return
sleep 2
import -window root "$OUT/client-4-typed-after-resize.png"

# --- reconnect: a new connection must reach the same session ---------------
SWAY_PID=$(pgrep -u tester -x sway)
kill $CLIENT 2>/dev/null || true
sleep 3
${XFREERDP:-xfreerdp3} /v:127.0.0.1 /u:tester /p:wlup-test-pw /cert:ignore \
    /size:1600x900 /bpp:$BPP -grab-keyboard > "$OUT/client-reconnect.log" 2>&1 &
CLIENT=$!
sleep 8
SWAY_PID_AFTER=$(pgrep -u tester -x sway)
import -window root "$OUT/client-5-reconnected.png"
read -r FOOT_X FOOT_Y < <(centre foot)
xdotool mousemove "$FOOT_X" "$FOOT_Y" click 1
sleep 0.5
xdotool type --delay 80 'echo again-$((6*7)) > /tmp/typed3.txt'
xdotool key Return
sleep 2

# --- GFX H.264: a 32 bpp AVC420 client gets wlup's frames through the
# encoder, paced by frame acks -----------------------------------------------
kill $CLIENT 2>/dev/null || true
sleep 3
${XFREERDP:-xfreerdp3} /v:127.0.0.1 /u:tester /p:wlup-test-pw /cert:ignore \
    /size:1600x900 /bpp:32 /gfx:AVC420 -grab-keyboard > "$OUT/client-h264.log" 2>&1 &
CLIENT=$!
sleep 8
SWAY_PID_H264=$(pgrep -u tester -x sway)
import -window root "$OUT/client-6-h264.png"
read -r FOOT_X FOOT_Y < <(centre foot)
xdotool mousemove "$FOOT_X" "$FOOT_Y" click 1
sleep 0.5
xdotool type --delay 80 'echo h264-$((6*7)) > /tmp/typed4.txt'
xdotool key Return
sleep 2
import -window root "$OUT/client-7-h264-typed.png"

# --- GFX H.264 on two monitors: a frame per monitor, each on its own GFX
# surface. The client runs on an Xorg with two 800x600 monitors side by
# side (xorg-2mon.conf), so the session is 1600x600, foot left on
# monitor 0 and wev right on monitor 1. Clicking one of them moves the
# focus, and both title bars change: each surface must show it --------------
kill $CLIENT 2>/dev/null || true
sleep 3
Xorg :98 -noreset -extension RANDR -config "$SRC/wlup/test/xorg-2mon.conf" \
    -logfile "$OUT/xorg-2mon.log" > /dev/null 2>&1 &
XORG2=$!
sleep 3
DISPLAY=:98 ${XFREERDP:-xfreerdp3} /v:127.0.0.1 /u:tester /p:wlup-test-pw \
    /cert:ignore /multimon /bpp:32 /gfx:AVC420 -grab-keyboard \
    > "$OUT/client-2mon.log" 2>&1 &
CLIENT=$!
sleep 8
SIZE_2MON=$(output_size)
# title bar colour at x, 8: sway's focused one is #285577
title_at() {
    DISPLAY=:98 import -window root -crop "1x1+$1+8" -depth 8 txt:- 2> /dev/null |
        grep -o '#[0-9A-F]\{6\}' | head -1
}
read -r WEV_X WEV_Y < <(centre wev)
read -r FOOT_X FOOT_Y < <(centre foot)
echo "two monitors: output $SIZE_2MON, foot at $FOOT_X,$FOOT_Y  wev at $WEV_X,$WEV_Y"
DISPLAY=:98 xdotool mousemove "$WEV_X" "$WEV_Y" click 1
sleep 2
WEV_FOCUS_L=$(title_at 700)
WEV_FOCUS_R=$(title_at 1500)
DISPLAY=:98 import -window root "$OUT/client-8-2mon-wev.png"
DISPLAY=:98 xdotool mousemove "$FOOT_X" "$FOOT_Y" click 1
sleep 2
FOOT_FOCUS_L=$(title_at 700)
FOOT_FOCUS_R=$(title_at 1500)
DISPLAY=:98 import -window root "$OUT/client-9-2mon-foot.png"
echo "title bars, wev focused: $WEV_FOCUS_L $WEV_FOCUS_R;" \
     "foot focused: $FOOT_FOCUS_L $FOOT_FOCUS_R"

# --- AV1, FreeRDP's GFX extension: with "AV1" first in the codec order, a
# client that offers it gets the H.264 pipeline's frames encoded as AV1,
# and a client that doesn't still gets H.264. i420 is 4:2:0; with i444,
# wlup sends the colours at full resolution and AV1 encodes 4:4:4. Needs xrdp built with libaom
# and a FreeRDP built with WITH_GFX_AV1 (wlup/test/freerdp-av1) ------------
XFREERDP_AV1=${XFREERDP_AV1:-/opt/freerdp-av1/bin/xfreerdp}
AV1_RAN=0
if [ -x "$XFREERDP_AV1" ] && grep -q "av1 (libaom) *yes" "$OUT/configure.log"; then
    AV1_RAN=1
    kill $CLIENT 2>/dev/null || true
    sleep 3
    sed -i 's/^order = .*/order = [ "AV1", "H.264", "RFX" ]/' /etc/xrdp/gfx.toml
    # Debian's client has no AV1: H.264 as before
    log_from=$(($(wc -l < /var/log/xrdp.log) + 1))
    ${XFREERDP:-xfreerdp3} /v:127.0.0.1 /u:tester /p:wlup-test-pw /cert:ignore \
        /size:1600x900 /bpp:32 /gfx:AVC420 -grab-keyboard \
        > "$OUT/client-av1-fallback.log" 2>&1 &
    CLIENT=$!
    sleep 8
    AV1_FALLBACK_LOG=$(tail -n +"$log_from" /var/log/xrdp.log)
    kill $CLIENT 2>/dev/null || true
    sleep 3
    # The AV1 build with AV1 off: H.264, with its OpenH264. (It offers AV1
    # by default, even with /gfx:AVC420: FreeRDP_GfxCodecAV1 is on)
    log_from=$(($(wc -l < /var/log/xrdp.log) + 1))
    "$XFREERDP_AV1" /v:127.0.0.1 /u:tester /p:wlup-test-pw /cert:ignore \
        /size:1600x900 /bpp:32 /gfx:AVC420,AV1:off -grab-keyboard \
        > "$OUT/client-av1-build-h264.log" 2>&1 &
    CLIENT=$!
    sleep 8
    import -window root "$OUT/client-av1-build-h264.png"
    AV1_BUILD_H264_LOG=$(tail -n +"$log_from" /var/log/xrdp.log)
    kill $CLIENT 2>/dev/null || true
    sleep 3
    # i420: 4:2:0, as the H.264 pipeline's NV12
    log_from=$(($(wc -l < /var/log/xrdp.log) + 1))
    "$XFREERDP_AV1" /v:127.0.0.1 /u:tester /p:wlup-test-pw /cert:ignore \
        /size:1600x900 /bpp:32 /gfx:AV1:i420 -grab-keyboard \
        > "$OUT/client-av1-i420.log" 2>&1 &
    CLIENT=$!
    sleep 8
    AV1_I420_LOG=$(tail -n +"$log_from" /var/log/xrdp.log)
    kill $CLIENT 2>/dev/null || true
    sleep 3
    # i444: wlup sends the colours at full resolution, AV1 profile 1
    log_from=$(($(wc -l < /var/log/xrdp.log) + 1))
    "$XFREERDP_AV1" /v:127.0.0.1 /u:tester /p:wlup-test-pw /cert:ignore \
        /size:1600x900 /bpp:32 /gfx:AV1:i444 -grab-keyboard \
        > "$OUT/client-av1.log" 2>&1 &
    CLIENT=$!
    sleep 8
    import -window root "$OUT/client-10-av1.png"
    av1_title_at() {
        import -window root -crop "1x1+$1+8" -depth 8 txt:- 2> /dev/null |
            grep -o '#[0-9A-F]\{6\}' | head -1
    }
    read -r FOOT_X FOOT_Y < <(centre foot)
    read -r WEV_X WEV_Y < <(centre wev)
    xdotool mousemove "$FOOT_X" "$FOOT_Y" click 1
    sleep 0.5
    xdotool type --delay 80 'echo av1-$((6*7)) > /tmp/typed5.txt'
    xdotool key Return
    sleep 2
    AV1_FOOT_L=$(av1_title_at 700)
    AV1_FOOT_R=$(av1_title_at 1500)
    xdotool mousemove "$WEV_X" "$WEV_Y" click 1
    sleep 2
    AV1_WEV_L=$(av1_title_at 700)
    AV1_WEV_R=$(av1_title_at 1500)
    import -window root "$OUT/client-11-av1-wev.png"
    AV1_LOG=$(tail -n +"$log_from" /var/log/xrdp.log)
    echo "AV1 title bars, foot focused: $AV1_FOOT_L $AV1_FOOT_R;" \
         "wev focused: $AV1_WEV_L $AV1_WEV_R"
fi
# focused <#RRGGBB>: near sway's focused title bar colour. H.264 is
# lossy, so the client shows e.g. #275475 for #285577
focused() {
    [[ $1 =~ ^#[0-9A-Fa-f]{6}$ ]] || return 1
    local r=$((16#${1:1:2})) g=$((16#${1:3:2})) b=$((16#${1:5:2}))
    [ $(((r - 0x28) ** 2 + (g - 0x55) ** 2 + (b - 0x77) ** 2)) -le \
        $((3 * 12 ** 2)) ]
}

echo "=== result ==="
echo "sway output: connected $SIZE_CONNECTED, resized $SIZE_RESIZED"
pass=0
fail=0
check() {
    if eval "$2"; then
        echo "PASS  $1"; pass=$((pass + 1))
    else
        echo "FAIL  $1"; fail=$((fail + 1))
    fi
}
check "sesman: sway runs as the logged-in user" \
    'pgrep -u tester -x sway > /dev/null'
check "connect: output has the client size 1280x800" \
    '[ "$SIZE_CONNECTED" = 1280x800 ]'
check "keyboard: typed command ran in sway" \
    '[ "$(cat /tmp/typed.txt 2>/dev/null)" = wlup-typed-42 ]'
check "bitmap: paced by the client's frame acks" \
    'grep -q "wlup: bitmap output is paced by the client.s frame acks" /var/log/xrdp.log'
check "bitmap: every frame acknowledged in time (no ack timeout)" \
    '! grep -q "wlup: no ack for bitmap frame" /var/log/xrdp.log'
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
check "resize: output follows client window 1600x900" \
    '[ "$SIZE_RESIZED" = 1600x900 ]'
check "resize: input still works after resize" \
    '[ "$(cat /tmp/typed2.txt 2>/dev/null)" = resized-42 ]'
check "reconnect: same sway process" \
    '[ -n "$SWAY_PID" ] && [ "$SWAY_PID" = "$SWAY_PID_AFTER" ]'
check "reconnect: input works in the reconnected session" \
    '[ "$(cat /tmp/typed3.txt 2>/dev/null)" = again-42 ]'
check "h264: the 32 bpp client reaches the same session" \
    '[ -n "$SWAY_PID" ] && [ "$SWAY_PID" = "$SWAY_PID_H264" ]'
check "h264: frames go to the GFX H.264 encoder" \
    'grep -q "wlup: frames go to the GFX H.264 encoder" /var/log/xrdp.log &&
     grep -q "wlup: first H.264 frames sent" /var/log/xrdp.log'
check "h264: every frame acknowledged in time (no ack timeout)" \
    '! grep -q "wlup: no ack for frame" /var/log/xrdp.log'
check "h264: client shows more than a flat screen" \
    '[ "$(convert "$OUT/client-6-h264.png" -format %k info:)" -gt 50 ]'
check "h264: input works" \
    '[ "$(cat /tmp/typed4.txt 2>/dev/null)" = h264-42 ]'
check "h264 two monitors: the output spans both, 1600x600" \
    '[ "$SIZE_2MON" = 1600x600 ]'
check "h264 two monitors: xrdp maps a surface per monitor" \
    'grep -q "map surface_id 1 left 800 top 0 width 800 height 600" /var/log/xrdp.log'
check "h264 two monitors: wlup sends frames for both" \
    'grep -q "wlup: first H.264 frames sent, 2 monitors" /var/log/xrdp.log ||
     grep -q "wlup: H.264 frame [0-9]*, monitor 1 " /var/log/xrdp.log'
check "h264 two monitors: right monitor shows wev focused" \
    'focused "$WEV_FOCUS_R" && ! focused "$WEV_FOCUS_L"'
check "h264 two monitors: left monitor shows foot focused" \
    'focused "$FOOT_FOCUS_L" && ! focused "$FOOT_FOCUS_R"'
if [ "$AV1_RAN" = 1 ]; then
    check "av1: with AV1 first, a client without it still gets H.264" \
        'echo "$AV1_FALLBACK_LOG" | grep -q "Matched H264 mode" &&
         echo "$AV1_FALLBACK_LOG" | grep -q "starting h264 codec session gfx"'
    check "av1: the AV1 build with AV1 off gets H.264 and draws it" \
        'echo "$AV1_BUILD_H264_LOG" | grep -q "Matched H264 mode" &&
         ! echo "$AV1_BUILD_H264_LOG" | grep -q "Matched AV1 mode" &&
         [ "$(convert "$OUT/client-av1-build-h264.png" -format %k info:)" -gt 50 ]'
    check "av1: i420 gets AV1 in 4:2:0" \
        'echo "$AV1_I420_LOG" | grep -q "Matched AV1 mode" &&
         echo "$AV1_I420_LOG" | grep -q "encoder for 1600x900 (picture 1600x900, 4:2:0)" &&
         ! echo "$AV1_I420_LOG" | grep -q "AV1 in 4:4:4"'
    check "av1: the AV1 client gets AV1" \
        'echo "$AV1_LOG" | grep -q "Matched AV1 mode" &&
         echo "$AV1_LOG" | grep -q "starting AV1 codec session gfx" &&
         echo "$AV1_LOG" | grep -q "xrdp_encoder_av1: encoder for 1600x900"'
    check "av1: i444 gets 4:4:4 from wlup to the encoder" \
        'echo "$AV1_LOG" | grep -q "AV1 in 4:4:4: frames come as yuv444" &&
         echo "$AV1_LOG" | grep -q "wlup: frames go to the GFX encoder, as yuv444" &&
         echo "$AV1_LOG" | grep -q "encoder for 1600x900 (picture 1600x900, 4:4:4)"'
    check "av1: no frame dropped by the encoder" \
        '! echo "$AV1_LOG" | grep -q "dropped"'
    check "av1: the client decodes without errors" \
        '! grep -E "\[ERROR\].*(rdpgfx|codec|gdi)" "$OUT/client-av1.log"'
    check "av1: client shows more than a flat screen" \
        '[ "$(convert "$OUT/client-10-av1.png" -format %k info:)" -gt 50 ]'
    check "av1: input works" \
        '[ "$(cat /tmp/typed5.txt 2>/dev/null)" = av1-42 ]'
    check "av1: a focus change shows on the client" \
        'focused "$AV1_FOOT_L" && ! focused "$AV1_FOOT_R" &&
         focused "$AV1_WEV_R" && ! focused "$AV1_WEV_L"'
else
    echo "SKIP  av1: no AV1 client ($XFREERDP_AV1) or xrdp without libaom"
fi

# With systemd as PID 1 (wlup/test/systemd), sesman's PAM login goes
# through pam_systemd: the session must be a real logind session
if [ "$(cat /proc/1/comm)" = systemd ]; then
    SWAY_ENV=$(tr '\0' '\n' < /proc/"$SWAY_PID_AFTER"/environ)
    SWAY_RUNTIME=$(echo "$SWAY_ENV" | sed -n 's/^XDG_RUNTIME_DIR=//p')
    SWAY_SESSION=$(echo "$SWAY_ENV" | sed -n 's/^XDG_SESSION_ID=//p')
    loginctl list-sessions --no-legend > "$OUT/loginctl-sessions.txt" 2>&1 || true
    [ -n "$SWAY_SESSION" ] &&
        loginctl show-session "$SWAY_SESSION" > "$OUT/loginctl-session.txt" 2>&1
    echo "sway: XDG_RUNTIME_DIR=$SWAY_RUNTIME XDG_SESSION_ID=$SWAY_SESSION"
    check "logind: sway's runtime dir is /run/user/<uid>" \
        '[ "$SWAY_RUNTIME" = "/run/user/$(id -u tester)" ]'
    check "logind: sway is in a logind session of tester" \
        'grep -q "^Name=tester$" "$OUT/loginctl-session.txt" &&
         grep -q "^State=active$\|^State=online$" "$OUT/loginctl-session.txt"'
    check "logind: sway's process belongs to that session" \
        '[ "$(cat /proc/$SWAY_PID_AFTER/sessionid)" = "$SWAY_SESSION" ] ||
         grep -q "session-$SWAY_SESSION.scope" /proc/$SWAY_PID_AFTER/cgroup'
    check "logind: Wayland socket in /run/user/<uid>" \
        'ls /run/user/$(id -u tester)/wayland-* > /dev/null 2>&1'
    check "audio: chansrv runs in the session" \
        'pgrep -u tester -x xrdp-chansrv > /dev/null'
    check "audio: the session's PipeWire has the xrdp sink" \
        'grep -q "node.name = \"xrdp-sink\"" "$OUT/pw-nodes.txt"'
    check "audio: the client received sound (10+ Wave PDUs)" \
        '[ "$(grep -c "WaveInfo:" "$OUT/client-audio.log")" -ge 10 ]'
fi
echo "passed $pass, failed $fail"

kill $CLIENT $XORG2 2>/dev/null || true
cp /var/log/xrdp.log "$OUT/xrdp-file.log" 2>/dev/null || true
cp /var/log/xrdp-sesman.log "$OUT/sesman-file.log" 2>/dev/null || true
chmod -R a+r "$OUT"
grep -h -E 'wlup|resize_done|Advancing' "$OUT"/xrdp*.log | head -30 || true
grep -h -i -E 'wayland|compositor' "$OUT"/sesman*.log | head -20 || true
