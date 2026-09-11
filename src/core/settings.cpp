// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/settings.h"

#include "core/json.h"
#include "core/log.h"
#include "core/platform.h"

#include <cstdio>

namespace {

std::string SettingsPath()
{
    return Platform::DataDir() + "/settings.json";
}

const char* DisplayName(Settings::Display display)
{
    switch (display) {
        case Settings::Display::TvOnly:      return "tv";
        case Settings::Display::GamepadOnly: return "gamepad";
        case Settings::Display::TvAndGamepad:
        default:                             return "both";
    }
}

}  // namespace

void Settings::load()
{
    FILE* fp = std::fopen(Platform::NativePath(SettingsPath()).c_str(), "rb");
    if (!fp) return;

    std::string text;
    char buffer[1024];
    size_t read;
    while ((read = std::fread(buffer, 1, sizeof(buffer), fp)) > 0) {
        text.append(buffer, read);
    }
    std::fclose(fp);

    JsonDoc doc(text);
    if (!doc.valid()) return;

    const std::string mode = doc["display"].asString("both");
    if      (mode == "tv")      display = Display::TvOnly;
    else if (mode == "gamepad") display = Display::GamepadOnly;
    else                        display = Display::TvAndGamepad;

    const int height = doc["playbackHeight"].asInt(720);
    if (height >= 240 && height <= 1080) playbackHeight = height;

    const int w = doc["windowWidth"].asInt(0);
    const int h = doc["windowHeight"].asInt(0);
    if (w >= 640 && h >= 480) { windowWidth = w; windowHeight = h; }
    fullscreen = doc["fullscreen"].asInt(0) != 0;
    requestServerElsewhere = doc["requestServerElsewhere"].asInt(0) != 0;
    diagnostics = doc["diagnostics"].asInt(0) != 0;

    const int vbr = doc["videoBitrate"].asInt(0);
    if (vbr >= 0 && vbr <= 40000) videoBitrate = vbr;
    const int fps = doc["maxFramerate"].asInt(0);
    if (fps >= 0 && fps <= 120) maxFramerate = fps;
    const int abr = doc["audioBitrate"].asInt(192);
    if (abr >= 64 && abr <= 512) audioBitrate = abr;

    const std::string format = doc["audioFormat"].asString("auto");
    audioFormat = (format == "aac") ? AudioFormat::Aac
                : (format == "mp3") ? AudioFormat::Mp3
                                    : AudioFormat::Automatic;

    LOGF("[settings] display=%s, playback up to %dp",
         DisplayName(display), playbackHeight);
}

void Settings::save() const
{
    FILE* fp = std::fopen(Platform::NativePath(SettingsPath()).c_str(), "wb");
    if (!fp) return;
    std::fprintf(fp,
        "{\n  \"display\": \"%s\",\n  \"playbackHeight\": %d,\n"
        "  \"windowWidth\": %d,\n  \"windowHeight\": %d,\n"
        "  \"fullscreen\": %d,\n  \"requestServerElsewhere\": %d,\n"
        "  \"videoBitrate\": %d,\n  \"maxFramerate\": %d,\n"
        "  \"audioFormat\": \"%s\",\n  \"audioBitrate\": %d,\n"
        "  \"diagnostics\": %d\n}\n",
        DisplayName(display), playbackHeight,
        windowWidth, windowHeight, fullscreen ? 1 : 0,
        requestServerElsewhere ? 1 : 0,
        videoBitrate, maxFramerate,
        audioFormat == AudioFormat::Aac ? "aac"
      : audioFormat == AudioFormat::Mp3 ? "mp3" : "auto",
        audioBitrate, diagnostics ? 1 : 0);
    std::fclose(fp);
}
