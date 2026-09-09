Midway Butter and Jelly @VERSION@
A Jellyfin client for the original Xbox.


INSTALL

Needs a modded console, softmod or modchip.

Copy the butterandjelly folder to the hard disk or a mounted share, keeping
its layout:

    butterandjelly/
        default.xbe
        fonts/

Launch default.xbe from your dashboard. It creates a "data" folder beside
itself for settings, sign in and cached posters, so put it somewhere writable.


YOUR SERVER

The app finds Jellyfin by broadcast, which does not cross a subnet. If yours
does not appear, rename server.txt.example to server.txt and leave it beside
default.xbe.

    http://192.168.1.10:8096

http:// only.


SIGNING IN

Quick Connect. The app shows a code; enter it in Jellyfin under your user menu.
Enable it first on the server, under Dashboard, General, Quick Connect.


VIDEO

Capped at 480p, which is the console's own limit. There is no H.264 hardware,
so it decodes in software on the 733 MHz Pentium III and converts to RGB on the
GPU. Ask the server for 640x360 or smaller if a title struggles.


SOURCE

https://github.com/MrMilenko/ButterAndJelly

GPL-2.0-or-later. The binary statically links FFmpeg, which is LGPL; the
corresponding source is the repository above. See docs/licensing.md.
