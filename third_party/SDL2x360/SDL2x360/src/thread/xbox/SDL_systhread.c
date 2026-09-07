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

#if SDL_THREAD_XBOX

#include "SDL_hints.h"
#include "SDL_thread.h"
#include "../SDL_thread_c.h"
#include "../SDL_systhread.h"
#include "SDL_systhread_c.h"

static DWORD thread_local_storage = TLS_OUT_OF_INDEXES;
static SDL_bool generic_local_storage = SDL_FALSE;

void SDL_SYS_InitTLSData(void)
{
	if (thread_local_storage == TLS_OUT_OF_INDEXES && !generic_local_storage) {
		thread_local_storage = TlsAlloc();
		if (thread_local_storage == TLS_OUT_OF_INDEXES) {
			SDL_Generic_InitTLSData();
			generic_local_storage = SDL_TRUE;
		}
	}
}

void SDL_SYS_QuitTLSData(void)
{
	if (generic_local_storage) {
		SDL_Generic_QuitTLSData();
		generic_local_storage = SDL_FALSE;
	}
	else {
		if (thread_local_storage != TLS_OUT_OF_INDEXES) {
			TlsFree(thread_local_storage);
			thread_local_storage = TLS_OUT_OF_INDEXES;
		}
	}
}

static DWORD WINAPI RunThread(LPVOID data)
{
    SDL_Thread* thread = (SDL_Thread*)data;   /*  this is the SDL thread object */
    SDL_RunThread(thread);                     /* runs user func with user data */
    return 0;
}

int SDL_SYS_CreateThread(SDL_Thread* thread)
{
    DWORD threadnum;

    /* ?? pass the SDL_Thread*, NOT args */
    thread->handle = CreateThread(NULL,
        0,
        RunThread,
        thread,          /* <- this is the important change */
        0,
        &threadnum);

    if (thread->handle == NULL) {
        SDL_SetError("Not enough resources to create thread");
        return -1;
    }

    thread->threadid = (SDL_threadID)threadnum;
    return 0;
}

void SDL_SYS_SetupThread(const char* name)
{
    (void)name;
}

SDL_threadID SDL_ThreadID(void)
{
    return (SDL_threadID)GetCurrentThreadId();
}

int SDL_SYS_SetThreadPriority(SDL_ThreadPriority priority)
{
    int value;

    if (priority == SDL_THREAD_PRIORITY_LOW) {
        value = THREAD_PRIORITY_LOWEST;
    }
    else if (priority == SDL_THREAD_PRIORITY_HIGH) {
        value = THREAD_PRIORITY_HIGHEST;
    }
    else if (priority == SDL_THREAD_PRIORITY_TIME_CRITICAL) {
        value = THREAD_PRIORITY_TIME_CRITICAL;
    }
    else {
        value = THREAD_PRIORITY_NORMAL;
    }

    if (!SetThreadPriority(GetCurrentThread(), value)) {
        /* you used XBOX_SetError() but didn’t show it; fall back to SDL_SetError */
        SDL_SetError("SetThreadPriority() failed");
        return -1;
    }
    return 0;
}

void SDL_SYS_WaitThread(SDL_Thread* thread)
{
    WaitForSingleObject(thread->handle, INFINITE);
    CloseHandle(thread->handle);
}

void SDL_SYS_DetachThread(SDL_Thread* thread)
{
    CloseHandle(thread->handle);
}

#endif /* SDL_THREAD_XBOX */
