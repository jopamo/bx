/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 1998 Robert Nordier
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR(S) ``AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR(S) BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
 * IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "lib/fat/compat.h"
#include <sys/param.h>
#include <sys/stat.h>
#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <inttypes.h>
#include <paths.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lib/fat/format/mkfs_msdos.h"
#include "applets.h"
#include "dos.h"
#include "lib/checked_math.h"
#include "lib/option_scan.h"

#define	argto1(arg, lo, msg)  argtou(arg, lo, 0xff, msg)
#define	argto2(arg, lo, msg)  argtou(arg, lo, 0xffff, msg)
#define	argto4(arg, lo, msg)  argtou(arg, lo, 0xffffffff, msg)
#define	argtox(arg, lo, msg)  argtou(arg, lo, UINT_MAX, msg)

static u_int argtou(const char *, u_int, u_int, const char *);
static off_t argtooff(const char *, const char *);
static void usage(void) __dead2;

static time_t
get_tstamp(const char *b)
{
    struct stat st;
    char *eb;
    time_t timestamp;

    if (stat(b, &st) != -1)
        return (time_t)st.st_mtime;

    errno = 0;
    intmax_t l = strtoimax(b, &eb, 0);
    if (b == eb || *eb || errno || !bx_checked_intmax_to_time_t(l, &timestamp))
        errx(EXIT_FAILURE, "Can't parse timestamp '%s'", b);
    return timestamp;
}

/*
 * Construct a FAT12, FAT16, or FAT32 file system.
 */
int
bx_mkfs_fat_main(int argc, char *argv[])
{
    const char *name = strrchr(argv[0], '/');
    name = name ? name + 1 : argv[0];
    if (strcmp(name, "newfs_msdos"))
        return bx_fat_mkfs_dos(argc, argv);
    static const struct bx_option_spec specs[] = {
        {'@', '@', NULL, BX_OPTION_REQUIRED_VALUE},
        {'N', 'N', NULL, BX_OPTION_NO_VALUE},
        {'A', 'A', NULL, BX_OPTION_NO_VALUE},
        {'B', 'B', NULL, BX_OPTION_REQUIRED_VALUE},
        {'C', 'C', NULL, BX_OPTION_REQUIRED_VALUE},
        {'F', 'F', NULL, BX_OPTION_REQUIRED_VALUE},
        {'I', 'I', NULL, BX_OPTION_REQUIRED_VALUE},
        {'L', 'L', NULL, BX_OPTION_REQUIRED_VALUE},
        {'O', 'O', NULL, BX_OPTION_REQUIRED_VALUE},
        {'S', 'S', NULL, BX_OPTION_REQUIRED_VALUE},
        {'a', 'a', NULL, BX_OPTION_REQUIRED_VALUE},
        {'b', 'b', NULL, BX_OPTION_REQUIRED_VALUE},
        {'c', 'c', NULL, BX_OPTION_REQUIRED_VALUE},
        {'e', 'e', NULL, BX_OPTION_REQUIRED_VALUE},
        {'f', 'f', NULL, BX_OPTION_REQUIRED_VALUE},
        {'h', 'h', NULL, BX_OPTION_REQUIRED_VALUE},
        {'i', 'i', NULL, BX_OPTION_REQUIRED_VALUE},
        {'k', 'k', NULL, BX_OPTION_REQUIRED_VALUE},
        {'m', 'm', NULL, BX_OPTION_REQUIRED_VALUE},
        {'n', 'n', NULL, BX_OPTION_REQUIRED_VALUE},
        {'o', 'o', NULL, BX_OPTION_REQUIRED_VALUE},
        {'r', 'r', NULL, BX_OPTION_REQUIRED_VALUE},
        {'s', 's', NULL, BX_OPTION_REQUIRED_VALUE},
        {'T', 'T', NULL, BX_OPTION_REQUIRED_VALUE},
        {'u', 'u', NULL, BX_OPTION_REQUIRED_VALUE},
    };
    struct msdos_options o = {0};
    const char *operands[2] = {0};
    size_t count = 0;
    struct bx_option_cursor cursor;

    bx_option_cursor_init(&cursor, argv, 1, (size_t)argc, BX_OPTION_ORDERED,
                          specs, sizeof(specs) / sizeof(specs[0]));
    for (;;) {
        struct bx_option_result result = bx_option_next(&cursor);
        if (result.event == BX_OPTION_END)
            break;
        if (result.event == BX_OPTION_OPERAND) {
            if (count == sizeof(operands) / sizeof(operands[0]))
                usage();
            operands[count++] = result.token;
            continue;
        }
        if (result.event != BX_OPTION_MATCH)
            usage();
        const char *value = result.value;
        switch (result.spec->id) {
	case '@':
	    o.offset = argtooff(value, "offset");
	    break;
	case 'N':
	    o.no_create = 1;
	    break;
	case 'A':
	    o.align = true;
	    break;
	case 'B':
	    o.bootstrap = value;
	    break;
	case 'C':
	    o.create_size = argtooff(value, "create size");
	    if (o.create_size == 0)
		errx(1, "create size must be positive");
	    break;
	case 'F':
	    if (strcmp(value, "12") &&
		strcmp(value, "16") &&
		strcmp(value, "32"))
		errx(1, "%s: bad FAT type", value);
	    o.fat_type = atoi(value);
	    break;
	case 'I':
	    o.volume_id = argto4(value, 0, "volume ID");
	    o.volume_id_set = 1;
	    break;
	case 'L':
	    o.volume_label = value;
	    break;
	case 'O':
	    o.OEM_string = value;
	    break;
	case 'S':
	    o.bytes_per_sector = argto2(value, 1, "bytes/sector");
	    break;
	case 'a':
	    o.sectors_per_fat = argto4(value, 1, "sectors/FAT");
	    break;
	case 'b':
	    o.block_size = argtox(value, 1, "block size");
	    o.sectors_per_cluster = 0;
	    break;
	case 'c':
	    o.sectors_per_cluster = argto1(value, 1, "sectors/cluster");
	    o.block_size = 0;
	    break;
	case 'e':
	    o.directory_entries = argto2(value, 1, "directory entries");
	    break;
	case 'f':
	    o.floppy = value;
	    break;
	case 'h':
	    o.drive_heads = argto2(value, 1, "drive heads");
	    break;
	case 'i':
	    o.info_sector = argto2(value, 1, "info sector");
	    break;
	case 'k':
	    o.backup_sector = argto2(value, 1, "backup sector");
	    break;
	case 'm':
	    o.media_descriptor = argto1(value, 0, "media descriptor");
	    o.media_descriptor_set = 1;
	    break;
	case 'n':
	    o.num_FAT = argto1(value, 1, "number of FATs");
	    break;
	case 'o':
	    o.hidden_sectors = argto4(value, 0, "hidden sectors");
	    o.hidden_sectors_set = 1;
	    break;
	case 'r':
	    o.reserved_sectors = argto2(value, 1, "reserved sectors");
	    break;
	case 's':
	    o.size = argto4(value, 1, "file system size");
	    break;
	case 'T':
	    o.timestamp_set = 1;
	    o.timestamp = get_tstamp(value);
	    break;
	case 'u':
	    o.sectors_per_track = argto2(value, 1, "sectors/track");
	    break;
	default:
	    usage();
	}
    }
    if (count == 0)
	usage();
    if (o.align && o.reserved_sectors)
	errx(1, "align (-A) is incompatible with -r");
    exit(!!mkfs_msdos(operands[0], operands[1], &o));
}

/*
 * Convert and check a numeric option argument.
 */
static u_int
argtou(const char *arg, u_int lo, u_int hi, const char *msg)
{
    char *s;
    u_long x;

    while (isspace((unsigned char)*arg))
	arg++;
    errno = 0;
    x = strtoul(arg, &s, 0);
    if (*arg == '-' || errno || s == arg || *s || x < lo || x > hi)
	errx(1, "%s: bad %s", arg, msg);
    return x;
}

/*
 * Same for off_t, with optional skmgpP suffix
 */
static off_t
argtooff(const char *arg, const char *msg)
{
    char *s;
    long long x;
    unsigned shift = 0;

    errno = 0;
    x = strtoll(arg, &s, 0);
    /* allow at most one extra char */
    if (errno || s == arg || x < 0 || (s[0] && s[1]) )
	errx(1, "%s: bad %s", arg, msg);
    if (*s) {	/* the extra char is the multiplier */
	switch (*s) {
	default:
	    errx(1, "%s: bad %s", arg, msg);
	    /* notreached */

	case 's':		/* sector */
	case 'S':
	    shift = 9;
	    break;

	case 'k':		/* kilobyte */
	case 'K':
	    shift = 10;
	    break;

	case 'm':		/* megabyte */
	case 'M':
	    shift = 20;
	    break;

	case 'g':		/* gigabyte */
	case 'G':
	    shift = 30;
	    break;

	case 'p':		/* partition start */
	case 'P':
	case 'l':		/* partition length */
	case 'L':
	    errx(1, "%s: not supported yet %s", arg, msg);
	    /* notreached */
	}
    }
    off_t result;
    if ((uint64_t)x > (uint64_t)INT64_MAX >> shift ||
        __builtin_mul_overflow(x, (uint64_t)1 << shift, &result))
	errx(1, "%s: bad %s", arg, msg);
    return result;
}

/*
 * Print usage message.
 */
static void
usage(void)
{
    fprintf(stderr,
	    "usage: newfs_msdos [ -options ] special [disktype]\n");
    fprintf(stderr, "where the options are:\n");
static struct {
    char o;
    const char *h;
} opts[] = {
#define AOPT(_opt, _type, _name, _min, _desc) { _opt, _desc },
ALLOPTS
#undef AOPT
    };
    for (size_t i = 0; i < nitems(opts); i++)
	fprintf(stderr, "\t-%c %s\n", opts[i].o, opts[i].h);
    exit(1);
}
