/*
 * Spreading threads across the Xbox 360's hardware threads.
 *
 * The console has three physical cores of two hardware threads each,
 * numbered 0-1, 2-3 and 4-5. A thread created here does NOT get placed on an
 * idle one: it runs wherever its creator was running and stays there. Since
 * everything descends from main, everything ends up on hardware thread 0 --
 * six ways of parallelism collapsed onto one, silently, with no error and no
 * symptom other than being slow.
 *
 * XSetThreadProcessor is what moves them. It is exported from xapilib but the
 * headers in this XDK extract do not declare it, so it is declared here.
 */
#ifndef BJ_XENON_HWTHREAD_H
#define BJ_XENON_HWTHREAD_H

#ifdef _XBOX

#ifdef __cplusplus
extern "C" {
#endif

unsigned long XSetThreadProcessor(void *hThread, unsigned long dwHardwareThread);

/* Put one worker on the next hardware thread of cores 1 and 2, in turn.
 * Core 0 is left to the main thread and the system. */
void BJ_PinWorkerThread(void *hThread);

#ifdef __cplusplus
}
#endif

#endif /* _XBOX */
#endif /* BJ_XENON_HWTHREAD_H */
