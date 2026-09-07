#!/usr/bin/env bash
# Pushes the console build to a Wii U running the ftpiiu plugin.
#
#   WIIU_IP=192.168.40.50 ./scripts/deploy.sh
#
# ftpiiu exposes the SD card at /fs/vol/external01, which is where Aroma and
# the Wii U Menu look for .wuhb bundles.
set -euo pipefail

WIIU_IP="${WIIU_IP:-}"
WIIU_PORT="${WIIU_PORT:-21}"
REMOTE_DIR="${REMOTE_DIR:-/fs/vol/external01/wiiu/apps}"
BUNDLE="${BUNDLE:-build-wiiu/butterjelly.wuhb}"

if [ -z "$WIIU_IP" ]; then
    echo "Set WIIU_IP to the address shown by the ftpiiu plugin." >&2
    exit 1
fi

if [ ! -f "$BUNDLE" ]; then
    echo "No bundle at $BUNDLE. Build the console target first:" >&2
    echo "  cmake -B build-wiiu -G Ninja -DCMAKE_TOOLCHAIN_FILE=\$DEVKITPRO/cmake/WiiU.cmake" >&2
    echo "  cmake --build build-wiiu" >&2
    exit 1
fi

echo "Uploading $(basename "$BUNDLE") ($(du -h "$BUNDLE" | cut -f1)) to ${WIIU_IP}..."

# ftpiiu ignores credentials; anonymous is the convention.
curl --ftp-create-dirs -T "$BUNDLE" \
     --user "anonymous:anonymous" --connect-timeout 10 \
     "ftp://${WIIU_IP}:${WIIU_PORT}${REMOTE_DIR}/$(basename "$BUNDLE")"

echo "Done. Launch it from the Wii U Menu or the Homebrew Launcher."
