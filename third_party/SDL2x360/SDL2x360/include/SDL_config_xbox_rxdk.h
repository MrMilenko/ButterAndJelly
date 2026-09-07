#pragma once

#define SDL_PLATFORM_XBOX_RXDK 1

/* --- Core/CRT --- */
#define STDC_HEADERS 1
#define HAVE_STDARG_H 1
#define HAVE_STDDEF_H 1
#define SDL_BYTEORDER SDL_LIL_ENDIAN
#define SIZEOF_VOIDP 4

/* --- No <stdint.h>/<inttypes.h> on OG XDK --- */
#undef HAVE_INTTYPES_H
#ifndef _SDL_XDK_STDINT_TYPES_
#define _SDL_XDK_STDINT_TYPES_
typedef   signed __int8   int8_t;
typedef unsigned __int8   uint8_t;
typedef   signed __int16  int16_t;
typedef unsigned __int16  uint16_t;
typedef   signed __int32  int32_t;
typedef unsigned __int32  uint32_t;
typedef   signed __int64  int64_t;
typedef unsigned __int64  uint64_t;
#ifndef _UINTPTR_T_DEFINED
typedef unsigned int      uintptr_t;
#define _UINTPTR_T_DEFINED
#endif
#endif
#ifndef SDL_HAS_64BIT_TYPE
#define SDL_HAS_64BIT_TYPE 1
#endif

/* --- C library funcs present on XDK --- */
#define HAVE_MALLOC   1
#define HAVE_CALLOC   1
#define HAVE_REALLOC  1
#define HAVE_FREE     1
#define HAVE_QSORT    1
#define HAVE_ABS      1
#define HAVE_MEMSET   1
#define HAVE_MEMCPY   1
#define HAVE_MEMMOVE  1
#define HAVE_MEMCMP   1
#define HAVE_STRLEN   1
#define HAVE_STRCHR   1
#define HAVE_STRRCHR  1
#define HAVE_STRSTR   1
#define HAVE_STRTOL   1
#define HAVE_STRTOUL  1
#define HAVE_STRTOD   1
#define HAVE_ATOI     1
#define HAVE_ATOF     1
#define HAVE_STRCMP   1
#define HAVE_STRNCMP  1
#define HAVE__STRICMP  1
#define HAVE__STRNICMP 1

/* --- Math --- */
#define HAVE_COS   1
#define HAVE_SIN   1
#define HAVE_TAN   1
#define HAVE_ACOS  1
#define HAVE_ASIN  1
#define HAVE_ATAN  1
#define HAVE_ATAN2 1
#define HAVE_FABS  1
#define HAVE_FLOOR 1
#define HAVE_FMOD  1
#define HAVE_LOG   1
#define HAVE_LOG10 1
#define HAVE_POW   1
#define HAVE_SQRT  1
#if !defined(_MSC_VER) || defined(_USE_MATH_DEFINES)
#define HAVE_M_PI 1
#endif

#define HAVE_STDIO_H 1

/* --- Disable dynamic loading on XDK --- */
#define SDL_LOADSO_DISABLED 1

/* --- Subsystems we’re not using (for now) --- */
#define SDL_HAPTIC_DISABLED 1
#define SDL_SENSOR_DISABLED 1
#define SDL_POWER_DISABLED  1

/* XBOX Video driver */
#define SDL_VIDEO_DRIVER_XBOX  1

/* XBOX Timer driver */
#define SDL_TIMER_XBOX  1

/* === Select SDL backends by the names SDL expects ===
   We are using Windows-family backends that work with XDK’s xtl.h.
   (You already patched SDL_windows.h to include <xtl.h> under _XBOX.) */
#define SDL_AUDIO_DRIVER_DSOUND     1
#define SDL_VIDEO_RENDER_D3D        1   /* renderer support flag */

#define SDL_FILESYSTEM_XBOX_RXDK  1

#define SDL_THREAD_XBOX  1

#define SDL_MUTEX_WINDOWS           1
#define SDL_SEMAPHORE_WINDOWS       1
#define SDL_TIMER_WINDOWS           1

/* Joystick: keep using your custom driver if you have one, otherwise Windows */
#ifndef SDL_JOYSTICK_XINPUT
/* Use the XBOX joystick driver */
#define SDL_JOYSTICK_XBOX 1
#endif

/* Optional assembly (x86) */
#ifndef _WIN64
#define SDL_ASSEMBLY_ROUTINES 1
#endif
