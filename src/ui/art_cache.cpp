// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/art_cache.h"

#include "core/jellyfin.h"
#include "core/http.h"
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

// The key, and the file name, carry the size as well as the item.
//
// Keying on the item alone means whichever size is asked for first wins for
// the rest of the session: the home rows want 260 and ask first, so the detail
// page's request for 600 gets the 260 one back and stretches it. That is what
// made posters look soft.
std::string ArtCache::cacheKey(const std::string& itemId,
                               int targetWidth, int targetHeight)
{
    // The key is also the file name, and FATX allows no "@" and at most 42
    // characters. A full 32 character item id plus the size and ".jpg.part"
    // exceeds that, so half the id is used: still 64 bits.
    const size_t kMaxIdChars = 16;
    const std::string shortId = itemId.size() > kMaxIdChars
                                    ? itemId.substr(0, kMaxIdChars)
                                    : itemId;
    return shortId + "_" + bj::ToString(targetWidth) +
           "x" + bj::ToString(targetHeight);
}

std::string ArtCache::diskPath(const std::string& key) const
{
    return Platform::DataDir() + "/art/" + key + ".jpg";
}

SDL_Texture* ArtCache::get(const std::string& itemId,
                           const std::string& primaryTag,
                           int targetWidth,
                           int targetHeight)
{
    if (itemId.empty()) return nullptr;

    const std::string key = cacheKey(itemId, targetWidth, targetHeight);

    auto it = textures_.find(key);
    if (it != textures_.end()) return it->second;

    // A poster that already failed once stays failed for this session rather
    // than re-requesting it every single frame.
    if (failed_.count(key)) return nullptr;

    {
        bj::ScopedLock lock(mutex_);
        if (inFlight_.count(key)) return nullptr;
        inFlight_.insert(key);
    }

    const std::string path = diskPath(key);

    // Already on disk from a previous run: skip straight to the upload queue.
    if (Platform::FileExists(path)) {
        bj::ScopedLock lock(mutex_);
        readyOnDisk_.push_back(key);
        return nullptr;
    }

    const std::string url = client_.imageUrl(itemId, primaryTag,
                                            targetWidth, targetHeight);
    if (url.empty()) {
        bj::ScopedLock lock(mutex_);
        inFlight_.erase(key);
        failed_.insert(key);
        return nullptr;
    }

    pool_.submit([this, key, url, path] {
        const bool ok = Http::GetToFile(url, path);
        if (!ok && g_artLogRemaining > 0) {
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
            textures_[key] = tex;
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
        if (entry.second) SDL_DestroyTexture(entry.second);
    }
    textures_.clear();
    failed_.clear();
    bj::ScopedLock lock(mutex_);
    inFlight_.clear();
    readyOnDisk_.clear();
}
