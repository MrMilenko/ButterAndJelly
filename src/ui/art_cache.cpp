// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/art_cache.h"

#include <cstdint>

#include "core/jellyfin.h"
#include "core/http.h"
#include "core/features.h"
#include "core/platform.h"
#include "core/worker.h"
#include "core/thread.h"
#include "ui/image_load.h"
#include "core/log.h"
#include "core/text.h"

#include <SDL.h>

namespace {
// Enough to say where artwork stops working without a line per poster.
int  g_artLogRemaining = 8;
bool g_artLoggedFirst  = false;
}  // namespace

ArtCache::ArtCache(SDL_Renderer* renderer, JellyfinClient& client, WorkerPool& pool)
    : renderer_(renderer), client_(client), pool_(pool)
{
    Platform::MakeDirs(Platform::DataDir() + "/art");
}

ArtCache::~ArtCache()
{
    clear();
}

// The key carries the size as well as the item, or whichever size is asked
// for first gets stretched to serve every later request.
std::string ArtCache::cacheKey(const std::string& itemId,
                               const std::string& variant,
                               int targetWidth, int targetHeight)
{
    // The key is the file name too, and FATX allows no ":" and 42 characters
    // at most. Ids run from a 32 character guid to "seerr:movie:1003821", so
    // the whole thing is hashed rather than cut short.
    uint64_t hash = 1469598103934665603ull;
    const std::string parts[2] = { itemId, variant };
    for (const std::string& part : parts) {
        for (size_t i = 0; i < part.size(); ++i) {
            hash ^= (unsigned char)part[i];
            hash *= 1099511628211ull;
        }
        hash ^= '|';
        hash *= 1099511628211ull;
    }

    static const char* kHex = "0123456789abcdef";
    std::string name(16, '0');
    for (int i = 15; i >= 0; --i) {
        name[(size_t)i] = kHex[hash & 0xF];
        hash >>= 4;
    }
    return name + "_" + bj::ToString(targetWidth) +
           "x" + bj::ToString(targetHeight);
}

std::string ArtCache::diskPath(const std::string& key) const
{
    return Platform::DataDir() + "/art/" + key + ".jpg";
}

void ArtCache::evictIfFull()
{
    while (textures_.size() >= BJ_ART_TEXTURES) {
        auto oldest = textures_.begin();
        for (auto at = textures_.begin(); at != textures_.end(); ++at) {
            if (at->second.used < oldest->second.used) oldest = at;
        }
        if (oldest->second.texture) SDL_DestroyTexture(oldest->second.texture);
        textures_.erase(oldest);
    }
}

SDL_Texture* ArtCache::get(const std::string& itemId,
                           const std::string& primaryTag,
                           int targetWidth,
                           int targetHeight,
                           const std::string& absoluteUrl)
{
    if (itemId.empty()) return nullptr;

    // A machine that cannot hold a texture the size of the tile asks for a
    // smaller picture and lets the GPU scale it up.
    if (BJ_ART_MAX_WIDTH > 0 && targetWidth > BJ_ART_MAX_WIDTH) {
        targetHeight = targetHeight * BJ_ART_MAX_WIDTH / targetWidth;
        targetWidth  = BJ_ART_MAX_WIDTH;
    }

    const std::string key = cacheKey(itemId,
                                     absoluteUrl.empty() ? primaryTag : absoluteUrl,
                                     targetWidth, targetHeight);

    auto it = textures_.find(key);
    if (it != textures_.end()) {
        it->second.used = ++clock_;
        return it->second.texture;
    }

    // A poster that already failed once stays failed for this session rather
    // than re-requesting it every single frame.
    if (failed_.count(key)) return nullptr;

    {
        bj::ScopedLock lock(mutex_);
        if (inFlight_.count(key)) return nullptr;
    }

    const std::string path = diskPath(key);

    // Already on disk: no network, so this needs no share of the pool.
    if (Platform::FileExists(path)) {
        bj::ScopedLock lock(mutex_);
        inFlight_.insert(key);
        readyOnDisk_.push_back(key);
        return nullptr;
    }

    {
        bj::ScopedLock lock(mutex_);
        // Leaves workers for everything else. Without this a grid of posters
        // takes the whole pool and a detail screen waits behind all of them.
        if (inFlight_.size() >= BJ_ART_IN_FLIGHT) return nullptr;
        inFlight_.insert(key);
    }

    const std::string url = absoluteUrl.empty()
        ? client_.imageUrl(itemId, primaryTag, targetWidth, targetHeight)
        : absoluteUrl;
    if (url.empty()) {
        bj::ScopedLock lock(mutex_);
        inFlight_.erase(key);
        failed_.insert(key);
        return nullptr;
    }

    pool_.submit([this, key, url, path] {
        const bool ok = Http::GetToFile(url, path);
        // Quitting cancels transfers in flight, which is not a failure.
        if (!ok && !Http::AbortRequested() && g_artLogRemaining > 0) {
            --g_artLogRemaining;
            LOGF("[art] could not fetch %s", url.c_str());
        }
        bj::ScopedLock lock(mutex_);
        if (ok) {
            readyOnDisk_.push_back(key);
        } else {
            inFlight_.erase(key);
            // Recorded on the main thread during processCompleted instead of
            // here, so failed_ stays single-threaded.
            readyOnDisk_.push_back("!" + key);
        }
    });

    return nullptr;
}

void ArtCache::processCompleted(int budget)
{
    for (int done = 0; done < budget; ++done) {
        std::string key;
        {
            bj::ScopedLock lock(mutex_);
            if (readyOnDisk_.empty()) return;
            key = readyOnDisk_.front();
            readyOnDisk_.pop_front();
        }

        if (!key.empty() && key[0] == '!') {
            failed_.insert(key.substr(1));
            continue;
        }

        SDL_Texture* tex = Image::LoadTexture(renderer_, diskPath(key));
        if (!tex && g_artLogRemaining > 0) {
            --g_artLogRemaining;
            LOGF("[art] %s downloaded but would not decode: %s",
                 diskPath(key).c_str(), Image::LastError());
        } else if (tex && !g_artLoggedFirst) {
            g_artLoggedFirst = true;
            int w = 0, h = 0;
            SDL_QueryTexture(tex, nullptr, nullptr, &w, &h);
            LOGF("[art] first poster decoded, texture %dx%d", w, h);
        }
        {
            bj::ScopedLock lock(mutex_);
            inFlight_.erase(key);
        }

        if (tex) {
#if defined(_XBOX) && !defined(_XENON)
            // Says so once if the machine ever gets close, which is the only
            // thing worth knowing after the budget is set.
            Platform::WarnIfMemoryLow("artwork");
#endif
            evictIfFull();
            Held held;
            held.texture = tex;
            held.used    = ++clock_;
            textures_[key] = held;
        } else {
            // The file exists but will not decode, so it is truncated or not
            // an image. Drop it; the next run re-downloads.
            std::remove(Platform::NativePath(diskPath(key)).c_str());
            failed_.insert(key);
        }
    }
}

size_t ArtCache::inFlightCount()
{
    bj::ScopedLock lock(mutex_);
    return inFlight_.size();
}

void ArtCache::clear()
{
    for (auto& entry : textures_) {
        if (entry.second.texture) SDL_DestroyTexture(entry.second.texture);
    }
    textures_.clear();
    failed_.clear();
    bj::ScopedLock lock(mutex_);
    inFlight_.clear();
    readyOnDisk_.clear();
}
