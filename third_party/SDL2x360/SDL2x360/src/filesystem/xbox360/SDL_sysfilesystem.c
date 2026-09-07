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

#ifdef SDL_FILESYSTEM_XBOX_360

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* System dependent filesystem routines                                */

#include "../../core/xbox/SDL_xbox.h"
#include "SDL_filesystem.h"

/* These return a string the caller releases with SDL_free, so they must be
   heap allocated -- returning a local array handed out a dangling stack pointer
   and freeing it corrupted the heap. "D:\" is also the Original Xbox DVD
   device; on the 360 the title's files are under "game:\". */

char*
SDL_GetBasePath(void)
{
	return SDL_strdup("game:\\");
}

char*
SDL_GetPrefPath(const char* org, const char* app)
{
	(void) org;
	(void) app;
	/* No per-user profile directory; the title's own directory is writable
	   when launched from the hard drive. */
	return SDL_strdup("game:\\");
}

#endif /* SDL_FILESYSTEM_XBOX_RXDK */

/* vi: set ts=4 sw=4 expandtab: */
