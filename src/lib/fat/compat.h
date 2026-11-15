/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef BX_FAT_COMPAT_H
#define BX_FAT_COMPAT_H

#include <sys/param.h>
#include <stdint.h>
#include <limits.h>
#include "io.h"
#include "names.h"

#ifndef __RCSID
#define __RCSID(s) _Static_assert(1, s)
#endif
#ifndef __dead2
#define __dead2 __attribute__((noreturn))
#endif
#ifndef __printflike
#define __printflike(f, a) __attribute__((format(printf, f, a)))
#endif
#define BX_FAT_UNUSED __attribute__((unused))
#ifndef __packed
#define __packed __attribute__((packed))
#endif
#ifndef __nonstring
#if defined(__has_attribute) && __has_attribute(nonstring)
#define __nonstring __attribute__((nonstring))
#else
#define __nonstring
#endif
#endif
#ifndef nitems
#define nitems(a) (sizeof(a) / sizeof((a)[0]))
#endif
#ifndef powerof2
#define powerof2(x) (((x) & ((x) - 1)) == 0)
#endif
#ifndef roundup2
#define roundup2(x, y) (((x) + (y) - 1) & ~((y) - 1))
#endif

static inline uint16_t bx_fat_le16dec(const void *p)
{
    const uint8_t *b = p;
    return (uint16_t)(b[0] | (uint16_t)b[1] << 8);
}
static inline uint32_t bx_fat_le32dec(const void *p)
{
    const uint8_t *b = p;
    return b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}
static inline void bx_fat_le16enc(void *p, uint16_t v)
{
    uint8_t *b = p;
    b[0] = v;
    b[1] = v >> 8;
}
static inline void bx_fat_le32enc(void *p, uint32_t v)
{
    uint8_t *b = p;
    b[0] = v;
    b[1] = v >> 8;
    b[2] = v >> 16;
    b[3] = v >> 24;
}
#define le16dec bx_fat_le16dec
#define le32dec bx_fat_le32dec
#define le16enc bx_fat_le16enc
#define le32enc bx_fat_le32enc

#endif
