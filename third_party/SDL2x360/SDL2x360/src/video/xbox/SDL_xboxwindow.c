/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2019 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#include "../../SDL_internal.h"

#if SDL_VIDEO_DRIVER_XBOX

#include "../SDL_sysvideo.h"

#include "SDL_xboxvideo.h"
#include "SDL_xboxwindow.h"

extern SDL_DisplayMode g_XboxDesktopMode;

void
XBOX_GetDisplayModes(_THIS, SDL_VideoDisplay* display)
{
    DWORD vflags = XGetVideoFlags();
    SDL_DisplayMode mode;
    // These are common across all modes
    mode.format = SDL_PIXELFORMAT_ARGB8888;
    mode.driverdata = NULL;

    if (XGetVideoStandard() == XC_VIDEO_STANDARD_PAL_I) {
        /* PAL SD: 576-line 50 Hz */
        mode.w = 720;
        mode.h = 576;
        mode.refresh_rate = 50;
        SDL_AddDisplayMode(display, &mode);

        /* PAL60 SD: 576-line 60 Hz */
        if (vflags & XC_VIDEO_FLAGS_PAL_60Hz) {
            mode.w = 720;
            mode.h = 576;
            mode.refresh_rate = 60;
            SDL_AddDisplayMode(display, &mode);
        }
    }
    else {
        /* All NTSC modes use 60Hz */
        mode.refresh_rate = 60;

        /* NTSC SD 480i fallback */
        mode.w = 640;
        mode.h = 480;
        SDL_AddDisplayMode(display, &mode);

        if (vflags & XC_VIDEO_FLAGS_HDTV_480p) {
            mode.w = 720;
            mode.h = 480;
            SDL_AddDisplayMode(display, &mode);
        }

        if (vflags & XC_VIDEO_FLAGS_HDTV_720p) {
            mode.w = 1280;
            mode.h = 720;
            SDL_AddDisplayMode(display, &mode);
        }

        if (vflags & XC_VIDEO_FLAGS_HDTV_1080i) {
            mode.w = 1920;
            mode.h = 1080;
            SDL_AddDisplayMode(display, &mode);
        }
    }
}

int XBOX_SetDisplayMode(_THIS, SDL_VideoDisplay* display, SDL_DisplayMode* mode)
{
    // Accept only modes compatible with dashboard flags; otherwise fallback.
    display->current_mode = *mode;
    g_XboxDesktopMode = *mode;
    return 0;  // pretend success, but state is consistent
}


int XBOX_CreateWindow(_THIS, SDL_Window* window)
{
    const SDL_DisplayMode* dm = &g_XboxDesktopMode;  // set in XBOX_VideoInit

    // Force fullscreen, exact desktop mode size
    window->x = 0;
    window->y = 0;
    window->w = dm->w;
    window->h = dm->h;
    window->flags |= SDL_WINDOW_FULLSCREEN;

    // (Optional) ensure no residual viewport issues:
    // Your renderer init should set viewport to full size.

    XBOX_PumpEvents(_this);
    return 0;
}

int
XBOX_CreateWindowFrom(_THIS, SDL_Window* window, const void* data)
{
    return SDL_Unsupported();
}

void
XBOX_SetWindowTitle(_THIS, SDL_Window* window)
{
}

void
XBOX_SetWindowIcon(_THIS, SDL_Window* window, SDL_Surface* icon)
{
}

void
XBOX_SetWindowPosition(_THIS, SDL_Window* window)
{
}

void
XBOX_SetWindowSize(_THIS, SDL_Window* window)
{
}

void
XBOX_ShowWindow(_THIS, SDL_Window* window)
{
}

void
XBOX_HideWindow(_THIS, SDL_Window* window)
{
}

void
XBOX_RaiseWindow(_THIS, SDL_Window* window)
{
}

void
XBOX_MaximizeWindow(_THIS, SDL_Window* window)
{
}

void
XBOX_MinimizeWindow(_THIS, SDL_Window* window)
{
}

void
XBOX_RestoreWindow(_THIS, SDL_Window* window)
{
}

void
XBOX_SetWindowGrab(_THIS, SDL_Window* window, SDL_bool grabbed)
{
}

void
XBOX_DestroyWindow(_THIS, SDL_Window* window)
{
}

void XBOX_OnWindowEnter(_THIS, SDL_Window* window)
{
}

int
XBOX_SetWindowHitTest(SDL_Window* window, SDL_bool enabled)
{
    return 0;  /* just succeed, the real work is done elsewhere. */
}

void
XBOX_AcceptDragAndDrop(SDL_Window* window, SDL_bool accept)
{
}

#endif /* SDL_VIDEO_DRIVER_XBOX */

/* vi: set ts=4 sw=4 expandtab: */