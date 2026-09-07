// SPDX-License-Identifier: GPL-2.0-or-later

// Poster artwork, from server to texture. Three stages, because only the
// middle one may block:
//
//   1. the main thread asks for an item's poster
//   2. a pool thread downloads it under DataDir()/art
//   3. the main thread decodes and uploads, a few per frame
//
// The disk copy survives a relaunch.

#pragma once

#include "core/thread.h"

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
    SDL_Texture* get(const std::string& itemId,
                     const std::string& primaryTag,
                     int targetWidth,
                     int targetHeight);

    // Uploads at most `budget` decoded images. Called once per frame so a
    // burst of finished downloads cannot stall the UI.
    void processCompleted(int budget = 2);

    void clear();

    // Diagnostics for the on-screen debug overlay.
    size_t textureCount() const { return textures_.size(); }
    size_t inFlightCount();

private:
    static std::string cacheKey(const std::string& itemId,
                                int targetWidth, int targetHeight);
    std::string diskPath(const std::string& key) const;

    SDL_Renderer*   renderer_;
    JellyfinClient& client_;
    WorkerPool&     pool_;

    std::map<std::string, SDL_Texture*> textures_;   // main thread only
    std::set<std::string>               failed_;     // main thread only

    bj::Mutex            mutex_;
    std::set<std::string> inFlight_;                 // guarded
    std::deque<std::string> readyOnDisk_;            // guarded
};
