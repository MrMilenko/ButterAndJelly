// SPDX-License-Identifier: GPL-2.0-or-later

// image_load.h: decoding artwork, and writing screenshots.
//
// Two implementations, chosen the same way TextFont is. SDL_image where a
// build of it exists; stb_image and stb_image_write on the Xbox 360, which has
// neither SDL_image nor libpng nor libjpeg. Only JPEG and PNG are ever
// wanted, which is what a Jellyfin server serves artwork as, and which is
// exactly the part of SDL_image these two headers already cover.

#pragma once

#include <string>

#include <SDL.h>

#if defined(_XENON)
  #define BJ_IMAGE_SDL 0
#else
  #define BJ_IMAGE_SDL 1
#endif

namespace Image {

bool Init();
void Quit();
const char* LastError();

// Decodes the file at `path` straight into a texture, or nullptr if it will
// not decode. The caller owns the texture.
SDL_Texture* LoadTexture(SDL_Renderer* renderer, const std::string& path);

// Writes `surface` out as a PNG. Used for screenshots.
bool SavePng(SDL_Surface* surface, const std::string& path);

}  // namespace Image
