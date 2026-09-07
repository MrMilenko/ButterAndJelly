// SPDX-License-Identifier: GPL-2.0-or-later

// text_font_ttf.cpp: the SDL_ttf implementation, for platforms that have one.

#include "ui/text_font.h"

#if BJ_TEXT_SDL_TTF

bool TextFont::initLibrary()     { return TTF_Init() == 0; }
void TextFont::shutdownLibrary() { if (TTF_WasInit()) TTF_Quit(); }
const char* TextFont::lastError() { return TTF_GetError(); }

bool TextFont::open(const std::string& path, int pixelHeight)
{
    close();
    font_ = TTF_OpenFont(path.c_str(), pixelHeight);
    return font_ != nullptr;
}

void TextFont::close()
{
    if (font_) { TTF_CloseFont(font_); font_ = nullptr; }
}

bool TextFont::valid() const { return font_ != nullptr; }

int TextFont::lineSkip() const { return font_ ? TTF_FontLineSkip(font_) : 0; }

int TextFont::measure(const std::string& utf8) const
{
    if (!font_ || utf8.empty()) return 0;
    int w = 0, h = 0;
    TTF_SizeUTF8(font_, utf8.c_str(), &w, &h);
    return w;
}

SDL_Surface* TextFont::render(const std::string& utf8, Color c) const
{
    if (!font_ || utf8.empty()) return nullptr;
    SDL_Color sdl;
    sdl.r = c.r; sdl.g = c.g; sdl.b = c.b; sdl.a = c.a;
    return TTF_RenderUTF8_Blended(font_, utf8.c_str(), sdl);
}

#endif  // BJ_TEXT_SDL_TTF
