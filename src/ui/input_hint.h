// SPDX-License-Identifier: GPL-2.0-or-later

// Button names for the hint bar, which differ by what the viewer is holding.
//
// Hint strings carry placeholders rather than letters, because "A" means
// nothing on a PlayStation pad and less than nothing on a keyboard.

#pragma once

#include <string>

#include <SDL.h>

enum class InputScheme { Keyboard, Xbox, PlayStation, Nintendo };

// $A accept, $B back, $Y refresh, $S start, $L and $R the shoulders.

// The glyph font for a scheme, relative to the font directory.
const char* HintFontFile(InputScheme scheme);

// One glyph as UTF-8, empty if the token is not one of the six.
std::string HintGlyph(InputScheme scheme, char token);

// The same button as a word, for when the glyph font did not load.
std::string HintWord(InputScheme scheme, char token);

// Every placeholder replaced by its word. Used where glyphs cannot be drawn.
std::string ExpandHints(const std::string& text, InputScheme scheme);

// What a pad calls its buttons. Falls back to Xbox, whose names SDL uses.
InputScheme SchemeForController(SDL_GameController* pad);
