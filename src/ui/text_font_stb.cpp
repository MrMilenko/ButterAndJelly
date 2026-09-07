// SPDX-License-Identifier: GPL-2.0-or-later

// text_font_stb.cpp: glyphs without a font library.
//
// stb_truetype rasterises a codepoint to an 8-bit coverage bitmap and reports
// the metrics to place it with. Everything above that, laying a string out,
// kerning it and compositing it into a surface SDL can upload, is here. It is
// the same shape as what TTF_RenderUTF8_Blended hands back, so the renderer
// above does not know which implementation it is talking to.

#include "ui/text_font.h"

#include "core/platform.h"

#if !BJ_TEXT_SDL_TTF

#include <cstdio>
#include <cstring>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

namespace {

const char* g_error = "";

// Decodes one UTF-8 sequence, advancing `i`. Malformed input yields U+FFFD and
// consumes one byte, so a bad string still terminates.
uint32_t NextCodepoint(const std::string& s, size_t& i)
{
    const unsigned char lead = (unsigned char)s[i];
    int extra;
    uint32_t cp;

    if      (lead < 0x80) { ++i; return lead; }
    else if ((lead & 0xE0) == 0xC0) { cp = lead & 0x1Fu; extra = 1; }
    else if ((lead & 0xF0) == 0xE0) { cp = lead & 0x0Fu; extra = 2; }
    else if ((lead & 0xF8) == 0xF0) { cp = lead & 0x07u; extra = 3; }
    else { ++i; return 0xFFFD; }

    // The continuation bytes have to be there: i+1 through i+extra.
    if (i + (size_t)extra >= s.size()) { ++i; return 0xFFFD; }
    for (int k = 1; k <= extra; ++k) {
        const unsigned char cont = (unsigned char)s[i + (size_t)k];
        if ((cont & 0xC0) != 0x80) { ++i; return 0xFFFD; }
        cp = (cp << 6) | (cont & 0x3Fu);
    }
    i += (size_t)extra + 1;
    return cp;
}

}  // namespace

bool TextFont::initLibrary()     { return true; }
void TextFont::shutdownLibrary() {}
const char* TextFont::lastError() { return g_error; }

bool TextFont::open(const std::string& path, int pixelHeight)
{
    close();

    const std::string native = Platform::NativePath(path);
    std::FILE* file = std::fopen(native.c_str(), "rb");
    if (!file) { g_error = "could not open the font file"; return false; }

    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (size <= 0) { std::fclose(file); g_error = "the font file is empty"; return false; }

    data_.resize((size_t)size);
    const size_t read = std::fread(data_.data(), 1, (size_t)size, file);
    std::fclose(file);
    if (read != (size_t)size) { data_.clear(); g_error = "short read on the font file"; return false; }

    const int offset = stbtt_GetFontOffsetForIndex(data_.data(), 0);
    if (offset < 0 || !stbtt_InitFont(&info_, data_.data(), offset)) {
        data_.clear();
        g_error = "stb_truetype did not recognize the font";
        return false;
    }

    pixelHeight_ = pixelHeight;
    // ScaleForPixelHeight measures ascent to descent, which is what SDL_ttf's
    // point size lines up with closely enough that the layout above is
    // unchanged between the two implementations.
    scale_ = stbtt_ScaleForPixelHeight(&info_, (float)pixelHeight);
    stbtt_GetFontVMetrics(&info_, &ascent_, &descent_, &lineGap_);
    ready_ = true;
    return true;
}

void TextFont::close()
{
    data_.clear();
    data_.shrink_to_fit();
    ready_ = false;
    scale_ = 0.0f;
}

bool TextFont::valid() const { return ready_; }

int TextFont::lineSkip() const
{
    if (!ready_) return 0;
    return (int)(((float)(ascent_ - descent_ + lineGap_)) * scale_ + 0.5f);
}

int TextFont::measure(const std::string& utf8) const
{
    if (!ready_ || utf8.empty()) return 0;

    float width = 0.0f;
    uint32_t previous = 0;
    for (size_t i = 0; i < utf8.size(); ) {
        const uint32_t cp = NextCodepoint(utf8, i);
        int advance = 0, leftBearing = 0;
        stbtt_GetCodepointHMetrics(&info_, (int)cp, &advance, &leftBearing);
        if (previous) {
            width += (float)stbtt_GetCodepointKernAdvance(&info_, (int)previous, (int)cp) * scale_;
        }
        width += (float)advance * scale_;
        previous = cp;
    }
    return (int)(width + 0.5f);
}

SDL_Surface* TextFont::render(const std::string& utf8, Color c) const
{
    if (!ready_ || utf8.empty()) return nullptr;

    const int height = lineSkip();
    const int width  = measure(utf8);
    if (width <= 0 || height <= 0) return nullptr;

    // A pixel of the requested colour everywhere, transparent to start; the
    // glyphs only ever raise the alpha. Same result as a blended TTF render,
    // and it means scaling and blending behave identically.
    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(
        0, width, height, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!surface) { g_error = SDL_GetError(); return nullptr; }

    const Uint32 clear = ((Uint32)0 << 24) | ((Uint32)c.r << 16) |
                         ((Uint32)c.g << 8) | (Uint32)c.b;
    SDL_FillRect(surface, nullptr, clear);

    const int baseline = (int)((float)ascent_ * scale_ + 0.5f);

    float penX = 0.0f;
    uint32_t previous = 0;
    for (size_t i = 0; i < utf8.size(); ) {
        const uint32_t cp = NextCodepoint(utf8, i);

        if (previous) {
            penX += (float)stbtt_GetCodepointKernAdvance(&info_, (int)previous, (int)cp) * scale_;
        }

        int gw = 0, gh = 0, xoff = 0, yoff = 0;
        // Subpixel positioning off: the pen lands on whole pixels because the
        // renderer above draws at integer coordinates anyway.
        unsigned char* coverage = stbtt_GetCodepointBitmap(
            &info_, scale_, scale_, (int)cp, &gw, &gh, &xoff, &yoff);

        if (coverage) {
            const int originX = (int)(penX + 0.5f) + xoff;
            const int originY = baseline + yoff;

            for (int gy = 0; gy < gh; ++gy) {
                const int py = originY + gy;
                if (py < 0 || py >= height) continue;
                Uint32* row = (Uint32*)((Uint8*)surface->pixels + (size_t)py * surface->pitch);
                for (int gx = 0; gx < gw; ++gx) {
                    const int px = originX + gx;
                    if (px < 0 || px >= width) continue;
                    const unsigned int a = coverage[gy * gw + gx];
                    if (!a) continue;
                    // Glyphs can overlap where one is kerned into another, so
                    // keep the greater coverage rather than adding and wrapping.
                    const Uint32 existing = (row[px] >> 24) & 0xFFu;
                    const Uint32 alpha = (a * (unsigned int)c.a) / 255u;
                    if (alpha > existing) {
                        row[px] = (alpha << 24) | (row[px] & 0x00FFFFFFu);
                    }
                }
            }
            stbtt_FreeBitmap(coverage, nullptr);
        }

        int advance = 0, leftBearing = 0;
        stbtt_GetCodepointHMetrics(&info_, (int)cp, &advance, &leftBearing);
        penX += (float)advance * scale_;
        previous = cp;
    }

    return surface;
}

#endif  // !BJ_TEXT_SDL_TTF
