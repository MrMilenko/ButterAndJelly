/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

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

#if SDL_VIDEO_DRIVER_XBOX_360

#include "../SDL_sysvideo.h"

#include "SDL_xbox360video.h"
#include "SDL_xbox360window.h"
#include "../../events/SDL_keyboard_c.h"

extern SDL_DisplayMode g_XboxDesktopMode;

void
XBOX_360_GetDisplayModes(_THIS, SDL_VideoDisplay* display)
{
    XVIDEO_MODE VideoMode;
    SDL_DisplayMode mode;

	XMemSet( &VideoMode, 0, sizeof(XVIDEO_MODE) ); 
	XGetVideoMode( &VideoMode );

    // These are common across all modes
    mode.format = SDL_PIXELFORMAT_ARGB8888;
    mode.w = VideoMode.dwDisplayWidth;
    mode.h = VideoMode.dwDisplayHeight;

    /* Cap advertised modes at 720p: a 1080p ARGB8888 back buffer plus depth is
       16.6 MB against 10 MB of EDRAM, so CreateDevice fails with E_OUTOFMEMORY. */
    if (mode.w > 1280 || mode.h > 720) {
        mode.w = 1280;
        mode.h = 720;
    }
    mode.refresh_rate = (int)VideoMode.RefreshRate;
    mode.driverdata = NULL;

    SDL_AddDisplayMode(display, &mode);
}

int XBOX_360_SetDisplayMode(_THIS, SDL_VideoDisplay* display, SDL_DisplayMode* mode)
{
    // Accept only modes compatible with dashboard flags; otherwise fallback.
    display->current_mode = *mode;
    g_XboxDesktopMode = *mode;
    return 0;  // pretend success, but state is consistent
}


int XBOX_360_CreateWindow(_THIS, SDL_Window* window)
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

    window->flags |= SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_MOUSE_FOCUS;
    SDL_SetKeyboardFocus(window);

    XBOX_360_PumpEvents(_this);
    return 0;
}

int
XBOX_360_CreateWindowFrom(_THIS, SDL_Window* window, const void* data)
{
    return SDL_Unsupported();
}

void
XBOX_360_SetWindowTitle(_THIS, SDL_Window* window)
{
}

void
XBOX_360_SetWindowIcon(_THIS, SDL_Window* window, SDL_Surface* icon)
{
}

void
XBOX_360_SetWindowPosition(_THIS, SDL_Window* window)
{
}

void
XBOX_360_SetWindowSize(_THIS, SDL_Window* window)
{
}

void
XBOX_360_ShowWindow(_THIS, SDL_Window* window)
{
}

void
XBOX_360_HideWindow(_THIS, SDL_Window* window)
{
}

void
XBOX_360_RaiseWindow(_THIS, SDL_Window* window)
{
}

void
XBOX_360_MaximizeWindow(_THIS, SDL_Window* window)
{
}

void
XBOX_360_MinimizeWindow(_THIS, SDL_Window* window)
{
}

void
XBOX_360_RestoreWindow(_THIS, SDL_Window* window)
{
}

void
XBOX_360_SetWindowGrab(_THIS, SDL_Window* window, SDL_bool grabbed)
{
}

void
XBOX_360_DestroyWindow(_THIS, SDL_Window* window)
{
}

void XBOX_360_OnWindowEnter(_THIS, SDL_Window* window)
{
}

int
XBOX_360_SetWindowHitTest(SDL_Window* window, SDL_bool enabled)
{
    return 0;  /* just succeed, the real work is done elsewhere. */
}

void
XBOX_360_AcceptDragAndDrop(SDL_Window* window, SDL_bool accept)
{
}

#endif /* SDL_VIDEO_DRIVER_XBOX */

/* vi: set ts=4 sw=4 expandtab: */