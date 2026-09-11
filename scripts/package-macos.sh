#!/usr/bin/env sh
# Build Darwin Butter and Jelly as a double-clickable .app.
#
#   scripts/package-macos.sh
#
# The bundle carries its own assets and its own copies of the libraries it
# links, so it runs on a Mac with no Homebrew.

set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

VERSION=$(sed -n 's/.*kAppVersion *= *"\([^"]*\)".*/\1/p' src/core/platform.h)
APP="$ROOT/dist/Darwin Butter and Jelly.app"

command -v cmake >/dev/null || { echo "cmake is needed." >&2; exit 1; }
command -v ninja >/dev/null || { echo "ninja is needed." >&2; exit 1; }

echo "Building"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build -j"$(sysctl -n hw.ncpu)"

echo "Staging the bundle"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources" "$APP/Contents/Frameworks"

cp build/butterandjelly "$APP/Contents/MacOS/butterandjelly"
cp -R assets "$APP/Contents/Resources/assets"

ICON=""
if [ -f packaging/butterandjelly.icns ]; then
    cp packaging/butterandjelly.icns "$APP/Contents/Resources/butterandjelly.icns"
    ICON='  <key>CFBundleIconFile</key>          <string>butterandjelly</string>'
fi

cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
  "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key>              <string>Butter and Jelly</string>
  <key>CFBundleDisplayName</key>       <string>Darwin Butter and Jelly</string>
  <key>CFBundleIdentifier</key>        <string>com.mikeweekley.butterandjelly</string>
  <key>CFBundleVersion</key>           <string>$VERSION</string>
  <key>CFBundleShortVersionString</key><string>$VERSION</string>
  <key>CFBundleExecutable</key>        <string>butterandjelly</string>
$ICON
  <key>CFBundlePackageType</key>       <string>APPL</string>
  <key>LSMinimumSystemVersion</key>    <string>11.0</string>
  <key>NSHighResolutionCapable</key>   <true/>
  <key>NSLocalNetworkUsageDescription</key>
  <string>Butter and Jelly looks for media servers on your network.</string>
</dict>
</plist>
PLIST

# The libraries it links live in Homebrew, which a Mac that did not build it
# will not have. dylibbundler copies them in and rewrites the load paths.
if command -v dylibbundler >/dev/null; then
    echo "Bundling libraries"
    dylibbundler -cd -of \
        -b -x "$APP/Contents/MacOS/butterandjelly" \
        -d "$APP/Contents/Frameworks" \
        -p "@executable_path/../Frameworks" >/dev/null
else
    echo "warning: dylibbundler not found, the bundle needs Homebrew to run." >&2
fi

# Homebrew's sdl2 is sdl2-compat: SDL2's API sitting on top of SDL3, which it
# opens by hand at startup rather than linking. Nothing in the link line
# mentions it, so dylibbundler cannot know about it. The shim looks beside
# itself first, so a copy in Frameworks is what it finds.
SDL3=$(ls /opt/homebrew/lib/libSDL3.0.dylib /usr/local/lib/libSDL3.0.dylib 2>/dev/null | head -1)
if [ -f "$APP/Contents/Frameworks/libSDL2-2.0.0.dylib" ] && [ -n "$SDL3" ]; then
    echo "Bundling SDL3, which sdl2-compat opens at run time"
    cp "$SDL3" "$APP/Contents/Frameworks/libSDL3.dylib"
    chmod u+w "$APP/Contents/Frameworks/libSDL3.dylib"
    install_name_tool -id "@executable_path/../Frameworks/libSDL3.dylib" \
        "$APP/Contents/Frameworks/libSDL3.dylib"
fi

# Ad hoc, so Gatekeeper lets it start locally. A build for anyone else needs
# a Developer ID signature and notarization.
codesign --force --deep --sign - "$APP" >/dev/null 2>&1 || true

SIZE=$(du -sh "$APP" | cut -f1)
echo "  $APP  ($SIZE)"
