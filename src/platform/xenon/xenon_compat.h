// SPDX-License-Identifier: GPL-2.0-or-later
/* xenon_compat.h: force-included into every C file of the 360 build.
 *
 * The XDK's headers are Microsoft's from 2006 and predate parts of C99 that
 * portable code takes for granted. Rather than patch each library, declare the
 * missing pieces once, here.
 */

#ifndef BJ_XENON_COMPAT_H
#define BJ_XENON_COMPAT_H

/* C99. The XDK's vadefs.h has va_start, va_arg and va_end but not this, and
   SDL calls it in three places. On this ABI a va_list is a single char *
   pointing into the caller's parameter save area, so copying it is a plain
   assignment, which is exactly what the C standard's own fallback for
   register-free ABIs does. Without it the call survives compilation as an
   implicit declaration and only fails at link, naming a symbol nothing
   defines. */
#ifndef va_copy
#define va_copy(dst, src) ((dst) = (src))
#endif

#endif /* BJ_XENON_COMPAT_H */
