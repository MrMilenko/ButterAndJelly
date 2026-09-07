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

/* This is an include file for windows.h with the SDL build settings */

#ifndef _INCLUDED_XBOX_H
#define _INCLUDED_XBOX_H

#include <xtl.h>
#include "SDL_xbox_BaseTyps.h" /* for REFIID with broken mingw.org headers */

/* Routines to convert from UTF8 to native Windows text */
#if UNICODE
#define XBOX_StringToUTF8(S) SDL_iconv_string("UTF-8", "UTF-16LE", (char *)(S), (SDL_wcslen(S)+1)*sizeof(WCHAR))
#define XBOX_UTF8ToString(S) (WCHAR *)SDL_iconv_string("UTF-16LE", "UTF-8", (char *)(S), SDL_strlen(S)+1)
#else
/* !!! FIXME: UTF8ToString() can just be a SDL_strdup() here. */
#define XBOX_StringToUTF8(S) SDL_iconv_string("UTF-8", "ASCII", (char *)(S), (SDL_strlen(S)+1))
#define XBOX_UTF8ToString(S) SDL_iconv_string("ASCII", "UTF-8", (char *)(S), SDL_strlen(S)+1)
#endif

/* Sets an error message based on a given HRESULT */
extern int XBOX_SetErrorFromHRESULT(const char* prefix, HRESULT hr);

/* Sets an error message based on GetLastError(). Always return -1. */
extern int XBOX_SetError(const char* prefix);

#ifdef __XBOX__
/* Xbox-specific functions */
VOID WINAPI HalReturnToFirmware(unsigned int value);
VOID WINAPI HalWriteSMBusValue(UCHAR devAddress, UCHAR offset, UCHAR writeDW, DWORD data);
VOID WINAPI HalReadSMBusValue(UCHAR devAddress, UCHAR offset, UCHAR readDW, DWORD* pData);
#endif

#endif /* _INCLUDED_XBOX_H */

/* vi: set ts=4 sw=4 expandtab: */
