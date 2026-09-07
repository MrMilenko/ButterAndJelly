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

#include "SDL_xbox360.h"
#include "SDL_error.h"
#include "SDL_system.h"

typedef enum RO_INIT_TYPE
{
    RO_INIT_SINGLETHREADED = 0,
    RO_INIT_MULTITHREADED = 1
} RO_INIT_TYPE;


/* Sets an error message based on an HRESULT */
int WIN_SetErrorFromHRESULT(const char *prefix, HRESULT hr)
{
	/* TODO */
    return -1;
}

/* Sets an error message based on GetLastError() */
int WIN_SetError(const char *prefix)
{
    return WIN_SetErrorFromHRESULT(prefix, GetLastError());
}

HRESULT
WIN_CoInitialize(void)
{
    /* SDL handles any threading model, so initialize with the default, which
       is compatible with OLE and if that doesn't work, try multi-threaded mode.

       If you need multi-threaded mode, call CoInitializeEx() before SDL_Init()
    */

    /* On Xbox, there's no need to call CoInitializeEx (and it's not implemented) */
    return S_OK;
}

void WIN_CoUninitialize(void)
{
}

HRESULT
WIN_RoInitialize(void)
{
    return S_OK;
}

void WIN_RoUninitialize(void)
{

}

BOOL WIN_IsWindowsVistaOrGreater(void)
{
    return TRUE;
}

BOOL WIN_IsWindows7OrGreater(void)
{
    return TRUE;
}

BOOL WIN_IsWindows8OrGreater(void)
{
    return TRUE;
}

/*
WAVExxxCAPS gives you 31 bytes for the device name, and just truncates if it's
longer. However, since WinXP, you can use the WAVExxxCAPS2 structure, which
will give you a name GUID. The full name is in the Windows Registry under
that GUID, located here: HKLM\System\CurrentControlSet\Control\MediaCategories

Note that drivers can report GUID_NULL for the name GUID, in which case,
Windows makes a best effort to fill in those 31 bytes in the usual place.
This info summarized from MSDN:

http://web.archive.org/web/20131027093034/http://msdn.microsoft.com/en-us/library/windows/hardware/ff536382(v=vs.85).aspx

Always look this up in the registry if possible, because the strings are
different! At least on Win10, I see "Yeti Stereo Microphone" in the
Registry, and a unhelpful "Microphone(Yeti Stereo Microph" in winmm. Sigh.

(Also, DirectSound shouldn't be limited to 32 chars, but its device enum
has the same problem.)

WASAPI doesn't need this. This is just for DirectSound/WinMM.
*/
char *WIN_LookupAudioDeviceName(const WCHAR *name, const GUID *guid)
{
    return WIN_StringToUTF8W(name); /* No registry access on WinRT/UWP and Xbox, go with what we've got. */
}

BOOL WIN_IsEqualGUID(const GUID *a, const GUID *b)
{
    return SDL_memcmp(a, b, sizeof(*a)) == 0;
}

BOOL WIN_IsEqualIID(REFIID a, REFIID b)
{
    return SDL_memcmp(a, b, sizeof(*a)) == 0;
}

void WIN_RECTToRect(const RECT *winrect, SDL_Rect *sdlrect)
{
    sdlrect->x = winrect->left;
    sdlrect->w = (winrect->right - winrect->left) + 1;
    sdlrect->y = winrect->top;
    sdlrect->h = (winrect->bottom - winrect->top) + 1;
}

void WIN_RectToRECT(const SDL_Rect *sdlrect, RECT *winrect)
{
    winrect->left = sdlrect->x;
    winrect->right = sdlrect->x + sdlrect->w - 1;
    winrect->top = sdlrect->y;
    winrect->bottom = sdlrect->y + sdlrect->h - 1;
}

BOOL WIN_IsRectEmpty(const RECT *rect)
{
    /* Calculating this manually because UWP and Xbox do not support Win32 IsRectEmpty. */
    return (rect->right <= rect->left) || (rect->bottom <= rect->top);
}

/* vi: set ts=4 sw=4 expandtab: */
