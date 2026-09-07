/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2025 Sam Lantinga <slouken@libsdl.org>

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

#include "SDL_xinput.h"

/* Set up for C function definitions, even when using C++ */
#ifdef __cplusplus
extern "C" {
#endif

XInputGetState_t SDL_XInputGetState = NULL;
XInputSetState_t SDL_XInputSetState = NULL;
XInputGetCapabilities_t SDL_XInputGetCapabilities = NULL;
XInputGetCapabilitiesEx_t SDL_XInputGetCapabilitiesEx = NULL;
/*XInputGetBatteryInformation_t SDL_XInputGetBatteryInformation = NULL;*/
DWORD SDL_XInputVersion = 0;

static HMODULE s_pXInputDLL = NULL;
static int s_XInputDLLRefCount = 0;

int WIN_LoadXInputDLL(void)
{
    /* Getting handles to system dlls (via LoadLibrary and its variants) is not
     * supported on WinRT, thus, pointers to XInput's functions can't be
     * retrieved via GetProcAddress.
     *
     * When on WinRT, assume that XInput is already loaded, and directly map
     * its XInput.h-declared functions to the SDL_XInput* set of function
     * pointers.
     *
     * Side-note: XInputGetStateEx is not available for use in WinRT.
     * This seems to mean that support for the guide button is not available
     * in WinRT, unfortunately.
     */
    SDL_XInputGetState = (XInputGetState_t)XInputGetState;
    SDL_XInputSetState = (XInputSetState_t)XInputSetState;
    SDL_XInputGetCapabilities = (XInputGetCapabilities_t)XInputGetCapabilities;
    //SDL_XInputGetBatteryInformation = (XInputGetBatteryInformation_t)XInputGetBatteryInformation;

    /* XInput 1.4 ships with Windows 8 and 8.1: */
    SDL_XInputVersion = (1 << 16) | 4;

    return 0;
}

void WIN_UnloadXInputDLL(void)
{
}

/* Ends C function definitions when using C++ */
#ifdef __cplusplus
}
#endif

/* vi: set ts=4 sw=4 expandtab: */
