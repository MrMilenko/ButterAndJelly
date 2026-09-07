// SPDX-License-Identifier: GPL-2.0-or-later
/* oxdk360_shim.c: the handful of symbols this toolchain owes the XDK's headers.
 *
 * Every OXDK360 port carries a file like this. Keep it small: anything that
 * grows past a few compiler intrinsics belongs in OXDK360 itself.
 */

/* MSVC provides _ReadWriteBarrier as a compiler intrinsic that emits no code,
   only constrains reordering. clang has no such symbol, and SDL's atomics
   reference it, so supply a real compiler barrier. */
void _ReadWriteBarrier(void)
{
    __asm__ __volatile__("" ::: "memory");
}
