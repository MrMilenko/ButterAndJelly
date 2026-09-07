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

    LOGF("[settings] display=%s, playback up to %dp",
         DisplayName(display), playbackHeight);
}

void Settings::save() const
{
    FILE* fp = std::fopen(Platform::NativePath(SettingsPath()).c_str(), "wb");
    if (!fp) return;
    std::fprintf(fp,
        "{\n  \"display\": \"%s\",\n  \"playbackHeight\": %d\n}\n",
        DisplayName(display), playbackHeight);
    std::fclose(fp);
}
