WiiU Butter and Jelly @VERSION@
A Jellyfin client for the Wii U.


INSTALL

Needs Aroma. Copy butterandjelly.wuhb to sd:/wiiu/apps/ and launch it from the
Wii U Menu.


YOUR SERVER

The app finds Jellyfin by broadcast, which does not cross a subnet. If yours
does not appear, rename server.txt.example to server.txt and put it in
sd:/wiiu/butterjelly/, which the app creates on first run.

    http://192.168.1.10:8096

http:// only.


SIGNING IN

Quick Connect. The app shows a code; enter it in Jellyfin under your user menu.
Enable it first on the server, under Dashboard, General, Quick Connect.


VIDEO

Capped at 480p, which is what the console holds at full frame rate. Plays on
the TV, the GamePad, or both; see Settings.


SOURCE

https://github.com/MrMilenko/ButterAndJelly

GPL-2.0-or-later. Third party code keeps its own license, see docs/licensing.md.
