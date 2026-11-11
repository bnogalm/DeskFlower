#!/bin/bash
# Runs inside the container (tests/docker/Dockerfile): configures and builds the read-only source
# tree mounted at /src into /build (a container volume, so rebuilds are incremental), then runs the
# test suite with a private D-Bus session bus and an Xvfb display. Unit tests use the offscreen
# platform like the CI; the X11 integration tests open $DISPLAY themselves.
# Extra arguments go to ctest (for example: -R tst_x11).
set -euo pipefail

src=${SRC_DIR:-/src}
build=${BUILD_DIR:-/build}

cmake -S "$src" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build "$build" -- -k 0

if [ "${BUILD_ONLY:-0}" = 1 ]; then
    exit 0
fi

export QT_QPA_PLATFORM=offscreen
# The display and the session bus below are this run's own: the desktop tests (tst_x11, tst_portal)
# may grab, inject and claim names there. They skip themselves without it.
export DESKFLOWER_PRIVATE_SESSION=1
export QT_LOGGING_RULES=${QT_LOGGING_RULES:-deskflower.node.debug=true}
# tst_web and tst_phoneuser need a working OpenSSL; here it is real, so they run too.
exec dbus-run-session -- xvfb-run -a -s "-screen 0 1280x800x24 +extension XTEST +extension XFIXES" \
    ctest --test-dir "$build" --output-on-failure "$@"
