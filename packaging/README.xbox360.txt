Xenon Butter and Jelly @VERSION@
A Jellyfin client for the Xbox 360.


INSTALL

Needs a console that runs unsigned code.

Copy the butterandjelly folder to the hard disk, keeping its layout:

    butterandjelly/
        default.xex
        fonts/

Launch default.xex. It creates a "data" folder beside itself for settings,
sign in and cached posters, so put it somewhere writable.


YOUR SERVER

The app finds Jellyfin by broadcast, which does not cross a subnet. If yours
does not appear, rename server.txt.example to server.txt and leave it beside
default.xex.

    http://192.168.1.10:8096

http:// only.


SEERR

The second tab at the bottom left is Seerr, which is where you ask for things
the library does not have. If Seerr runs on the same machine as Jellyfin and
shares its accounts, the app finds it and signs in on its own.

Otherwise, set it up under Settings, or rename seerr.txt.example to seerr.txt
and put the address and an API key in it, one per line. Anyone holding that
key can make requests as you, so treat the file as a password. Without any of
this the tab says nothing is set up, and everything else works as before.


SIGNING IN

Quick Connect. The app shows a code; enter it in Jellyfin under your user menu.
Enable it first on the server, under Dashboard, General, Quick Connect.


VIDEO

Capped at 720p. The 360 has no H.264 hardware, so it decodes in software.


SOURCE

https://github.com/MrMilenko/ButterAndJelly

GPL-2.0-or-later. The binary statically links FFmpeg, which is LGPL; the
corresponding source is the repository above. See docs/licensing.md.
