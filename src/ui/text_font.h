// SPDX-License-Identifier: GPL-2.0-or-later

// One TrueType face at one pixel size, and the strings it draws.
//
// SDL_ttf where a build exists, which is the Wii U and the desktop. The Xbox
// 360 has neither SDL_ttf nor FreeType and rasterises with stb_truetype.

#pragma once

#include <string>
#include <vector>

#include <SDL.h>

#include "ui/palette.h"

#if defined(_XENON)
  #define BJ_TEXT_SDL_TTF 0
#else
  #define BJ_TEXT_SDL_TTF 1
#endif

#if BJ_TEXT_SDL_TTF
  // Included rather than forward declared: SDL_ttf has spelled the TTF_Font
  // tag differently across versions, so any local declaration breaks on one
  // of them.
  #include <SDL_ttf.h>
#else
  #include "stb_truetype.h"
#endif

class TextFont {
public:
    TextFont() = default;
    ~TextFont() { close(); }
    TextFont(const TextFont&) = delete;
    TextFont& operator=(const TextFont&) = delete;

    // SDL_ttf needs starting and stopping; stb_truetype does not, and says so
    // by doing nothing here.
    static bool initLibrary();
    static void shutdownLibrary();
    static const char* lastError();

    bool open(const std::string& path, int pixelHeight);
    void close();
    bool valid() const;

    // Baseline to baseline, which is what a wrapped paragraph steps by.
    int lineSkip() const;

    // Width in pixels of the string as it would be drawn.
    int measure(const std::string& utf8) const;

    // An ARGB surface the caller owns: `c`'s colour throughout, alpha from
    // the glyph coverage times `c`'s own alpha. The same thing
    // TTF_RenderUTF8_Blended returns.
    SDL_Surface* render(const std::string& utf8, Color c) const;

private:
#if BJ_TEXT_SDL_TTF
    TTF_Font* font_ = nullptr;
#else
    std::vector<unsigned char> data_;   // the face, held for stb's lifetime
    stbtt_fontinfo info_{};
    float scale_ = 0.0f;
    int   pixelHeight_ = 0;
    int   ascent_ = 0, descent_ = 0, lineGap_ = 0;
    bool  ready_ = false;
#endif
};
