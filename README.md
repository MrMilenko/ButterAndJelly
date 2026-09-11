# Butter and Jelly

A Jellyfin client for consoles and computers.

> [!WARNING]
> **Beta software.** It runs, it plays, and it will have rough edges. Expect
> bugs, expect things to move around between releases, and expect the Seerr
> side in particular to be unfinished. Nothing here will touch your library,
> but it can and will surprise you.
>
> Found something broken, or want to help? **[Join the Discord](https://discord.gg/fXNqnpaNay)**

**Wii U**

<img src="docs/screenshots/wiiu-shows.png" width="420"> <img src="docs/screenshots/wiiu-episodes.png" width="420">
<img src="docs/screenshots/wiiu-playback.png" width="420"> <img src="docs/screenshots/wiiu-gamepad.png" width="420">

That last one is the GamePad. It mirrors playback, or takes it on its own.

**Xbox 360**

<img src="docs/screenshots/xbox360-home.png" width="420"> <img src="docs/screenshots/xbox360-library.png" width="420">
<img src="docs/screenshots/xbox360-playback.png" width="420">

All captured on actual hardware.

One codebase and one design, whether you're watching cartoons on a MacBook or
trying to watch a movie on a rock. The rock in question is a 733MHz Pentium III
that shipped two years before H.264 was standardized, and it does fine.

`src/core` and `src/ui` are shared. `src/platform` is where the machines stop
agreeing with each other, and `src/core/features.h` is where each one owns up
to what it can't do.

## What works

| | macOS | Wii U | Xbox 360 | Xbox |
| --- | --- | --- | --- | --- |
| Browse, search, resume | yes | yes | yes | yes |
| Quick Connect sign in | yes | yes | yes | yes |
| Sort, filter, favorites | yes | yes | yes | yes |
| Collections and albums | yes | yes | yes | yes |
| Audio, subtitle and version picking | yes | yes | yes | yes |
| Chapters | yes | yes | yes | yes |
| Seerr: discover and request | yes | yes | yes | yes |
| Video | up to 1080p | up to 720p | up to 720p | up to 480p |
| Audio | AAC, MP3 | MP3 | MP3 | MP3 |

## Getting it

One archive per console on the [releases page](../../releases), each with
install notes inside.

**Wii U.** Aroma. Drop `butterandjelly.wuhb` in `sd:/wiiu/apps/`.

**Xbox 360.** A console that runs unsigned code. Copy `butterandjelly/` to the
hard disk and launch `default.xex`. It keeps settings and cached posters next
to itself, so put it somewhere it can write.

**Xbox.** A modded console. Copy `butterandjelly/` to the hard disk and launch
`default.xbe` from your dashboard. Same deal about somewhere writable.

**macOS.** `scripts/package-macos.sh` builds the app bundle, libraries and all.
It's signed ad hoc, so any Mac other than the one that built it is going to
want it signed properly.

## Server and sign in

It finds your server by broadcast, which won't cross a subnet. If yours doesn't
show up, drop a `server.txt` next to the app or in its `data` folder:

```
http://192.168.1.10:8096
```

`http://` only. Sign in is Quick Connect, so turn that on under Dashboard,
General, Quick Connect.

## Seerr

Supported, and a work in progress. The menus were built from the API paths, so
some of them may not do what they say yet.

## Building

```sh
git clone --recursive https://github.com/MrMilenko/ButterAndJelly
cd ButterAndJelly
scripts/build.sh
```

[docs/building.md](docs/building.md) for what each console needs,
[docs/architecture.md](docs/architecture.md) for how the thing fits together.

## License

GPL-2.0-or-later, see [LICENSE](LICENSE) and
[docs/licensing.md](docs/licensing.md). No console SDK is included, and none
of this would work without one you already have.
