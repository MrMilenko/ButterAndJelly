/*
 * Compatibility stubs for both Xboxes: POSIX and older FFmpeg names that
 * neither XDK provides, plus hardware thread placement on the 360.
 */

#include <string.h>
#include "libavutil/random_seed.h"

/* POSIX strcasecmp - map to MSVC _stricmp */
int strcasecmp(const char *s1, const char *s2)
{
    return _stricmp(s1, s2);
}

/* Old FFmpeg internal name for av_get_random_seed */
unsigned int ff_random_get_seed(void)
{
    return av_get_random_seed();
}

/* Hardware thread placement on the 360. See xenon_hwthread.h for why this is
 * needed at all: without it every decoder worker shares one hardware thread
 * with the main thread, and H.264 at 720p decodes at about three quarters of
 * realtime.
 *
 * Cores 1 and 2 (hardware threads 2-5) are handed out in turn. Core 0 is left
 * alone: the main thread renders there and the system takes its share of it. */
#ifdef _XENON
#include "xenon_hwthread.h"
#endif

void BJ_PinWorkerThread(void *hThread)
{
#ifdef _XENON
    static const unsigned long kWorkerThreads[] = { 2, 4, 3, 5 };
    static unsigned long next = 0;

    if (!hThread) {
        return;
    }
    XSetThreadProcessor(hThread, kWorkerThreads[next & 3]);
    ++next;
#else
    /* One core, nothing to place. */
    (void)hThread;
#endif
}
