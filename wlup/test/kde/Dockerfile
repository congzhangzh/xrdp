# Test bed for the KWin backend of wlup: KDE Plasma on KWin's virtual
# backend, systemd as PID 1, plus the libraries the backend builds
# against (PipeWire, the KDE Wayland protocol files)
FROM xrdp-wlup-systemd

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        kwin-wayland plasma-workspace plasma-desktop plasma-wayland-protocols \
        libpipewire-0.3-dev libsystemd-dev libei-dev libkscreen-bin \
        wayland-utils wl-clipboard \
        dolphin konsole \
    && rm -rf /var/lib/apt/lists/*
