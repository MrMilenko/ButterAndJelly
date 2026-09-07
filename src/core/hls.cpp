// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/hls.h"

#include "core/http.h"
#include "core/log.h"
#include "core/text.h"

#include <cstdlib>

namespace {

// Splits playlist text into lines, tolerating both line ending conventions.
std::vector<std::string> SplitLines(const std::string& text)
{
    std::vector<std::string> lines;
    std::string current;
    for (char ch : text) {
        if (ch == '\n') {
            if (!current.empty() && current.back() == '\r') current.pop_back();
            lines.push_back(current);
            current.clear();
        } else {
            current += ch;
        }
    }
    if (!current.empty()) lines.push_back(current);
    return lines;
}

}  // namespace

std::string ResolveUrl(const std::string& baseUrl, const std::string& reference)
{
    if (reference.empty()) return "";
    if (reference.find("://") != std::string::npos) return reference;

    // Absolute path: keep scheme and host, replace everything after.
    if (reference[0] == '/') {
        const size_t schemeEnd = baseUrl.find("://");
        if (schemeEnd == std::string::npos) return reference;
        const size_t hostEnd = baseUrl.find('/', schemeEnd + 3);
        const std::string origin = (hostEnd == std::string::npos)
                                   ? baseUrl : baseUrl.substr(0, hostEnd);
        return origin + reference;
    }

    // Relative: drop the last path element, and any query string with it.
    std::string base = baseUrl;
    const size_t query = base.find('?');
    if (query != std::string::npos) base = base.substr(0, query);
    const size_t lastSlash = base.rfind('/');
    if (lastSlash == std::string::npos) return reference;
    return base.substr(0, lastSlash + 1) + reference;
}

std::string HlsPlaylist::ParseMaster(const std::string& text,
                                     const std::string& baseUrl)
{
    // The first non-comment line after a STREAM-INF is the variant playlist.
    // Jellyfin only ever offers one, so there is nothing to choose between.
    const std::vector<std::string> lines = SplitLines(text);
    for (size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].rfind("#EXT-X-STREAM-INF", 0) != 0) continue;
        for (size_t j = i + 1; j < lines.size(); ++j) {
            if (lines[j].empty() || lines[j][0] == '#') continue;
            return ResolveUrl(baseUrl, lines[j]);
        }
    }
    return "";
}

std::vector<HlsSegment> HlsPlaylist::ParseVariant(const std::string& text,
                                                  const std::string& baseUrl)
{
    std::vector<HlsSegment> segments;
    double pendingDuration = 0.0;
    double elapsed = 0.0;

    for (const std::string& line : SplitLines(text)) {
        if (line.rfind("#EXTINF:", 0) == 0) {
            pendingDuration = std::atof(line.c_str() + 8);
            continue;
        }
        if (line.empty() || line[0] == '#') continue;

        HlsSegment segment;
        segment.url       = ResolveUrl(baseUrl, line);
        segment.duration  = pendingDuration;
        segment.startTime = elapsed;
        if (!segment.url.empty()) {
            elapsed += pendingDuration;
            segments.push_back(segment);
        }
        pendingDuration = 0.0;
    }
    return segments;
}

bool HlsPlaylist::load(const std::string& masterUrl, std::string& error)
{
    segments_.clear();
    totalDuration_ = 0.0;

    HttpResponse master = Http::Get(masterUrl);
    if (!master.ok()) {
        error = master.error.empty() ? "the server refused the playlist" : master.error;
        return false;
    }

    std::string variantUrl = ParseMaster(master.body, masterUrl);
    std::string variantText;

    if (variantUrl.empty()) {
        // Some responses are already the variant playlist.
        if (master.body.find("#EXTINF") == std::string::npos) {
            error = "the playlist named no stream";
            return false;
        }
        variantUrl  = masterUrl;
        variantText = master.body;
    } else {
        HttpResponse variant = Http::Get(variantUrl);
        if (!variant.ok()) {
            error = variant.error.empty() ? "could not load the stream" : variant.error;
            return false;
        }
        variantText = variant.body;
    }

    segments_ = ParseVariant(variantText, variantUrl);
    if (segments_.empty()) { error = "the stream listed no segments"; return false; }

    for (const HlsSegment& segment : segments_) totalDuration_ += segment.duration;

    LOGF("[hls] %lu segments, %s seconds", (unsigned long)segments_.size(),
         bj::ToString(totalDuration_, 0).c_str());
    return true;
}

size_t HlsPlaylist::segmentAt(double seconds) const
{
    if (segments_.empty()) return 0;
    for (size_t i = 0; i < segments_.size(); ++i) {
        if (seconds < segments_[i].startTime + segments_[i].duration) return i;
    }
    return segments_.size() - 1;
}
