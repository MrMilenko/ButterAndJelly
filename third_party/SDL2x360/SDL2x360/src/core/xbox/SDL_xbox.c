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

#if defined(__WIN32__) || defined(__WINRT__) ||  defined(__XBOX__)

#include "SDL_xbox.h"
#include "SDL_error.h"
#include "SDL_assert.h"

/* Sets an error message based on an HRESULT */
int
XBOX_SetErrorFromHRESULT(const char* prefix, HRESULT hr)
{
	// TODO
	/*  TCHAR buffer[1024];
		char *message;
		FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM, NULL, hr, 0,
					  buffer, SDL_arraysize(buffer), NULL);
		message = XBOX_StringToUTF8(buffer);
		SDL_SetError("%s%s%s", prefix ? prefix : "", prefix ? ": " : "", message);
		SDL_free(message);
	*/
	SDL_SetError("ERROR: XBOX_SetErrorFromHRESULT");
	return -1;
}

/* Sets an error message based on GetLastError() */
int
XBOX_SetError(const char* prefix)
{
	return XBOX_SetErrorFromHRESULT(prefix, GetLastError());
}

// Workaround for MSVC trying to use SSE2 ftol on our poor Pentium 3
// Declare a regular _ftol() function that is present.
long cdecl _ftol(double);

// Define _ftol2_sse() to be the regular one.
#undef _ftol2_sse

long cdecl _ftol2_sse(double f) {
	return _ftol(f);
}

#endif /* __WIN32__ || __WINRT__  || __XBOX__*/

/* vi: set ts=4 sw=4 expandtab: */