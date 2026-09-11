# Building

```sh
git clone --recursive https://github.com/MrMilenko/ButterAndJelly
cd ButterAndJelly
scripts/build.sh            # all three consoles
scripts/build.sh wiiu
scripts/build.sh xenon
scripts/build.sh xbox
scripts/build.sh clean

scripts/package-macos.sh    # Darwin Butter and Jelly.app
```

One archive per console in `dist/`. Requirements are checked first and anything
missing is named.

Cloned without `--recursive`: `git submodule update --init`.

## Requirements

| | macOS | Wii U | Xbox 360 | Xbox |
| --- | --- | --- | --- | --- |
| Toolchain | clang | devkitPPC and wut | OXDK, submodule in `third_party/OXDK` | OXDK |
| Build | cmake, ninja | cmake, ninja | make | make, nasm |
| SDK | none | none | Xbox 360 XDK, yours | 2003 Xbox XDK, yours |
| Output | `.app` | `butterandjelly.wuhb` | `default.xex` and `fonts/` | `default.xbe` and `fonts/` |

## macOS

Homebrew: `sdl2`, `sdl2_image`, `sdl2_ttf`, `mpg123`, `ffmpeg`, and
`dylibbundler` to make the bundle self contained. Homebrew's `sdl2` is
`sdl2-compat`, which loads SDL3 at run time; the packaging script copies that
in as well, since nothing in the link line names it.

## Wii U

devkitPro packages: `wut`, `wiiu-sdl2`, `wiiu-sdl2_ttf`, `wiiu-sdl2_image`,
`wiiu-mpg123`, `wiiu-curl`. Install with devkitPro pacman,
<https://devkitpro.org/wiki/Getting_Started>.

`DEVKITPRO` defaults to `/opt/devkitpro`.

## Xbox 360

OXDK needs a clang with the Xenon ABI patches. It builds one:

```sh
third_party/OXDK/oxdk build-llvm     # about 20 GB, takes a while
third_party/OXDK/oxdk doctor         # what it found
```

Set `XDK_DIR` to your XDK, the directory holding `lib/xbox/xboxkrnl.lib`.
`~/xdk360/XDK` and `~/xdk360-extract/sdk/XDK` are found automatically.

## Xbox

The same OXDK checkout as the 360, targeting x86 instead, so no separate
toolchain build is needed. It uses the 2003 Xbox XDK rather than the 360's,
which is not redistributable and so is not in the submodule. Copy its
`lib/*.lib` and `include/` into `third_party/OXDK/xbox/xdk/`, or set
`XDK_DIR` to a directory holding `lib/xboxkrnl.lib`.

```sh
make -f Makefile.xbox
```

nasm assembles libavcodec's MMX and SSE paths. Without it:

```sh
make -f Makefile.xbox FFMPEG_SIMD=0
```

which builds the same tree in plain C at roughly half the decode speed. Useful
for telling a decoder fault from a bad input.

## Desktop

For testing the shared code. Needs SDL2, SDL2_ttf, SDL2_image, libcurl,
libmpg123 and FFmpeg. Plain cmake.
