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
| cJSON | MIT | `third_party/cJSON/LICENSE` |
| minimp3 | CC0 | top of `minimp3.h` |
| stb | MIT or public domain | foot of each header |
| OXDK | public domain | its own repository |

Noto Sans under `assets/fonts` is SIL OFL; the text ships beside it.

## Binaries

A release binary is a GPL work: recipients are entitled to the corresponding
source, which is this repository at the commit it was built from. The Xbox 360
binary statically links LGPL FFmpeg, which the same source satisfies.

Neither console's SDK is included and neither is ours to give.
