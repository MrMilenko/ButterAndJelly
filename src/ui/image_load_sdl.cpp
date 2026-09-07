// SPDX-License-Identifier: GPL-2.0-or-later

// image_load_sdl.cpp: the SDL_image implementation.

#include "ui/image_load.h"

#if BJ_IMAGE_SDL

#include <SDL_image.h>

namespace Image {

bool Init()
{
    const int wanted = IMG_INIT_JPG | IMG_INIT_PNG;
    return (IMG_Init(wanted) & wanted) != 0;
}

void Quit() { IMG_Quit(); }

const char* LastError() { return IMG_GetError(); }

SDL_Texture* LoadTexture(SDL_Renderer* renderer, const std::string& path)
{
    return IMG_LoadTexture(renderer, path.c_str());
}

bool SavePng(SDL_Surface* surface, const std::string& path)
{
    return IMG_SavePNG(surface, path.c_str()) == 0;
}

}  // namespace Image

#endif  // BJ_IMAGE_SDL
