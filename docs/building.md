# Building

```sh
git clone --recursive https://github.com/MrMilenko/ButterAndJelly
cd ButterAndJelly
scripts/build.sh            # all three
scripts/build.sh wiiu
scripts/build.sh xenon
scripts/build.sh xbox
scripts/build.sh clean
```

One archive per console in `dist/`. Requirements are checked first and anything
missing is named.

Cloned without `--recursive`: `git submodule update --init`.

## Requirements

| | Wii U | Xbox 360 | Xbox |
| --- | --- | --- | --- |
| Toolchain | devkitPPC and wut | OXDK, submodule in `third_party/OXDK` | OXDK |
| Build | cmake, ninja | make | make, nasm |
| SDK | none | Xbox 360 XDK, yours | 2003 Xbox XDK, yours |
| Output | `butterandjelly.wuhb` | `default.xex` and `fonts/` | `default.xbe` and `fonts/` |

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
toolchain build is needed. It uses the 2003 Xbox XDK rather than the 360's.

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
