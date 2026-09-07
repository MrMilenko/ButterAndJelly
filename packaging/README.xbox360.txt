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


SIGNING IN

Quick Connect. The app shows a code; enter it in Jellyfin under your user menu.
Enable it first on the server, under Dashboard, General, Quick Connect.


VIDEO

Capped at 720p. The 360 has no H.264 hardware, so it decodes in software.


SOURCE

https://github.com/MrMilenko/ButterAndJelly

GPL-2.0-or-later. The binary statically links FFmpeg, which is LGPL; the
corresponding source is the repository above. See docs/licensing.md.
