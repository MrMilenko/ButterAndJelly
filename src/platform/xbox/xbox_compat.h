/* SPDX-License-Identifier: GPL-2.0-or-later */
/* xbox_compat.h: force-included into every file of the original Xbox build.
 *
 * libc++ needs _MSC_VER at 1900 or later, which makes third party code reach
 * for CRT functions Microsoft added long after this console's. Supply them
 * here rather than patching each library.
 */

#ifndef BJ_XBOX_COMPAT_H
#define BJ_XBOX_COMPAT_H

#include <stdio.h>

/* MSVC 2005. stb_image_write takes this path once _MSC_VER says 1900. */
#ifdef __cplusplus
extern "C" {
#endif

static __inline int bj_fopen_s(FILE** f, const char* name, const char* mode)
{
    if (!f) return 1;
    *f = fopen(name, mode);
    return *f ? 0 : 1;
}

#ifdef __cplusplus
}
#endif

#define fopen_s bj_fopen_s

#endif /* BJ_XBOX_COMPAT_H */
