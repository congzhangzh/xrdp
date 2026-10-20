#!/bin/sh
#
# Starts the KDE Plasma session of an xrdp KDE session
# (sesman.ini [KDE] section, wlup module with backend=kwin).
#
# KWin must be exec'd from this script: sesman watches the PID it
# started, finds the Wayland socket that process holds the lock of, and
# ends the session when it exits. KWin then starts Plasma, and exits with
# it (--exit-with-session).
#
# Plasma normally starts KWin itself, as a systemd user unit drawing on
# the DRM devices of a seat. An xrdp session has no seat: KWin runs on its
# virtual backend here, sized like the RDP client. Plasma's classic boot
# (systemdBoot=false) starts the other parts directly, and leaves KWin
# alone once org.kde.KWinWrapper is on the session bus.

export XDG_SESSION_TYPE=wayland
export XDG_CURRENT_DESKTOP=KDE
export XDG_SESSION_DESKTOP=KDE
export DESKTOP_SESSION=plasma

# Plasma needs the session bus of the user's systemd instance
if [ -z "$DBUS_SESSION_BUS_ADDRESS" ] && [ -S "$XDG_RUNTIME_DIR/bus" ]; then
    export DBUS_SESSION_BUS_ADDRESS="unix:path=$XDG_RUNTIME_DIR/bus"
fi
if [ -z "$DBUS_SESSION_BUS_ADDRESS" ]; then
    echo "startplasma.sh: no session bus ($XDG_RUNTIME_DIR/bus); KDE" \
        "sessions need systemd's user instance (pam_systemd)" >&2
    exit 1
fi

# The classic boot, through a configuration directory of this session
# only; a startkderc of the user's own still comes first
conf="$XDG_RUNTIME_DIR/xrdp-plasma"
mkdir -p "$conf"
printf '[General]\nsystemdBoot=false\n' > "$conf/startkderc"
export XDG_CONFIG_DIRS="$conf:${XDG_CONFIG_DIRS:-/etc/xdg}"

# KWin before 6.8 lets only programs listed in a desktop file cast the
# screen and send input; 6.8 lets every program that is not sandboxed
export KWIN_WAYLAND_NO_PERMISSION_CHECKS=1

# KWin casts its output only when it composites with OpenGL, which takes
# a DRM render node: a machine without a GPU gets one from vgem. The
# session has no seat, so logind gives the user no access to the DRM
# devices: the user needs the groups of the nodes (often render, and
# video for vgem's card node)
usable=
for node in /dev/dri/renderD* /dev/dri/card*; do
    if [ -r "$node" ] && [ -w "$node" ]; then
        usable=$node
        break
    fi
done
if [ -z "$usable" ]; then
    if ls /dev/dri/renderD* > /dev/null 2>&1; then
        echo "startplasma.sh: $(id -un) cannot open the DRM devices" \
            "($(ls -l /dev/dri/renderD* | head -1)); KWin will not be able" \
            "to cast its output: add the user to their groups" >&2
    else
        echo "startplasma.sh: no DRM render node; KWin will not be able to" \
            "cast its output (without a GPU: modprobe vgem)" >&2
    fi
fi

exec kwin_wayland_wrapper --xwayland --virtual \
    --width "${XRDP_START_WIDTH:-1280}" --height "${XRDP_START_HEIGHT:-800}" \
    --exit-with-session=startplasma-wayland
