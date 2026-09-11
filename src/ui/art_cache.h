// SPDX-License-Identifier: GPL-2.0-or-later

// Poster artwork, from server to texture. The main thread asks, a pool thread
// downloads under DataDir()/art, and the main thread decodes and uploads a few
// per frame. The disk copy survives a relaunch.

#pragma once

#include "core/thread.h"

#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <string>

struct SDL_Renderer;
struct SDL_Texture;

class JellyfinClient;
class WorkerPool;

class ArtCache {
public:
    ArtCache(SDL_Renderer* renderer, JellyfinClient& client, WorkerPool& pool);
    ~ArtCache();

    // Returns the texture if it is ready, otherwise null and starts fetching.
    // Safe to call every frame for every visible tile.
    // absoluteUrl overrides the Jellyfin URL the client would build, for
    // items whose artwork lives somewhere else entirely.
    SDL_Texture* get(const std::string& itemId,
                     const std::string& primaryTag,
                     int targetWidth,
                     int targetHeight,
                     const std::string& absoluteUrl = std::string());

    // Uploads at most `budget` decoded images. Called once per frame so a
    // burst of finished downloads cannot stall the UI.
    void processCompleted(int budget = 2);

    void clear();

    // Diagnostics for the on-screen debug overlay.
    size_t textureCount() const { return textures_.size(); }
    size_t inFlightCount();

private:
    // `variant` is whatever identifies this particular image: Jellyfin's
    // image tag, or the full URL when the artwork lives elsewhere. Included
    // so replaced artwork gets a new file rather than the old one forever.
    static std::string cacheKey(const std::string& itemId,
                                const std::string& variant,
                                int targetWidth, int targetHeight);
    std::string diskPath(const std::string& key) const;

    SDL_Renderer*   renderer_;
    JellyfinClient& client_;
    WorkerPool&     pool_;

    // Textures are bounded: a paged grid would otherwise hold every poster
    // it has ever shown, and one of those is megabytes on a console. `used`
    // is a counter, so the least recently asked for goes first.
    struct Held {
        SDL_Texture* texture = nullptr;
        uint64_t     used    = 0;
    };
    void evictIfFull();

    std::map<std::string, Held> textures_;   // main thread only
    std::set<std::string>       failed_;     // main thread only
    uint64_t                    clock_ = 0;

    bj::Mutex            mutex_;
    std::set<std::string> inFlight_;                 // guarded
    std::deque<std::string> readyOnDisk_;            // guarded
};
