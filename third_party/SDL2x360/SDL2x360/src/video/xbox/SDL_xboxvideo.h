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

#ifndef SDL_xboxvideo_h_
#define SDL_xboxvideo_h_

#include "../../core/xbox/SDL_xbox.h"
#include "../SDL_sysvideo.h"

#include "SDL_xboxevents.h"
#include "SDL_xboxkeyboard.h"
#include "SDL_xboxmouse.h"
#include "SDL_xboxwindow.h"
#include "SDL_events.h"
#ifndef _XBOX
#include "SDL_loadso.h"
#endif

typedef struct SDL_VideoData
{
	int render; // Redundant
} SDL_VideoData;

typedef struct IDirect3D8 IDirect3D8;
extern SDL_bool D3D_LoadDLL(/*void **pD3DDLL,*/ IDirect3D8** pDirect3D8Interface);

// TODO: Not used atm
SDL_bool XBOX_HasScreenKeyboardSupport(_THIS);
void XBOX_ShowScreenKeyboard(_THIS, SDL_Window* window);
void XBOX_HideScreenKeyboard(_THIS, SDL_Window* window);
SDL_bool XBOX_IsScreenKeyboardShown(_THIS, SDL_Window* window);

#endif /* SDL_xboxvideo_h_ */

/* vi: set ts=4 sw=4 expandtab: */
