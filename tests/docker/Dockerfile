# Linux build and test environment (README, "Linux tests in Docker"): Ubuntu 24.04 with the same
# packages as the CI (.github/workflows/build.yml), Qt 6.11.2 from the official repository (via
# aqtinstall), and what the X11/Wayland integration tests need: Xvfb, xdotool, xclip, a D-Bus
# session bus, and the PipeWire/portal development files.
FROM ubuntu:24.04

ARG QT_VERSION=6.11.2
ENV DEBIAN_FRONTEND=noninteractive

# The CI's list first (keep in sync with build.yml), then the test tools.
RUN apt-get update && apt-get install -y --no-install-recommends \
        ninja-build pkg-config libsodium-dev libssl-dev \
        libx11-dev libxi-dev libxtst-dev libxrandr-dev libxfixes-dev libxext-dev libei-dev \
        libpipewire-0.3-dev \
        libgl1-mesa-dev libxkbcommon-dev libxkbcommon-x11-dev libfontconfig1-dev libfreetype-dev \
        libxcb-cursor0 dpkg-dev file \
        xvfb xclip dbus \
    && apt-get install -y --no-install-recommends \
        build-essential cmake git ca-certificates python3 python3-venv \
        libglib2.0-0 libegl1 libxcb-icccm4 libxcb-image0 libxcb-keysyms1 libxcb-render-util0 libxcb-shape0 \
        libxcb-xinerama0 libxcb-xkb1 libxkbcommon-x11-0 libxcb-randr0 libxcb-xfixes0 libxcb-sync1 \
        xauth x11-utils xdotool dbus-x11 procps \
    && rm -rf /var/lib/apt/lists/*

# Qt from the official online repository, unattended (aqtinstall pinned, so that the image is
# reproducible).
RUN python3 -m venv /opt/aqt \
    && /opt/aqt/bin/pip install --no-cache-dir aqtinstall==3.3.0 \
    && /opt/aqt/bin/aqt install-qt linux desktop ${QT_VERSION} linux_gcc_64 --outputdir /opt/Qt \
    && rm -rf /opt/aqt /root/.cache aqtinstall.log

ENV CMAKE_PREFIX_PATH=/opt/Qt/${QT_VERSION}/gcc_64 \
    PATH=/opt/Qt/${QT_VERSION}/gcc_64/bin:${PATH}

COPY build-and-test.sh /usr/local/bin/build-and-test.sh
RUN chmod +x /usr/local/bin/build-and-test.sh
WORKDIR /build
ENTRYPOINT ["/usr/local/bin/build-and-test.sh"]
