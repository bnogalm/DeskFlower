#!/bin/bash
# Builds the Linux test image (once) and runs the build and the test suite of this source tree in
# it (README, "Linux tests in Docker"). The source is mounted read-only; the build lives in the
# Docker volume "deskflower-linux-build" (delete it to start clean). Arguments go to ctest, for
# example: tests/docker/run.sh -R 'tst_x11|tst_portal'. BUILD_ONLY=1 skips the tests.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
image=${DESKFLOWER_LINUX_IMAGE:-deskflower-linux-test}
volume=${DESKFLOWER_LINUX_VOLUME:-deskflower-linux-build}

# Git Bash on Windows: Docker wants a Windows path and no MSYS path conversion.
if command -v cygpath > /dev/null 2>&1; then
    root=$(cygpath -w "$root")
    here=$(cygpath -w "$here")
    export MSYS_NO_PATHCONV=1
fi

docker build -t "$image" "$here"
tty=()
[ -t 1 ] && tty=(-t)
docker run --rm "${tty[@]}" \
    -e BUILD_ONLY="${BUILD_ONLY:-0}" \
    -v "$root:/src:ro" \
    -v "$volume:/build" \
    "$image" "$@"
