// SPDX-License-Identifier: GPL-2.0-or-later

// settings.h: choices that outlive a session.
//
// Separate from the Jellyfin session, which holds credentials and is dropped
// on sign out.

#pragma once

#include <string>

struct Settings {
    // Which screens the app draws to. Only the Wii U has more than one.
    enum class Display { TvAndGamepad, TvOnly, GamepadOnly };

    Display display = Display::TvAndGamepad;

    // How playback is asked for. 0 means direct: the server sends what it
    // has and nothing is re-encoded. Any other value is a height to transcode
    // down to. Each console clamps this to what it can decode, and one that
    // cannot decode the source has no direct option at all.
    int playbackHeight = 720;

    // kbit/s, or 0 to let the height decide.
    int videoBitrate = 0;

    // Frames per second ceiling, or 0 for whatever the source runs at.
    int maxFramerate = 0;

    // Automatic is whatever this build decodes best.
    enum class AudioFormat { Automatic, Aac, Mp3 };
    AudioFormat audioFormat = AudioFormat::Automatic;

    // Audio bitrate in kbit/s, for the transcoded case.
    int audioBitrate = 192;

    // Keeps the per frame timing out of the log unless it is wanted.
    bool diagnostics = false;

    // Stops the client looking for Seerr beside Jellyfin.
    bool requestServerElsewhere = false;

    // Where the window was last left. Zero until one is stored.
    int windowWidth  = 0;
    int windowHeight = 0;
    bool fullscreen  = false;


    void load();
    void save() const;
};
