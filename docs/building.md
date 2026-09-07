# Building

```sh
git clone --recursive https://github.com/MrMilenko/ButterAndJelly
cd ButterAndJelly
scripts/build.sh            # both
scripts/build.sh wiiu
scripts/build.sh xenon
scripts/build.sh clean
```

One archive per console in `dist/`. Requirements are checked first and anything
missing is named.

Cloned without `--recursive`: `git submodule update --init`.

## Requirements

| | Wii U | Xbox 360 |
| --- | --- | --- |
| Toolchain | devkitPPC and wut | OXDK, submodule in `third_party/OXDK` |
| Build | cmake, ninja | make |
| SDK | none | Xbox 360 XDK, yours |
| Output | `butterandjelly.wuhb` | `default.xex` and `fonts/` |

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

## Desktop

For testing the shared code. Needs SDL2, SDL2_ttf, SDL2_image, libcurl,
libmpg123 and FFmpeg. Plain cmake.
