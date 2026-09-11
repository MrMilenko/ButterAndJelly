// SPDX-License-Identifier: GPL-2.0-or-later

// image_load.h: decoding artwork, and writing screenshots.
//
// SDL_image where a build of it exists, stb otherwise. Only JPEG and PNG are
// ever wanted.

#pragma once

#include <string>

#include <SDL.h>

// Neither Xbox has an SDL_image build.
#if defined(_XBOX)
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
