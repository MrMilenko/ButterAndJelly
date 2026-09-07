// SPDX-License-Identifier: GPL-2.0-or-later

// main.cpp: window, renderer, and the frame loop.
//
// Identical on both targets. The Wii U's SDL port creates a GX2-backed
// renderer and drives ProcUI itself, so nothing here has to know which
// machine it is on.

#include <SDL.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/platform.h"
#include "core/settings.h"
#include "ui/app.h"
#include "ui/image_load.h"

namespace {

// Maps a --keys name onto an Action. Used only by the scripted-input path.
Action ActionFromName(const std::string& name)
{
    if (name == "up")       return Action::Up;
    if (name == "down")     return Action::Down;
    if (name == "left")     return Action::Left;
    if (name == "right")    return Action::Right;
    if (name == "accept" || name == "a") return Action::Accept;
    if (name == "back"   || name == "b") return Action::Back;
    if (name == "menu")     return Action::Menu;
    if (name == "refresh")  return Action::Refresh;
    if (name == "pageup")   return Action::PageUp;
    if (name == "pagedown") return Action::PageDown;
    return Action::None;
}

std::vector<Action> ParseKeyScript(const std::string& spec)
{
    std::vector<Action> out;
    std::string token;
    for (size_t i = 0; i <= spec.size(); ++i) {
        if (i == spec.size() || spec[i] == ',') {
            if (!token.empty()) {
                const Action a = ActionFromName(token);
                if (a != Action::None) out.push_back(a);
                token.clear();
            }
        } else {
            token += spec[i];
        }
    }
    return out;
}

// The console outputs 720p; everything is laid out in these coordinates and
// SDL scales to whatever the window actually is on a desktop.
constexpr int kLogicalWidth  = 1280;
constexpr int kLogicalHeight = 720;

}  // namespace

// C linkage, which SDL_main.h asks for in as many words: "The application's
// main() function must be called with C linkage." Without it this is
// _Z4mainiPPc, the console's CRT startup does not find it by name, and any
// object that happens to define a plain main, and FFmpeg's softfloat.c has one
// behind an #ifdef TEST this tree does not guard, gets to be the
// program instead. Which is exactly what happened: the title printed
// softfloat's self-test and exited.
extern "C" int main(int argc, char** argv)
{
    // Before anything else, so the debug channel shows the title is alive even
    // if the next line is what kills it. Log::Write works without Log::Init.
    LOGF("[boot] %s %s starting", kAppName, kAppVersion);

    // --shot <path> [seconds]: render for a moment, save one PNG, exit.
    std::string shotPath;
    std::string keyScript;
    int shotDelayMs   = 6000;
    int keyIntervalMs = 1200;

    // The Xbox 360's CRT hands main no command line at all: argv is null and
    // argc is not to be trusted with it. Walking it regardless is a fault on
    // the very first thing this program does, which is a miserable way to
    // find out.
    if (!argv) argc = 0;
    for (int i = 1; i < argc && argv[i]; ++i) {
        const std::string arg = argv[i];
        if (arg == "--shot" && i + 1 < argc) {
            shotPath = argv[++i];
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                shotDelayMs = (int)(atof(argv[++i]) * 1000.0);
            }
        } else if (arg == "--keys" && i + 1 < argc) {
            keyScript = argv[++i];
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                keyIntervalMs = (int)(atof(argv[++i]) * 1000.0);
            }
        }
    }

    Platform::Init();
    Log::Init();
    LOGF("[boot] data=%s assets=%s",
         Platform::DataDir().c_str(), Platform::AssetDir().c_str());
    LOGF("[boot] data directory is %s",
         Platform::FileExists(Platform::DataDir()) ? "there" : "MISSING");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_AUDIO) != 0) {
        LOGF("[boot] SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    if (!Image::Init()) {
        LOGF("[boot] the image backend would not start: %s", Image::LastError());
        SDL_Quit();
        return 1;
    }

    Settings settings;
    settings.load();

#ifdef __WIIU__
    // Which screens to draw to. Drawing to both costs a second scan out per
    // frame, which mattered while the CPU was converting video and does not
    // now that the GPU does it.
    Uint32 windowFlags = SDL_WINDOW_FULLSCREEN;
    switch (settings.display) {
        case Settings::Display::TvOnly:
            windowFlags |= SDL_WINDOW_WIIU_TV_ONLY;
            break;
        case Settings::Display::GamepadOnly:
            windowFlags |= SDL_WINDOW_WIIU_GAMEPAD_ONLY;
            break;
        case Settings::Display::TvAndGamepad:
            break;   // the port's default is both
    }
    LOGF("[boot] display mode: %s",
         settings.display == Settings::Display::TvOnly ? "TV only"
       : settings.display == Settings::Display::GamepadOnly ? "GamePad only"
       : "TV and GamePad");
#elif defined(_XENON)
    // One screen, and it is a television. SDL2x360 caps the mode at 720p
    // whatever the dashboard is outputting, because a 1080i back buffer plus
    // depth does not fit in the console's 10 MB of EDRAM.
    const Uint32 windowFlags = SDL_WINDOW_FULLSCREEN;
#else
    const Uint32 windowFlags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
#endif

    SDL_Window* window = SDL_CreateWindow(
        kAppName,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        kLogicalWidth, kLogicalHeight, windowFlags);
    if (!window) {
        LOGF("[boot] SDL_CreateWindow failed: %s", SDL_GetError());
        Image::Quit();
        SDL_Quit();
        return 1;
    }

    const Uint32 rendererFlags = SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC;
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, rendererFlags);
    if (!renderer) {
        // Software is slow but still usable, and beats refusing to start.
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!renderer) {
        LOGF("[boot] SDL_CreateRenderer failed: %s", SDL_GetError());
        SDL_DestroyWindow(window);
        Image::Quit();
        SDL_Quit();
        return 1;
    }

    // Linear filtering everywhere: artwork is fetched at one size and drawn at
    // another, and point sampling shows it.
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");

    // No logical size on the Xbox 360. The console already outputs 1280x720,
    // which is what everything above is laid out for, so it would be a scale
    // of exactly one, and the interface reads the renderer's output size
    // instead (see App::init). Both were turned off while chasing a texture
    // fault that turned out to be elsewhere; this one stays off because it
    // genuinely buys nothing here.
#if !defined(_XENON)
    SDL_RenderSetLogicalSize(renderer, kLogicalWidth, kLogicalHeight);
#endif

    SDL_RendererInfo info;
    if (SDL_GetRendererInfo(renderer, &info) == 0) {
        int outW = 0, outH = 0;
        SDL_GetRendererOutputSize(renderer, &outW, &outH);
        LOGF("[boot] renderer=%s output=%dx%d formats=%u",
             info.name ? info.name : "?", outW, outH,
             (unsigned)info.num_texture_formats);
        // Whether NV12 appears here decides how decoded video reaches the
        // screen: straight into a texture, or through a colour conversion
        // this CPU would rather not do.
        for (Uint32 i = 0; i < info.num_texture_formats; ++i) {
            LOGF("[boot]   texture format: %s",
                 SDL_GetPixelFormatName(info.texture_formats[i]));
        }
    }
    Platform::LogVideoCapabilities();

    // Open every attached pad. On the console this picks up the GamePad plus
    // any Pro Controllers or Wiimotes that happen to be paired.
    // Controllers already present arrive as SDL_CONTROLLERDEVICEADDED once
    // the event queue is pumped, so the app opens them all in one place.
    LOGF("[input] %d joystick(s) attached at start", SDL_NumJoysticks());

    int exitCode = 0;
    {
        App app;
        app.setSettings(settings);
        if (!shotPath.empty()) app.setAutoScreenshot(shotPath, shotDelayMs);

        if (!app.init(window, renderer)) {
            LOGF("[boot] app init failed");
            exitCode = 1;
        } else {
            if (!keyScript.empty()) {
                app.setScriptedInput(ParseKeyScript(keyScript), keyIntervalMs);
            }
            while (app.frame()) {
                // frame() blocks on vsync via SDL_RenderPresent.
            }
        }
        app.shutdown();
    }

    LOGF("[boot] shutting down");
    Log::Shutdown();

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    Image::Quit();
    SDL_Quit();
    return exitCode;
}
