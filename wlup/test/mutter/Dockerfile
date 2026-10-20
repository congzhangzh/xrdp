# Test bed for the Mutter backend of wlup: headless GNOME Shell plus the
# libraries the backend builds against (sd-bus, PipeWire, libei), and
# gnome-session to start a whole GNOME session
FROM xrdp-wlup-gnome

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        libsystemd-dev libpipewire-0.3-dev libei-dev gnome-session xclip \
        pipewire-module-xrdp pipewire-bin pulseaudio pulseaudio-utils \
    && rm -rf /var/lib/apt/lists/*

# A PulseAudio daemon for the RDP client side only (the microphone test):
# don't start it in users' sessions, they use PipeWire
RUN systemctl --global disable pulseaudio.socket pulseaudio.service

# These containers run privileged, so /sys is the host's: udev's
# coldplug trigger would replay every device event on the host, where
# the console's login screen (gdm's gnome-shell) and logind react to
# them, and the greeter has crashed on it. Nothing here needs it
RUN systemctl mask systemd-udev-trigger.service
