// SPDX-License-Identifier: GPL-2.0-or-later

// image_load_stb.cpp: artwork without an image library.
//
// stb_image decodes the JPEG or PNG to RGBA8888 in memory and stb_image_write
// puts a PNG back; the surface and texture handling on either side of that is
// ordinary SDL, so the caller cannot tell which implementation it has.

#include "ui/image_load.h"

#include "core/platform.h"

#if !BJ_IMAGE_SDL

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO          // the console spells paths its own way; see below

// No thread local storage. stb_image marks its error string thread_local
// under C++11, which neither console supports: the Xbox 360 leaves r2 zero so
// the first write faults, and elf2rpl cannot process the relocations on the
// Wii U. Nothing here decodes on two threads at once anyway.
#define STBI_NO_THREAD_LOCALS
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace {

const char* g_error = "";

int NextPowerOfTwo(int value)
{
    int result = 1;
    while (result < value) result <<= 1;
    return result;
}

// stb's own stdio path is switched off so that every file this build opens
// goes through NativePath first.
bool ReadWholeFile(const std::string& path, std::vector<unsigned char>& out)
{
    std::FILE* file = std::fopen(Platform::NativePath(path).c_str(), "rb");
    if (!file) { g_error = "could not open the file"; return false; }

    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (size <= 0) { std::fclose(file); g_error = "the file is empty"; return false; }

    out.resize((size_t)size);
    const size_t read = std::fread(out.data(), 1, (size_t)size, file);
    std::fclose(file);
    if (read != (size_t)size) { out.clear(); g_error = "short read"; return false; }
    return true;
}

}  // namespace

namespace Image {

bool Init() { return true; }
void Quit() {}
const char* LastError() { return g_error; }

SDL_Texture* LoadTexture(SDL_Renderer* renderer, const std::string& path)
{
    std::vector<unsigned char> file;
    if (!ReadWholeFile(path, file)) return nullptr;

    int w = 0, h = 0, channels = 0;
    // Four channels always, so the pixel layout below is not conditional on
    // whether a particular JPEG happened to be greyscale.
    stbi_uc* pixels = stbi_load_from_memory(file.data(), (int)file.size(),
                                            &w, &h, &channels, 4);
    if (!pixels) { g_error = stbi_failure_reason(); return nullptr; }

    // stb hands back R,G,B,A in memory order, which is SDL_PIXELFORMAT_RGBA32.
    // This renderer advertises exactly one texture format and it is ARGB8888,
    // so the swizzle happens here rather than being asked of a driver that
    // cannot do it. Big-endian, so the packed Uint32 is A,R,G,B in memory too.
    Uint32* argb = (Uint32*)pixels;
    for (int i = 0, n = w * h; i < n; ++i) {
        const stbi_uc* p = pixels + (size_t)i * 4;
        argb[i] = ((Uint32)p[3] << 24) | ((Uint32)p[0] << 16) |
                  ((Uint32)p[1] << 8)  | (Uint32)p[2];
    }

    // Streaming rather than static, and padded to a power of two, for the same
    // reasons as the text cache: the streaming path is what Chocolate Doom
    // runs on this console, and non-power-of-two textures came back sampling
    // each other's memory. The image sits in the top-left; ArtCache draws it
    // with a source rectangle.
    const int texW = NextPowerOfTwo(w);
    const int texH = NextPowerOfTwo(h);
    SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                             SDL_TEXTUREACCESS_STREAMING, texW, texH);
    if (!texture) {
        g_error = SDL_GetError();
        stbi_image_free(pixels);
        return nullptr;
    }

    void* locked = nullptr;
    int   pitch  = 0;
    if (SDL_LockTexture(texture, nullptr, &locked, &pitch) != 0) {
        g_error = SDL_GetError();
        SDL_DestroyTexture(texture);
        stbi_image_free(pixels);
        return nullptr;
    }
    SDL_memset(locked, 0, (size_t)pitch * texH);
    for (int y = 0; y < h; ++y) {
        // Four bytes a pixel on both sides: the source row stride is w * 4,
        // not w. It was w, and every row after the first came from the wrong
        // place.
        SDL_memcpy((Uint8*)locked + (size_t)y * pitch,
                   (const Uint8*)argb + (size_t)y * w * 4, (size_t)w * 4);
    }
    SDL_UnlockTexture(texture);

    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    // The texture is padded; record what part of it is the picture, which is
    // what Renderer::drawTextureCover needs to keep the aspect right.
    SDL_SetTextureUserData(texture,
        (void*)(uintptr_t)(((uintptr_t)(w & 0xFFFF) << 16) | (uintptr_t)(h & 0xFFFF)));
    stbi_image_free(pixels);
    return texture;
}

bool SavePng(SDL_Surface* surface, const std::string& path)
{
    if (!surface) return false;

    SDL_Surface* rgba = SDL_ConvertSurfaceFormat(surface, SDL_PIXELFORMAT_RGBA32, 0);  // stb writes in memory order
    if (!rgba) { g_error = SDL_GetError(); return false; }

    const int ok = stbi_write_png(Platform::NativePath(path).c_str(), rgba->w, rgba->h, 4,
                                  rgba->pixels, rgba->pitch);
    SDL_FreeSurface(rgba);
    if (!ok) g_error = "stb_image_write refused to write the file";
    return ok != 0;
}

}  // namespace Image

#endif  // !BJ_IMAGE_SDL
