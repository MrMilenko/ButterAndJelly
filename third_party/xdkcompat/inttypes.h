/* inttypes.h for the Xbox 360 XDK.
 *
 * The XDK's CRT predates C99 and clang's own copy refuses to serve MSVC, so
 * this supplies what portable C actually reaches for: the fixed-width types,
 * and the PRI and SCN format macros that FFmpeg uses throughout.
 *
 * The widths are the ones this ABI uses -- 32-bit long, 64-bit long long --
 * so the 64-bit conversions spell themselves "ll", not "l".
 *
 * A caution that is not obvious: printf on this console cannot actually carry
 * a 64-bit argument, because clang splits one across two argument slots where
 * the XDK's own compiler passes one register. PRId64 will compile and produce
 * nonsense. See src/core/text.h. It is here so FFmpeg's logging compiles, not
 * because it works.
 */
#ifndef OXDK360_COMPAT_INTTYPES_H
#define OXDK360_COMPAT_INTTYPES_H

#include <stdint.h>

#define PRId8    "d"
#define PRId16   "d"
#define PRId32   "d"
#define PRId64   "lld"
#define PRIi8    "i"
#define PRIi16   "i"
#define PRIi32   "i"
#define PRIi64   "lli"
#define PRIu8    "u"
#define PRIu16   "u"
#define PRIu32   "u"
#define PRIu64   "llu"
#define PRIo8    "o"
#define PRIo16   "o"
#define PRIo32   "o"
#define PRIo64   "llo"
#define PRIx8    "x"
#define PRIx16   "x"
#define PRIx32   "x"
#define PRIx64   "llx"
#define PRIX8    "X"
#define PRIX16   "X"
#define PRIX32   "X"
#define PRIX64   "llX"

#define PRIdMAX  PRId64
#define PRIuMAX  PRIu64
#define PRIxMAX  PRIx64
#define PRIdPTR  PRId32
#define PRIuPTR  PRIu32
#define PRIxPTR  PRIx32

#define SCNd8    "hhd"
#define SCNd16   "hd"
#define SCNd32   "d"
#define SCNd64   "lld"
#define SCNu8    "hhu"
#define SCNu16   "hu"
#define SCNu32   "u"
#define SCNu64   "llu"
#define SCNx8    "hhx"
#define SCNx16   "hx"
#define SCNx32   "x"
#define SCNx64   "llx"

#endif /* OXDK360_COMPAT_INTTYPES_H */
