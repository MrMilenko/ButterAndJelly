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

#ifndef SDL_windowswindow_h_
#define SDL_windowswindow_h_

#if SDL_VIDEO_OPENGL_EGL
#include "../SDL_egl_c.h"
#endif

extern void XBOX_360_GetDisplayModes(_THIS, SDL_VideoDisplay* display);
extern int XBOX_360_SetDisplayMode(_THIS, SDL_VideoDisplay* display, SDL_DisplayMode* mode);
extern int XBOX_360_CreateWindow(_THIS, SDL_Window* window);
extern int XBOX_360_CreateWindowFrom(_THIS, SDL_Window* window, const void* data);
extern void XBOX_360_SetWindowTitle(_THIS, SDL_Window* window);
extern void XBOX_360_SetWindowIcon(_THIS, SDL_Window* window, SDL_Surface* icon);
extern void XBOX_360_SetWindowPosition(_THIS, SDL_Window* window);
extern void XBOX_360_SetWindowSize(_THIS, SDL_Window* window);
extern void XBOX_360_ShowWindow(_THIS, SDL_Window* window);
extern void XBOX_360_HideWindow(_THIS, SDL_Window* window);
extern void XBOX_360_RaiseWindow(_THIS, SDL_Window* window);
extern void XBOX_360_MaximizeWindow(_THIS, SDL_Window* window);
extern void XBOX_360_MinimizeWindow(_THIS, SDL_Window* window);
extern void XBOX_360_RestoreWindow(_THIS, SDL_Window* window);
extern void XBOX_360_SetWindowGrab(_THIS, SDL_Window* window, SDL_bool grabbed);
extern void XBOX_360_DestroyWindow(_THIS, SDL_Window* window);
extern void XBOX_360_OnWindowEnter(_THIS, SDL_Window* window);
extern int XBOX_360_SetWindowHitTest(SDL_Window* window, SDL_bool enabled);
extern void XBOX_360_AcceptDragAndDrop(SDL_Window* window, SDL_bool accept);

#endif /* SDL_windowswindow_h_ */

/* vi: set ts=4 sw=4 expandtab: */
