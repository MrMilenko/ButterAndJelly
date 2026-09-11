// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/input_hint.h"

namespace {

// Kenney's Input Prompts, one font per device family, each mapping its glyphs
// into the private use area from U+E000. The codepoints below come from the
// _map.txt shipped beside each font.
struct Glyphs { unsigned a, b, x, y, s, l, r; };

constexpr Glyphs kGlyphs[] = {
    // Keyboard: enter, escape, X, R, tab, page up, page down
    { 0xE05E, 0xE062, 0xE0E3, 0xE0B9, 0xE0D1, 0xE0A7, 0xE0A5 },
    // Xbox: A, B, X, Y, menu, LB, RB
    { 0xE004, 0xE006, 0xE01E, 0xE020, 0xE014, 0xE043, 0xE049 },
    // PlayStation: cross, circle, square, triangle, options, L1, R1
    { 0xE04B, 0xE041, 0xE051, 0xE053, 0xE009, 0xE078, 0xE080 },
    // Wii U: A, B, Y, X, plus, L, R
    { 0xE005, 0xE007, 0xE019, 0xE017, 0xE00F, 0xE00B, 0xE013 },
};

constexpr const char* kFonts[] = {
    "input_keyboard.ttf", "input_xbox.ttf",
    "input_playstation.ttf", "input_nintendo.ttf",
};

// Words for when the glyph font is missing, so a hint never reads as a box.
struct Names { const char* a; const char* b; const char* x; const char* y;
               const char* s; const char* l; const char* r; };

constexpr Names kNames[] = {
    { "Enter", "Esc",    "X",      "R",        "Tab",     "PgUp", "PgDn" },
    { "A",     "B",      "X",      "Y",        "Start",   "LB",   "RB"   },
    { "Cross", "Circle", "Square", "Triangle", "Options", "L1",   "R1"   },
    { "A",     "B",      "Y",      "X",        "+",       "L",    "R"    },
};

std::string Utf8(unsigned cp)
{
    std::string out;
    if (cp < 0x80) { out += (char)cp; return out; }
    if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
        return out;
    }
    out += (char)(0xE0 | (cp >> 12));
    out += (char)(0x80 | ((cp >> 6) & 0x3F));
    out += (char)(0x80 | (cp & 0x3F));
    return out;
}

int IndexFor(char token, const Glyphs& g, unsigned* cp)
{
    switch (token) {
        case 'A': *cp = g.a; return 0;
        case 'B': *cp = g.b; return 1;
        case 'X': *cp = g.x; return 2;
        case 'Y': *cp = g.y; return 3;
        case 'S': *cp = g.s; return 4;
        case 'L': *cp = g.l; return 5;
        case 'R': *cp = g.r; return 6;
        default:  return -1;
    }
}

}  // namespace

const char* HintFontFile(InputScheme scheme)
{
    return kFonts[(int)scheme];
}

std::string HintGlyph(InputScheme scheme, char token)
{
    unsigned cp = 0;
    if (IndexFor(token, kGlyphs[(int)scheme], &cp) < 0) return "";
    return Utf8(cp);
}

std::string HintWord(InputScheme scheme, char token)
{
    const Names& n = kNames[(int)scheme];
    switch (token) {
        case 'A': return n.a;
        case 'B': return n.b;
        case 'X': return n.x;
        case 'Y': return n.y;
        case 'S': return n.s;
        case 'L': return n.l;
        case 'R': return n.r;
        default:  return "";
    }
}

std::string ExpandHints(const std::string& text, InputScheme scheme)
{
    std::string out;
    out.reserve(text.size() + 16);
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '$' || i + 1 >= text.size()) { out += text[i]; continue; }
        const std::string word = HintWord(scheme, text[++i]);
        if (word.empty()) { out += '$'; out += text[i]; } else { out += word; }
    }
    return out;
}

InputScheme SchemeForController(SDL_GameController* pad)
{
    if (!pad) return InputScheme::Keyboard;

    switch (SDL_GameControllerGetType(pad)) {
        case SDL_CONTROLLER_TYPE_PS3:
        case SDL_CONTROLLER_TYPE_PS4:
        case SDL_CONTROLLER_TYPE_PS5:
            return InputScheme::PlayStation;
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO:
            return InputScheme::Nintendo;
        default:
            return InputScheme::Xbox;
    }
}
