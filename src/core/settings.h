// SPDX-License-Identifier: GPL-2.0-or-later

// settings.h: the handful of choices that outlive a session.
//
// Kept separate from the Jellyfin session, which holds credentials and is
// discarded on sign out. These survive signing out and switching servers.

#pragma once

#include <string>

struct Settings {
    // Which screens the app draws to. The Wii U scans the same frame out to
    // the television and the GamePad, and doing both costs a second copy and
    // swap per frame. That mattered when the CPU was converting video; the
    // GPU does it now, so both is the sensible default and TV only remains
    // available if it ever bites again.
    enum class Display { TvAndGamepad, TvOnly, GamepadOnly };

    Display display = Display::TvAndGamepad;

    // Ceiling for the height requested from the server. Each console clamps
    // this to what it can decode.
    int playbackHeight = 720;

    void load();
    void save() const;
};
