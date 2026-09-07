#!/usr/bin/env bash
# Rebuilds assets/shaders/nv12.gsh from the Latte assembly sources.
#
# The compiled .gsh is committed, so this is only needed when the shaders
# change. latte-assembler is not part of devkitPro and has to be built from
# decaf-emu; see docs/gpu-video-path.md.
#
#   LATTE_ASSEMBLER=/path/to/latte-assembler ./scripts/build-shaders.sh
set -euo pipefail

LATTE_ASSEMBLER="${LATTE_ASSEMBLER:-latte-assembler}"

if ! command -v "$LATTE_ASSEMBLER" >/dev/null 2>&1 && [ ! -x "$LATTE_ASSEMBLER" ]; then
    echo "latte-assembler not found. Set LATTE_ASSEMBLER to its path." >&2
    echo "See docs/gpu-video-path.md for how to build it." >&2
    exit 1
fi

cd "$(dirname "$0")/.."
mkdir -p assets/shaders

"$LATTE_ASSEMBLER" assemble \
    --vsh=src/shaders/video.vsh \
    --psh=src/shaders/nv12.psh \
    assets/shaders/nv12.gsh

echo "assets/shaders/nv12.gsh: $(wc -c < assets/shaders/nv12.gsh) bytes"
