// SPDX-License-Identifier: GPL-2.0-or-later

// render.h: drawing helpers over SDL_Renderer.
//
// Text is the expensive part: SDL_ttf rasterises to a surface every call, so
// every string we draw is cached as a texture keyed by content, size and
// colour. A library grid redraws the same labels every frame and would
// otherwise re-rasterise a few hundred strings at 60Hz.

#pragma once

#include <map>
#include <string>
#include <tuple>

#include <SDL.h>

#include "ui/palette.h"
#include "ui/text_font.h"

enum class FontSize { Small, Body, Title, Huge, Display };
enum class Align    { Left, Center, Right };


class Renderer {
public:
    Renderer() = default;
    ~Renderer();

    bool init(SDL_Renderer* sdl, const std::string& fontDir);
    void shutdown();

    SDL_Renderer* sdl() const { return sdl_; }

    // ---- primitives ----
    void clear(Color c);
    void fillRect(const SDL_Rect& r, Color c);
    void strokeRect(const SDL_Rect& r, Color c, int thickness = 1);
    // Rounded corners are drawn as a cross plus four quarter-circles. Cheap
    // enough for a handful of panels; not meant for hundreds.
    void fillRoundedRect(const SDL_Rect& r, int radius, Color c);

    // Draws `tex` inside `dst`, preserving aspect and cropping the overflow.
    void drawTextureCover(SDL_Texture* tex, const SDL_Rect& dst);

    // ---- text ----
    void drawText(const std::string& text, int x, int y,
                  FontSize size, Color c, Align align = Align::Left);
    // Wraps to `maxWidth`, stops after `maxLines`, ellipsising the last line.
    // Returns the height actually used.
    int  drawTextWrapped(const std::string& text, int x, int y, int maxWidth,
                         FontSize size, Color c, int maxLines = 3);
    // Single line, ellipsised to fit. Returns the width drawn.
    int  drawTextClipped(const std::string& text, int x, int y, int maxWidth,
                         FontSize size, Color c, Align align = Align::Left);

    int  textWidth(const std::string& text, FontSize size);
    int  lineHeight(FontSize size);

    // Frees cached text textures once the cache grows past a soft limit.
    void trimTextCache(size_t maxEntries = 512);

    // Drops every cached texture. Needed after the console takes the
    // foreground away, which invalidates what the GPU was holding.
    void clearTextCache();

private:
    struct CachedText {
        SDL_Texture* texture = nullptr;
        int w = 0, h = 0;
        Uint32 lastUsedFrame = 0;
    };

    const TextFont* fontFor(FontSize size) const;
    CachedText* acquire(const std::string& text, FontSize size, Color c);

    SDL_Renderer* sdl_ = nullptr;
    TextFont fonts_[5];

    using CacheKey = std::tuple<std::string, int, Uint32>;
    std::map<CacheKey, CachedText> textCache_;
    Uint32 frame_ = 0;

public:
    // Bumped once per frame so the cache can evict what nobody drew.
    void beginFrame() { ++frame_; }
};
