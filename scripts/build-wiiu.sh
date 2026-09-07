#!/usr/bin/env bash
# Builds the console target and fails loudly.
#
# Written after deploying a stale binary twice while reporting success. Two
# things went wrong: elf2rpl reports failures as "ERROR:" in capitals, which a
# grep for lowercase "error" misses, and ${PIPESTATUS[0]} does not work in zsh
# so the exit code being checked was an empty string. Neither the compiler nor
# ninja is asked for an opinion here; the exit code decides.
set -euo pipefail

cd "$(dirname "$0")/.."

export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
export DEVKITPPC="${DEVKITPPC:-/opt/devkitpro/devkitPPC}"

BUILD_DIR="${BUILD_DIR:-build-wiiu}"
LOG=$(mktemp)

if ! cmake --build "$BUILD_DIR" > "$LOG" 2>&1; then
    echo "BUILD FAILED" >&2
    grep -iE 'error|failed|undefined reference' "$LOG" | head -20 >&2
    rm -f "$LOG"
    exit 1
fi

# elf2rpl can return zero while printing errors, so check the output too.
if grep -qiE '^ERROR:|fixRelocations failed' "$LOG"; then
    echo "BUILD REPORTED ERRORS despite succeeding:" >&2
    grep -iE '^ERROR:|fixRelocations' "$LOG" | head -10 >&2
    rm -f "$LOG"
    exit 1
fi

rm -f "$LOG"
echo "built $(ls -lh "$BUILD_DIR"/butterjelly.wuhb | awk '{print $5}') at $(date +%H:%M:%S)"
