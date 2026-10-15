# The RDP client for the AV1 tests in run-e2e.sh: an X11 FreeRDP built from
# upstream with the GFX AV1 extension (WITH_GFX_AV1, decoded by dav1d),
# installed in /opt/freerdp-av1. Debian's FreeRDP is built without it.
# Without ffmpeg or OpenH264 this client has no H.264: the H.264 tests keep
# using Debian's xfreerdp3
FROM xrdp-wlup-dev

ARG FREERDP_COMMIT=c44ef40d5786c007c8f4dff46ccc4904d5db6b3c

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build pkg-config ca-certificates curl \
        libdav1d-dev libssl-dev libx11-dev libxext-dev libxcursor-dev libxinerama-dev \
        libxrandr-dev libxkbfile-dev libxi-dev libxrender-dev libxfixes-dev \
        libxdamage-dev libxtst-dev libxv-dev libkrb5-dev libcjson-dev zlib1g-dev \
        liburiparser-dev libcups2-dev \
    && rm -rf /var/lib/apt/lists/*

# WITH_DAV1D must be asked for: WITH_GFX_AV1 alone builds no decoder.
# pipefail: tee would hide a failed configure
SHELL ["/bin/bash", "-o", "pipefail", "-c"]
RUN mkdir /tmp/freerdp-src \
    && curl -fsSL "https://github.com/FreeRDP/FreeRDP/archive/$FREERDP_COMMIT.tar.gz" | \
        tar -xz --strip-components=1 -C /tmp/freerdp-src \
    && cmake -G Ninja -S /tmp/freerdp-src -B /tmp/build \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/freerdp-av1 \
        -DCMAKE_INSTALL_RPATH=/opt/freerdp-av1/lib -DCMAKE_SKIP_INSTALL_RPATH=OFF \
        -DWITH_GFX_AV1=ON -DWITH_DAV1D=ON -DWITH_X11=ON -DWITH_SDL=OFF \
        -DWITH_CLIENT_SDL=OFF -DWITH_SERVER=OFF -DWITH_SAMPLE=OFF \
        -DWITH_MANPAGES=OFF -DBUILD_TESTING=OFF -DWITH_FFMPEG=OFF \
        -DWITH_SWSCALE=OFF -DCHANNEL_URBDRC=OFF 2>&1 | tee /tmp/cmake.log \
    && grep -q "AV1 decoder: dav1d" /tmp/cmake.log \
    && cmake --build /tmp/build && cmake --install /tmp/build \
    && echo "$FREERDP_COMMIT" > /opt/freerdp-av1/SOURCE_COMMIT \
    && rm -rf /tmp/build /tmp/freerdp-src
