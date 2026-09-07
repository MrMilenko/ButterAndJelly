#!/usr/bin/env bash
# Pulls screenshots taken with Aroma's screenshot plugin off the console.
#
#   WIIU_IP=192.168.40.18 ./scripts/pull-screenshots.sh
#
# The plugin has to be enabled in its config, and its default combo is the
# TV button. It writes under the title id of whatever is running, and a .wuhb
# launched through homebrew_on_menu runs in the Health and Safety slot rather
# than one of its own. ftpiiu also lists the folder with the title's name
# appended, and only accepts that full name back, so the id alone is refused.
set -euo pipefail

WIIU_IP="${WIIU_IP:-}"
# Defaults to a folder in the home directory rather than the repository, so
# captures do not land in the working tree.
OUT_DIR="${OUT_DIR:-$HOME/wiiuscreenshots}"

if [ -z "$WIIU_IP" ]; then
    echo "Set WIIU_IP to the address shown by the ftpiiu plugin." >&2
    exit 1
fi

ROOT="ftp://${WIIU_IP}/fs/vol/external01/wiiu/screenshots"
CURL=(curl -sS --user "anonymous:anonymous" --connect-timeout 15)

# Pick the most recently written title folder, since that is the one just used.
title=$("${CURL[@]}" "$ROOT/" | awk '$NF != "" { $1=$2=$3=$4=$5=$6=$7=$8=""; sub(/^ +/,""); print }' \
        | grep -v '^$' | tail -1)
if [ -z "$title" ]; then
    echo "No screenshots on the console. Is the plugin enabled?" >&2
    exit 1
fi

encode() { python3 -c 'import sys,urllib.parse;print(urllib.parse.quote(sys.argv[1]))' "$1"; }

titleEnc=$(encode "$title")
day=$("${CURL[@]}" "$ROOT/$titleEnc/" | awk '{print $NF}' | grep -E '^[0-9-]+$' | tail -1)
[ -n "$day" ] || { echo "No dated folder under $title" >&2; exit 1; }

mkdir -p "$OUT_DIR"
echo "From $title / $day"

files=$("${CURL[@]}" "$ROOT/$titleEnc/$day/" | awk '{print $NF}' | grep '\.png$' || true)
[ -n "$files" ] || { echo "Nothing captured yet."; exit 0; }

for name in $files; do
    [ -f "$OUT_DIR/$name" ] && continue
    echo "  $name"
    "${CURL[@]}" "$ROOT/$titleEnc/$day/$(encode "$name")" -o "$OUT_DIR/$name"
done
echo "Saved to $OUT_DIR/  (TV and DRC are the television and the GamePad)"
