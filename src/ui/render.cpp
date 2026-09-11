// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/render.h"

#include "core/log.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

int PixelSizeFor(FontSize size)
{
    // Sized for a 1280x720 TV signal viewed from a couch, not a monitor.
    switch (size) {
        case FontSize::Small:   return 18;
        case FontSize::Body:    return 22;
        case FontSize::Title:   return 30;
        case FontSize::Huge:    return 44;
        case FontSize::Display: return 96;
    }
    return 22;
}

Uint32 PackColor(Color c)
{
    return ((Uint32)c.r << 24) | ((Uint32)c.g << 16) |
           ((Uint32)c.b << 8)  | (Uint32)c.a;
}

int NextPowerOfTwo(int value)
{
    int result = 1;
    while (result < value) result <<= 1;
    return result;
}

// A texture holding what the surface holds.
//
// SDL_CreateTextureFromSurface everywhere but the Xbox 360, which uses a
// streaming texture: static textures there all sample one texture's pixels,
// while the streaming path is the one that console is known to run well.
SDL_Texture* TextureFromSurface(SDL_Renderer* renderer, SDL_Surface* surface)
{
#if !defined(_XENON)
    return SDL_CreateTextureFromSurface(renderer, surface);
#else
    // Both text backends already produce ARGB8888, which is also the only
    // format this renderer advertises; convert anyway rather than assume.
    SDL_Surface* source = surface;
    SDL_Surface* converted = nullptr;
    if (surface->format->format != SDL_PIXELFORMAT_ARGB8888) {
        converted = SDL_ConvertSurfaceFormat(surface, SDL_PIXELFORMAT_ARGB8888, 0);
        if (!converted) return nullptr;
        source = converted;
    }

    // Power of two in both directions, or the texture samples another one's
    // pixels. Drawn from the top left with a source rect, so the padding
    // costs only memory.
    const int texW = NextPowerOfTwo(source->w);
    const int texH = NextPowerOfTwo(source->h);

    SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                             SDL_TEXTUREACCESS_STREAMING,
                                             texW, texH);
    if (texture) {
        void* pixels = nullptr;
        int   pitch  = 0;
        if (SDL_LockTexture(texture, nullptr, &pixels, &pitch) == 0) {
            // Cleared first: the padding is sampled at the edges by a linear
            // filter, and whatever the allocator left there is not black.
            SDL_memset(pixels, 0, (size_t)pitch * texH);
            const int rowBytes = source->w * 4;
            for (int y = 0; y < source->h; ++y) {
                SDL_memcpy((Uint8*)pixels + (size_t)y * pitch,
                           (const Uint8*)source->pixels + (size_t)y * source->pitch,
                           (size_t)rowBytes);
            }
            SDL_UnlockTexture(texture);
            SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
        } else {
            SDL_DestroyTexture(texture);
            texture = nullptr;
        }
    }

    if (converted) SDL_FreeSurface(converted);
    return texture;
#endif
}

}  // namespace

Renderer::~Renderer()
{
    shutdown();
}

bool Renderer::init(SDL_Renderer* sdl, const std::string& fontDir)
{
    sdl_ = sdl;

    if (!TextFont::initLibrary()) {
        LOGF("[ui] the text backend would not start: %s", TextFont::lastError());
        return false;
    }

    const std::string regular = fontDir + "/NotoSans-Regular.ttf";
    const std::string bold    = fontDir + "/NotoSans-Bold.ttf";

    const FontSize sizes[] = { FontSize::Small, FontSize::Body, FontSize::Title,
                               FontSize::Huge,  FontSize::Display };
    for (FontSize s : sizes) {
        // Headings carry more weight; body text stays regular.
        const bool heavy = (s == FontSize::Title || s == FontSize::Huge ||
                            s == FontSize::Display);
        const std::string& path = heavy ? bold : regular;
        if (!fonts_[(int)s].open(path, PixelSizeFor(s))) {
            LOGF("[ui] could not open %s: %s", path.c_str(), TextFont::lastError());
            return false;
        }
    }

    return true;
}

void Renderer::shutdown()
{
    for (auto& entry : textCache_) {
        if (entry.second.texture) SDL_DestroyTexture(entry.second.texture);
    }
    textCache_.clear();

    // Every face has to go before the library does, or closing one afterwards
    // walks into a freed FreeType.
    for (TextFont& f : fonts_) f.close();
    iconFont_.close();
    iconFontPath_.clear();
    TextFont::shutdownLibrary();
    sdl_ = nullptr;
}

const TextFont* Renderer::fontFor(FontSize size) const
{
    return &fonts_[(int)size];
}

namespace {
// Icons share the text cache. The slot sits past every FontSize so a glyph
// and a string cannot key to the same entry.
constexpr int kIconCacheSlot = 100;
}  // namespace

// ------------------------------------------------------------- primitives

void Renderer::clear(Color c)
{
    SDL_SetRenderDrawColor(sdl_, c.r, c.g, c.b, c.a);
    SDL_RenderClear(sdl_);
}

void Renderer::fillRect(const SDL_Rect& r, Color c)
{
    SDL_SetRenderDrawBlendMode(sdl_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(sdl_, c.r, c.g, c.b, c.a);
    SDL_RenderFillRect(sdl_, &r);
}

void Renderer::strokeRect(const SDL_Rect& r, Color c, int thickness)
{
    SDL_SetRenderDrawBlendMode(sdl_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(sdl_, c.r, c.g, c.b, c.a);
    for (int i = 0; i < thickness; ++i) {
        SDL_Rect ring = { r.x - i, r.y - i, r.w + 2 * i, r.h + 2 * i };
        SDL_RenderDrawRect(sdl_, &ring);
    }
}

void Renderer::fillRoundedRect(const SDL_Rect& r, int radius, Color c)
{
    if (radius <= 0) { fillRect(r, c); return; }
    radius = std::min(radius, std::min(r.w, r.h) / 2);

    SDL_SetRenderDrawBlendMode(sdl_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(sdl_, c.r, c.g, c.b, c.a);

    // Middle band plus the two side bands, then the corners as scanlines.
    SDL_Rect middle = { r.x, r.y + radius, r.w, r.h - 2 * radius };
    SDL_RenderFillRect(sdl_, &middle);

    for (int dy = 0; dy < radius; ++dy) {
        const int y  = radius - dy;
        const int dx = (int)(SDL_sqrt((double)(radius * radius - y * y)) + 0.5);
        const int inset = radius - dx;

        SDL_Rect top = { r.x + inset, r.y + dy, r.w - 2 * inset, 1 };
        SDL_RenderFillRect(sdl_, &top);

        SDL_Rect bottom = { r.x + inset, r.y + r.h - dy - 1, r.w - 2 * inset, 1 };
        SDL_RenderFillRect(sdl_, &bottom);
    }
}

void Renderer::drawTextureCover(SDL_Texture* tex, const SDL_Rect& dst)
{
    if (!tex) return;

    // A texture can be larger than the image in it, since the Xbox 360 build
    // pads to a power of two, and the real size is left in the user data by
    // whoever loaded it. Falling back to the texture's own size is right
    // everywhere else.
    int tw = 0, th = 0;
    if (const void* packed = SDL_GetTextureUserData(tex)) {
        const uintptr_t bits = (uintptr_t)packed;
        tw = (int)(bits >> 16) & 0xFFFF;
        th = (int)(bits & 0xFFFF);
    }
    if (tw <= 0 || th <= 0) {
        SDL_QueryTexture(tex, nullptr, nullptr, &tw, &th);
    }
    if (tw <= 0 || th <= 0) return;

    // Pick the source rect that fills dst without distorting the poster.
    const double dstAspect = (double)dst.w / (double)dst.h;
    const double srcAspect = (double)tw / (double)th;

    SDL_Rect src = { 0, 0, tw, th };
    if (srcAspect > dstAspect) {
        src.w = (int)(th * dstAspect + 0.5);
        src.x = (tw - src.w) / 2;
    } else {
        src.h = (int)(tw / dstAspect + 0.5);
        src.y = (th - src.h) / 2;
    }
    SDL_RenderCopy(sdl_, tex, &src, &dst);
}

// -------------------------------------------------------------------- text

Renderer::CachedText* Renderer::acquire(const std::string& text,
                                        FontSize size, Color c)
{
    if (text.empty()) return nullptr;

    const CacheKey key{ text, (int)size, PackColor(c) };
    auto it = textCache_.find(key);
    if (it != textCache_.end()) {
        it->second.lastUsedFrame = frame_;
        return &it->second;
    }

    const TextFont* font = fontFor(size);
    if (!font || !font->valid()) return nullptr;

    SDL_Surface* surf = font->render(text, c);
    if (!surf) return nullptr;

    CachedText entry;
    entry.w = surf->w;
    entry.h = surf->h;
    entry.lastUsedFrame = frame_;
    entry.texture = TextureFromSurface(sdl_, surf);
    SDL_FreeSurface(surf);

    if (!entry.texture) return nullptr;

    auto inserted = textCache_.emplace(key, entry);
    return &inserted.first->second;
}

void Renderer::drawText(const std::string& text, int x, int y,
                        FontSize size, Color c, Align align)
{
    CachedText* entry = acquire(text, size, c);
    if (!entry) return;

    int drawX = x;
    if      (align == Align::Center) drawX = x - entry->w / 2;
    else if (align == Align::Right)  drawX = x - entry->w;

    SDL_Rect dst = { drawX, y, entry->w, entry->h };
    // The texture may be larger than the text: see TextureFromSurface.
    const SDL_Rect src = { 0, 0, entry->w, entry->h };
    SDL_RenderCopy(sdl_, entry->texture, &src, &dst);
}

bool Renderer::setIconFont(const std::string& path)
{
    if (path == iconFontPath_) return iconFont_.valid();

    iconFont_.close();
    iconFontPath_ = path;
    if (path.empty()) return false;

    // Kenney's glyphs sit inside a square em box, so at the text's own pixel
    // size they come out visibly smaller than the words beside them.
    if (!iconFont_.open(path, (PixelSizeFor(FontSize::Small) * 2))) {
        LOGF("[ui] no input glyphs from %s: %s", path.c_str(),
             TextFont::lastError());
        iconFontPath_.clear();
        return false;
    }
    return true;
}

bool Renderer::hasIconFont() const
{
    return iconFont_.valid();
}

Renderer::CachedText* Renderer::acquireIcon(const std::string& utf8, Color c)
{
    if (utf8.empty() || !iconFont_.valid()) return nullptr;

    const Uint32 packed = ((Uint32)c.r << 24) | ((Uint32)c.g << 16) |
                          ((Uint32)c.b << 8)  | (Uint32)c.a;
    const CacheKey key{ utf8, kIconCacheSlot, packed };

    auto it = textCache_.find(key);
    if (it != textCache_.end() && it->second.texture) {
        it->second.lastUsedFrame = frame_;
        return &it->second;
    }

    SDL_Surface* surf = iconFont_.render(utf8, c);
    if (!surf) return nullptr;

    CachedText entry;
    entry.w = surf->w;
    entry.h = surf->h;
    entry.lastUsedFrame = frame_;
    entry.texture = TextureFromSurface(sdl_, surf);
    SDL_FreeSurface(surf);
    if (!entry.texture) return nullptr;

    return &textCache_.emplace(key, entry).first->second;
}

void Renderer::drawIcon(const std::string& utf8, int x, int y, Color c)
{
    CachedText* entry = acquireIcon(utf8, c);
    if (!entry) return;

    SDL_Rect dst = { x, y, entry->w, entry->h };
    const SDL_Rect src = { 0, 0, entry->w, entry->h };
    SDL_RenderCopy(sdl_, entry->texture, &src, &dst);
}

int Renderer::iconWidth(const std::string& utf8)
{
    if (utf8.empty() || !iconFont_.valid()) return 0;
    return iconFont_.measure(utf8);
}

int Renderer::iconHeight() const
{
    if (!iconFont_.valid()) return 0;
    const int skip = iconFont_.lineSkip();
    return skip > 0 ? skip : (PixelSizeFor(FontSize::Small) * 2);
}

int Renderer::textWidth(const std::string& text, FontSize size)
{
    if (text.empty()) return 0;
    const TextFont* font = fontFor(size);
    if (!font) return 0;
    return font->measure(text);
}

int Renderer::lineHeight(FontSize size)
{
    const TextFont* font = fontFor(size);
    const int skip = font ? font->lineSkip() : 0;
    return skip > 0 ? skip : PixelSizeFor(size);
}

int Renderer::drawTextClipped(const std::string& text, int x, int y,
                              int maxWidth, FontSize size, Color c, Align align)
{
    if (text.empty()) return 0;

    if (textWidth(text, size) <= maxWidth) {
        drawText(text, x, y, size, c, align);
        return textWidth(text, size);
    }

    // Trim a character at a time until the ellipsised string fits. Titles are
    // short, so the linear walk is not worth optimizing.
    std::string trimmed = text;
    while (!trimmed.empty()) {
        // Do not split a UTF-8 sequence; step back over continuation bytes.
        do {
            trimmed.pop_back();
        } while (!trimmed.empty() &&
                 ((unsigned char)trimmed.back() & 0xC0) == 0x80);

        if (textWidth(trimmed + "...", size) <= maxWidth) break;
    }

    const std::string out = trimmed + "...";
    drawText(out, x, y, size, c, align);
    return textWidth(out, size);
}

int Renderer::drawTextWrapped(const std::string& text, int x, int y,
                              int maxWidth, FontSize size, Color c, int maxLines)
{
    if (text.empty() || maxLines <= 0) return 0;

    const int step = lineHeight(size);

    // Greedy word wrap. Good enough for overviews; no hyphenation.
    std::vector<std::string> words;
    std::string current;
    for (char ch : text) {
        if (ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t') {
            if (!current.empty()) { words.push_back(current); current.clear(); }
        } else {
            current += ch;
        }
    }
    if (!current.empty()) words.push_back(current);

    int   lines = 0;
    int   drawnY = y;
    std::string line;

    for (size_t i = 0; i < words.size() && lines < maxLines; ++i) {
        const std::string candidate = line.empty() ? words[i] : line + " " + words[i];
        if (textWidth(candidate, size) <= maxWidth) {
            line = candidate;
            continue;
        }

        if (line.empty()) line = words[i];   // a single word wider than the box

        const bool lastLine = (lines + 1 == maxLines);
        if (lastLine) {
            // Everything left has to fit here or be cut off.
            std::string rest = line;
            for (size_t j = i; j < words.size(); ++j) rest += " " + words[j];
            drawTextClipped(rest, x, drawnY, maxWidth, size, c);
            return (lines + 1) * step;
        }

        drawText(line, x, drawnY, size, c);
        drawnY += step;
        ++lines;
        line = words[i];
    }

    if (!line.empty() && lines < maxLines) {
        drawTextClipped(line, x, drawnY, maxWidth, size, c);
        ++lines;
    }
    return lines * step;
}

void Renderer::clearTextCache()
{
    for (auto& entry : textCache_) {
        if (entry.second.texture) SDL_DestroyTexture(entry.second.texture);
    }
    textCache_.clear();
}

void Renderer::trimTextCache(size_t maxEntries)
{
    if (textCache_.size() <= maxEntries) return;

    // Drop anything not drawn in the last few frames.
    for (auto it = textCache_.begin(); it != textCache_.end(); ) {
        if (frame_ - it->second.lastUsedFrame > 120) {
            if (it->second.texture) SDL_DestroyTexture(it->second.texture);
            it = textCache_.erase(it);
        } else {
            ++it;
        }
    }
}
