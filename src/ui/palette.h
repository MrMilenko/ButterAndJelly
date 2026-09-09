// SPDX-License-Identifier: GPL-2.0-or-later

// palette.h: the colours, kept apart from render.h.
//
// Separate because render.h pulls in SDL_ttf, and things that only want to
// know what colour something is should not have to have a font library. The
// Xbox 360 skeleton is the first caller that could not include render.h.

#pragma once

#include <SDL.h>

struct Color {
    Uint8 r, g, b, a;
    constexpr Color(Uint8 red, Uint8 green, Uint8 blue, Uint8 alpha = 255)
        : r(red), g(green), b(blue), a(alpha) {}
};

// A restrained palette: on a TV, across a room, contrast beats decoration.
namespace Palette {
constexpr Color Background { 0x0E, 0x0F, 0x14 };
constexpr Color Panel      { 0x18, 0x1A, 0x22 };
constexpr Color PanelHi    { 0x22, 0x25, 0x30 };
constexpr Color Text       { 0xF2, 0xF3, 0xF7 };
constexpr Color TextDim    { 0x93, 0x98, 0xA8 };
constexpr Color Accent     { 0x8B, 0x5C, 0xF6 };   // Jellyfin's purple
constexpr Color AccentWarm { 0x00, 0xA4, 0xDC };   // and its blue

// The console's own colour, used for its name in the title.
#if defined(__WIIU__)
constexpr Color Platform   { 0x00, 0xA4, 0xDC };   // Nintendo blue
#elif defined(_XENON)
constexpr Color Platform   { 0x52, 0xB0, 0x43 };   // Xbox 360 green
#elif defined(_XBOX)
constexpr Color Platform   { 0x9B, 0xC8, 0x00 };   // original Xbox green
#else
constexpr Color Platform   { 0x93, 0x98, 0xA8 };
#endif
constexpr Color Danger     { 0xE5, 0x5B, 0x5B };
constexpr Color Shadow     { 0x00, 0x00, 0x00, 0x99 };
}  // namespace Palette
