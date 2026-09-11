# Licensing

`SPDX-License-Identifier: GPL-2.0-or-later`, full text in
[LICENSE](../LICENSE). Covers `src`, the build system, the scripts and the
documentation.

## Third party

All GPL-2.0-or-later compatible.

| | license | stated in |
| --- | --- | --- |
| FFmpeg 1.1 | LGPL-2.1-or-later | each source header, `CONFIG_GPL 0` |
| SDL2x360 | zlib | each source header |
| RXDK-SDL2x | zlib | each source header |
| cJSON | MIT | `third_party/cJSON/LICENSE` |
| minimp3 | CC0 | top of `minimp3.h` |
| stb | MIT or public domain | foot of each header |
| OXDK | public domain | its own repository |

Noto Sans under `assets/fonts` is SIL OFL; the text ships beside it.

The input glyph fonts under `assets/fonts` are Kenney's Input Prompts,
CC0, from <https://kenney.nl/assets/input-prompts>. CC0 asks for nothing,
but the credit is deserved.

Icons under `assets/icons`:

| | source | license |
| --- | --- | --- |
| `gear.png` | Font Awesome Free 7.3.1 | CC BY 4.0, recolored |
| `jellyfin.png` | the Jellyfin project | CC BY-SA 4.0 |
| `seerr.png` | the Seerr project | its own, see below |
| `app.png` and the `wiiu-` images | this project | GPL-2.0-or-later |

All were rasterized from SVG and otherwise unaltered, apart from the gear,
which was recolored white so it can be tinted at runtime. The app icon and the
Wii U images are the same artwork at different sizes.

Every service icon here is used to name the service it belongs to, which is
nominative use rather than any claim to the mark.

## Linked at build time

Not vendored here, but statically linked into a release binary and so covered
by the same obligation. The Wii U build takes these from devkitPro.

| | license |
| --- | --- |
| libmpg123 | LGPL-2.1-or-later |
| mbed TLS | Apache-2.0 or GPL-2.0-or-later |
| libcurl | curl (MIT style) |
| SDL2, SDL2_ttf, SDL2_image | zlib |
| FreeType | FTL or GPL-2.0-or-later |
| libpng, libjpeg-turbo, zlib | their own permissive terms |

## Binaries

A release binary is a GPL work: recipients are entitled to the corresponding
source, which is this repository at the commit it was built from, including
its submodules. Both Xbox binaries statically link LGPL FFmpeg and the Wii U
binary statically links LGPL libmpg123; for those, the LGPL's relinking
requirement is met by the same source plus the published build scripts.

No console SDK is included and none of them are ours to give.
