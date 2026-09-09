# Butter and Jelly

A Jellyfin client for the Wii U, Xbox 360 and original Xbox.

**Wii U**

<img src="docs/screenshots/wiiu-shows.png" width="420"> <img src="docs/screenshots/wiiu-episodes.png" width="420">
<img src="docs/screenshots/wiiu-playback.png" width="420"> <img src="docs/screenshots/wiiu-gamepad.png" width="420">

The last one is the GamePad, which mirrors playback and can take it on its own.

**Xbox 360**

<img src="docs/screenshots/xbox360-home.png" width="420"> <img src="docs/screenshots/xbox360-library.png" width="420">
<img src="docs/screenshots/xbox360-playback.png" width="420">

All captured from the consoles, not from an emulator.

One codebase, three consoles: two big endian PowerPC and one little endian
Pentium III. `src/core` and `src/ui` are shared; `src/platform` holds what
differs.

## What works

| | Wii U | Xbox 360 | Xbox |
| --- | --- | --- | --- |
| Browse libraries, search, resume | yes | yes | yes |
| Quick Connect sign in | yes | yes | yes |
| Video | up to 480p | up to 720p | up to 480p |
| Poster artwork | yes | yes | yes |

## Getting it

One archive per console on the [releases page](../../releases), each with
install notes inside.

**Wii U.** Aroma. `butterandjelly.wuhb` to `sd:/wiiu/apps/`.

**Xbox 360.** A console that runs unsigned code. Copy `butterandjelly/` to the
hard disk and launch `default.xex`. It writes settings and cached posters
beside itself, so put it somewhere writable.

**Xbox.** A modded console. Copy `butterandjelly/` to the hard disk and launch
`default.xbe` from your dashboard. Same rule about somewhere writable.

## Server and sign in

Discovery is by broadcast, which does not cross a subnet. If your server does
not appear, put `server.txt` beside the app or in its `data` folder, one server
per line:

```
http://192.168.1.10:8096
```

`http://` only. Sign in is Quick Connect, which must be enabled under
Dashboard, General, Quick Connect.

## Building

```sh
git clone --recursive https://github.com/MrMilenko/ButterAndJelly
cd ButterAndJelly
scripts/build.sh
```

[docs/building.md](docs/building.md) for per console requirements,
[docs/architecture.md](docs/architecture.md) for how it fits together.

## License

GPL-2.0-or-later, see [LICENSE](LICENSE) and
[docs/licensing.md](docs/licensing.md). Neither console's SDK is included.
