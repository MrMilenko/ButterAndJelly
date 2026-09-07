// SPDX-License-Identifier: GPL-2.0-or-later

// hls.h: just enough HLS to play a Jellyfin transcode.
//
// Jellyfin serves a master playlist naming one variant, and that variant's
// playlist lists every segment up front for video on demand. There is no
// bitrate switching to do and no live edge to chase, so this parses the two
// playlists and hands back an ordered list of segment URLs.

#pragma once

#include <string>
#include <vector>

struct HlsSegment {
    std::string url;         // absolute
    double      duration = 0.0;
    double      startTime = 0.0;   // seconds from the beginning
};

class HlsPlaylist {
public:
    // Fetches `masterUrl`, follows it to the variant playlist, and parses the
    // segment list. Both requests carry the caller's headers.
    bool load(const std::string& masterUrl, std::string& error);

    const std::vector<HlsSegment>& segments() const { return segments_; }
    double totalDuration() const { return totalDuration_; }
    bool   empty() const { return segments_.empty(); }

    // Index of the segment covering `seconds`, clamped to the list.
    size_t segmentAt(double seconds) const;

    // Exposed for tests: parses playlist text that has already been fetched.
    static std::vector<HlsSegment> ParseVariant(const std::string& text,
                                                const std::string& baseUrl);
    static std::string ParseMaster(const std::string& text,
                                   const std::string& baseUrl);

private:
    std::vector<HlsSegment> segments_;
    double totalDuration_ = 0.0;
};

// Resolves a possibly relative playlist reference against the URL it came
// from, the way a browser would.
std::string ResolveUrl(const std::string& baseUrl, const std::string& reference);
