#ifndef BX_RSYNC_PORTABLE_UTIL_H
#define BX_RSYNC_PORTABLE_UTIL_H

#include <stddef.h>
#include <stdint.h>

#define HN_AUTOSCALE (-1)
#define HN_DECIMAL 0x01
#define HN_NOSPACE 0x02
#define HN_DIVISOR_1000 0x04
#define HN_IEC_PREFIXES 0x08

int bx_rsync_humanize_number(char *, size_t, int64_t, const char *, int, int);
#define humanize_number bx_rsync_humanize_number

#endif
