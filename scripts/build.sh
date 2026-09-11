#!/usr/bin/env sh
# Build Butter and Jelly.
#
#   scripts/build.sh              both consoles
#   scripts/build.sh wiiu         Wii U only
#   scripts/build.sh xenon        Xbox 360 only
#   scripts/build.sh xbox         Original Xbox only
#   scripts/build.sh clean        remove build output
#
# Requirements are checked before anything is built, and each one names what to
# install if it is missing. See docs/building.md.

set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

VERSION=$(sed -n 's/.*kAppVersion *= *"\([^"]*\)".*/\1/p' src/core/platform.h)
OUT=$ROOT/dist

die() { echo "$*" >&2; exit 1; }

have() { command -v "$1" >/dev/null 2>&1; }

check_wiiu() {
    [ -n "$DEVKITPRO" ] || DEVKITPRO=/opt/devkitpro
    [ -d "$DEVKITPRO" ] || die "devkitPro not found at $DEVKITPRO.
Install devkitPPC and the wut and wiiu-sdl2 packages with devkitPro pacman:
  https://devkitpro.org/wiki/Getting_Started
Then set DEVKITPRO if it is not at /opt/devkitpro."
    [ -f "$DEVKITPRO/cmake/WiiU.cmake" ] || die "$DEVKITPRO has no cmake/WiiU.cmake. Install the wut package."
    have cmake || die "cmake is needed for the Wii U build."
    have ninja || die "ninja is needed for the Wii U build."
    export DEVKITPRO
    export DEVKITPPC=${DEVKITPPC:-$DEVKITPRO/devkitPPC}
}

check_xenon() {
    [ -n "$OXDK_DIR" ] || OXDK_DIR=$ROOT/third_party/OXDK
    [ -f "$OXDK_DIR/oxdk.mk" ] || die "OXDK not found at $OXDK_DIR.
It is a submodule of this repository:
  git submodule update --init
Or clone it yourself and set OXDK_DIR:
  git clone https://github.com/MrMilenko/OXDK"
    have make || die "make is needed for the Xbox 360 build."
    export OXDK_DIR
    # OXDK finds the compiler and the XDK. Ask it before building, so a missing
    # piece is named here rather than part way through a link.
    "$OXDK_DIR/scripts/doctor.sh" >/dev/null 2>&1 || true
    if [ -z "$XDK_DIR" ] && [ ! -d "$HOME/xdk360" ] && [ ! -d "$HOME/xdk360-extract" ]; then
        echo "warning: no Xbox 360 XDK found. Set XDK_DIR. See docs/building.md." >&2
    fi
}

build_wiiu() {
    check_wiiu
    echo "Building for the Wii U"
    cmake -S . -B build/wiiu -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/WiiU.cmake" \
        -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build build/wiiu -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
    package_wiiu
}

# One archive per console, each holding only what that console needs and its
# own instructions.
package_wiiu() {
    stage=$OUT/butterandjelly-$VERSION-wiiu
    rm -rf "$stage"; mkdir -p "$stage"
    cp build/wiiu/butterandjelly.wuhb "$stage/"
    sed -e "s/@VERSION@/$VERSION/" packaging/README.wiiu.txt > "$stage/README.txt"
    cp packaging/server.txt.example packaging/seerr.txt.example "$stage/"
    (cd "$OUT" && rm -f "butterandjelly-$VERSION-wiiu.zip" &&
     zip -qr "butterandjelly-$VERSION-wiiu.zip" "butterandjelly-$VERSION-wiiu")
    rm -rf "$stage"
    echo "  $OUT/butterandjelly-$VERSION-wiiu.zip"
}

check_xbox() {
    [ -n "$OXDK_DIR" ] || OXDK_DIR=$ROOT/third_party/OXDK
    [ -f "$OXDK_DIR/oxdk.mk" ] || die "OXDK not found at $OXDK_DIR.
It is a submodule of this repository:
  git submodule update --init
Or clone it yourself and set OXDK_DIR:
  git clone https://github.com/MrMilenko/OXDK"
    have make || die "make is needed for the original Xbox build."
    have nasm || die "nasm is needed for libavcodec's x86 assembly.
Build without it using FFMPEG_SIMD=0, at about half the decode speed."
    export OXDK_DIR
    # The 2003 XDK is not redistributable, so the submodule ships without it.
    if [ -z "$XDK_DIR" ] && [ ! -f "$OXDK_DIR/xbox/xdk/lib/xboxkrnl.lib" ]; then
        die "No Xbox XDK found. Copy its lib/*.lib and include/ into
  $OXDK_DIR/xbox/xdk/
or set XDK_DIR to a directory holding lib/xboxkrnl.lib. See docs/building.md."
    fi
    [ -n "$XDK_DIR" ] && export XDK_DIR
    "$OXDK_DIR/scripts/doctor.sh" >/dev/null 2>&1 || true
}

build_xbox() {
    check_xbox
    use_object_tree xbox
    echo "Building for the original Xbox"
    make -f Makefile.xbox -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
    package_xbox
}

package_xbox() {
    stage=$OUT/butterandjelly-$VERSION-xbox
    rm -rf "$stage"; mkdir -p "$stage/butterandjelly/fonts" "$stage/butterandjelly/icons"
    cp bin/default.xbe "$stage/butterandjelly/"
    cp assets/fonts/NotoSans-*.ttf assets/fonts/input_xbox.ttf \
       assets/fonts/OFL.txt assets/fonts/KENNEY-CC0.txt "$stage/butterandjelly/fonts/"
    sed -e "s/@VERSION@/$VERSION/" packaging/README.xbox.txt > "$stage/README.txt"
    cp assets/icons/jellyfin.png assets/icons/seerr.png assets/icons/gear.png "$stage/butterandjelly/icons/"
    cp packaging/server.txt.example packaging/seerr.txt.example "$stage/butterandjelly/"
    (cd "$OUT" && rm -f "butterandjelly-$VERSION-xbox.zip" &&
     zip -qr "butterandjelly-$VERSION-xbox.zip" "butterandjelly-$VERSION-xbox")
    rm -rf "$stage"
    echo "  $OUT/butterandjelly-$VERSION-xbox.zip"
}


# Both Xbox makefiles compile to objects beside their sources, so the targets
# share one tree. Switching between them has to empty it.
STAMP=$ROOT/.build-target
use_object_tree() {
    if [ "$(cat "$STAMP" 2>/dev/null)" != "$1" ]; then
        find src third_party -name '*.o' -delete 2>/dev/null || true
        find src third_party -name '*.d' -delete 2>/dev/null || true
    fi
    echo "$1" > "$STAMP"
}

build_xenon() {
    check_xenon
    use_object_tree xenon
    echo "Building for the Xbox 360"
    make -f Makefile.xenon -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
    package_xenon
}

package_xenon() {
    stage=$OUT/butterandjelly-$VERSION-xbox360
    rm -rf "$stage"; mkdir -p "$stage/butterandjelly/fonts" "$stage/butterandjelly/icons"
    cp default.xex "$stage/butterandjelly/"
    cp assets/fonts/NotoSans-*.ttf assets/fonts/input_xbox.ttf \
       assets/fonts/OFL.txt assets/fonts/KENNEY-CC0.txt "$stage/butterandjelly/fonts/"
    sed -e "s/@VERSION@/$VERSION/" packaging/README.xbox360.txt > "$stage/README.txt"
    cp assets/icons/jellyfin.png assets/icons/seerr.png assets/icons/gear.png "$stage/butterandjelly/icons/"
    cp packaging/server.txt.example packaging/seerr.txt.example "$stage/butterandjelly/"
    (cd "$OUT" && rm -f "butterandjelly-$VERSION-xbox360.zip" &&
     zip -qr "butterandjelly-$VERSION-xbox360.zip" "butterandjelly-$VERSION-xbox360")
    rm -rf "$stage"
    echo "  $OUT/butterandjelly-$VERSION-xbox360.zip"
}

case "${1:-all}" in
    wiiu)   build_wiiu ;;
    xenon|xbox360|360) build_xenon ;;
    xbox|ogxbox) build_xbox ;;
    clean)  rm -rf build dist bin default.xex; find src third_party -name '*.o' -delete 2>/dev/null || true; find src third_party -name '*.d' -delete 2>/dev/null || true; rm -f "$ROOT/.build-target"; echo "cleaned" ;;
    all)    build_wiiu; build_xenon; build_xbox ;;
    *)      die "usage: build.sh [wiiu|xenon|xbox|clean|all]" ;;
esac
