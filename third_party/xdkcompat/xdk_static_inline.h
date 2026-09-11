// SPDX-License-Identifier: GPL-2.0-or-later

// The XDK defines helpers like HRESULT_FROM_WIN32 in its headers, marked
// __inline or __forceinline. Under -fgnu89-inline, which ffmpeg needs, clang
// emits a real definition of each in every file that includes windows.h, and
// any two of them then collide.
//
// Force included ahead of the XDK headers. A macro is not re-expanded inside
// its own expansion, so these resolve once.

#pragma once

#define __inline      static __inline
#define __forceinline static __forceinline
