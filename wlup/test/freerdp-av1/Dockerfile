# The RDP client for the e2e tests: an X11 FreeRDP built from source with
# the GFX AV1 extension (WITH_GFX_AV1, decoded by dav1d), H.264 (OpenH264)
# and the sound backends the suites use (PulseAudio, ALSA, Opus), installed
# in /opt/freerdp-av1. Debian's FreeRDP is built without AV1.
#
# The source is the stage freerdp_src: upstream's FREERDP_COMMIT by
# default, or any FreeRDP tree given as a build context:
#   docker build --build-context freerdp_src=<dir> \
#       --build-arg FREERDP_COMMIT=<its commit> ...
# (run-all.sh does this for FREERDP_SRC). ccache keeps its cache in a
# BuildKit cache mount, so a changed tree only recompiles what changed.
ARG FREERDP_COMMIT=c44ef40d5786c007c8f4dff46ccc4904d5db6b3c

FROM xrdp-wlup-dev AS freerdp_download
ARG FREERDP_COMMIT
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        ca-certificates curl \
    && rm -rf /var/lib/apt/lists/* \
    && mkdir /freerdp \
    && curl -fsSL "https://github.com/FreeRDP/FreeRDP/archive/$FREERDP_COMMIT.tar.gz" | \
        tar -xz --strip-components=1 -C /freerdp

FROM scratch AS freerdp_src
COPY --from=freerdp_download /freerdp /

FROM xrdp-wlup-dev
ARG FREERDP_COMMIT

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build ccache pkg-config \
        libdav1d-dev libssl-dev libx11-dev libxext-dev libxcursor-dev libxinerama-dev \
        libxrandr-dev libxkbfile-dev libxi-dev libxrender-dev libxfixes-dev \
        libxdamage-dev libxtst-dev libxv-dev libkrb5-dev libcjson-dev zlib1g-dev \
        liburiparser-dev libcups2-dev libopenh264-dev \
        libpulse-dev libasound2-dev libopus-dev \
    && rm -rf /var/lib/apt/lists/*

# WITH_DAV1D must be asked for: WITH_GFX_AV1 alone builds no decoder.
# pipefail: tee would hide a failed configure
SHELL ["/bin/bash", "-o", "pipefail", "-c"]
ENV CCACHE_DIR=/root/.cache/ccache
RUN --mount=type=bind,from=freerdp_src,target=/tmp/freerdp-src,rw \
    --mount=type=cache,id=wlup-freerdp-ccache,target=/root/.cache/ccache \
    cmake -G Ninja -S /tmp/freerdp-src -B /tmp/build \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/freerdp-av1 \
        -DCMAKE_INSTALL_RPATH=/opt/freerdp-av1/lib -DCMAKE_SKIP_INSTALL_RPATH=OFF \
        -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
        -DWITH_GFX_AV1=ON -DWITH_DAV1D=ON -DWITH_OPENH264=ON \
        -DWITH_PULSE=ON -DWITH_ALSA=ON -DWITH_OPUS=ON \
        -DWITH_X11=ON -DWITH_SDL=OFF \
        -DWITH_CLIENT_SDL=OFF -DWITH_SERVER=OFF -DWITH_SAMPLE=OFF \
        -DWITH_MANPAGES=OFF -DBUILD_TESTING=OFF -DWITH_FFMPEG=OFF \
        -DWITH_SWSCALE=OFF -DCHANNEL_URBDRC=OFF 2>&1 | tee /tmp/cmake.log \
    && grep -q "AV1 decoder: dav1d" /tmp/cmake.log \
    && ccache -z > /dev/null \
    && cmake --build /tmp/build && cmake --install /tmp/build \
    && ccache -s | grep -E "Hits|Misses" | head -2 \
    && /opt/freerdp-av1/bin/xfreerdp /buildconfig | tr ' ' '\n' > /tmp/buildconfig \
    && grep -qx WITH_GFX_AV1=ON /tmp/buildconfig \
    && grep -qx WITH_GFX_H264=ON /tmp/buildconfig \
    && grep -qx WITH_PULSE=ON /tmp/buildconfig \
    && echo "$FREERDP_COMMIT" > /opt/freerdp-av1/SOURCE_COMMIT \
    && rm -rf /tmp/build /tmp/cmake.log /tmp/buildconfig
