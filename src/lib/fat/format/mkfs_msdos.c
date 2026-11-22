/*
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
#include <sys/time.h>

#include <assert.h>
#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <paths.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "mkfs_msdos.h"
#include "lib/fd_ops.h"

#define	MAXU16	  0xffff	/* maximum unsigned 16-bit quantity */
#define	BPN	  4		/* bits per nibble */
#define	NPB	  2		/* nibbles per byte */

#define	DOSMAGIC  0xaa55	/* DOS magic number */
#define	MINBPS	  512		/* minimum bytes per sector */
#define	MAXBPS    4096		/* maximum bytes per sector */
#define	MAXSPC	  128		/* maximum sectors per cluster */
#define	MAXNFT	  16		/* maximum number of FATs */
#define	DEFBLK	  4096		/* default block size */
#define	DEFBLK16  2048		/* default block size FAT16 */
#define	DEFRDE	  512		/* default root directory entries */
#define	RESFTE	  2		/* reserved FAT entries */
#define	MINCLS12  1U		/* minimum FAT12 clusters */
#define	MINCLS16  0xff5U	/* minimum FAT16 clusters */
#define	MINCLS32  0xfff5U	/* minimum FAT32 clusters */
#define	MAXCLS12  0xff4U	/* maximum FAT12 clusters */
#define	MAXCLS16  0xfff4U	/* maximum FAT16 clusters */
#define	MAXCLS32  0xffffff4U	/* maximum FAT32 clusters */

#define	mincls(fat)  ((fat) == 12 ? MINCLS12 :	\
		      (fat) == 16 ? MINCLS16 :	\
				    MINCLS32)

#define	maxcls(fat)  ((fat) == 12 ? MAXCLS12 :	\
		      (fat) == 16 ? MAXCLS16 :	\
				    MAXCLS32)

#define	mk1(p, x)				\
    (p) = (u_int8_t)(x)

#define	mk2(p, x)				\
    (p)[0] = (u_int8_t)(x),			\
    (p)[1] = (u_int8_t)((x) >> 010)

#define	mk4(p, x)				\
    (p)[0] = (u_int8_t)(x),			\
    (p)[1] = (u_int8_t)((x) >> 010),		\
    (p)[2] = (u_int8_t)((x) >> 020),		\
    (p)[3] = (u_int8_t)((x) >> 030)

struct bs {
    u_int8_t bsJump[3];			/* bootstrap entry point */
    u_int8_t bsOemName[8];		/* OEM name and version */
} __packed;

struct bsbpb {
    u_int8_t bpbBytesPerSec[2];		/* bytes per sector */
    u_int8_t bpbSecPerClust;		/* sectors per cluster */
    u_int8_t bpbResSectors[2];		/* reserved sectors */
    u_int8_t bpbFATs;			/* number of FATs */
    u_int8_t bpbRootDirEnts[2];		/* root directory entries */
    u_int8_t bpbSectors[2];		/* total sectors */
    u_int8_t bpbMedia;			/* media descriptor */
    u_int8_t bpbFATsecs[2];		/* sectors per FAT */
    u_int8_t bpbSecPerTrack[2];		/* sectors per track */
    u_int8_t bpbHeads[2];		/* drive heads */
    u_int8_t bpbHiddenSecs[4];		/* hidden sectors */
    u_int8_t bpbHugeSectors[4];		/* big total sectors */
} __packed;

struct bsxbpb {
    u_int8_t bpbBigFATsecs[4];		/* big sectors per FAT */
    u_int8_t bpbExtFlags[2];		/* FAT control flags */
    u_int8_t bpbFSVers[2];		/* file system version */
    u_int8_t bpbRootClust[4];		/* root directory start cluster */
    u_int8_t bpbFSInfo[2];		/* file system info sector */
    u_int8_t bpbBackup[2];		/* backup boot sector */
    u_int8_t bpbReserved[12];		/* reserved */
} __packed;

struct bsx {
    u_int8_t exDriveNumber;		/* drive number */
    u_int8_t exReserved1;		/* reserved */
    u_int8_t exBootSignature;		/* extended boot signature */
    u_int8_t exVolumeID[4];		/* volume ID number */
    u_int8_t exVolumeLabel[11];		/* volume label */
    u_int8_t exFileSysType[8];		/* file system type */
} __packed;

struct de {
    u_int8_t deName[11];		/* name and extension */
    u_int8_t deAttributes;		/* attributes */
    u_int8_t rsvd[10];			/* reserved */
    u_int8_t deMTime[2];		/* last-modified time */
    u_int8_t deMDate[2];		/* last-modified date */
    u_int8_t deStartCluster[2];		/* starting cluster */
    u_int8_t deFileSize[4];		/* size */
} __packed;

struct bpb {
    u_int bpbBytesPerSec;		/* bytes per sector */
    u_int bpbSecPerClust;		/* sectors per cluster */
    u_int bpbResSectors;		/* reserved sectors */
    u_int bpbFATs;			/* number of FATs */
    u_int bpbRootDirEnts;		/* root directory entries */
    u_int bpbSectors;			/* total sectors */
    u_int bpbMedia;			/* media descriptor */
    u_int bpbFATsecs;			/* sectors per FAT */
    u_int bpbSecPerTrack;		/* sectors per track */
    u_int bpbHeads;			/* drive heads */
    u_int bpbHiddenSecs;		/* hidden sectors */
    u_int bpbHugeSectors; 		/* big total sectors */
    u_int bpbBigFATsecs; 		/* big sectors per FAT */
    u_int bpbRootClust; 		/* root directory start cluster */
    u_int bpbFSInfo; 			/* file system info sector */
    u_int bpbBackup; 			/* backup boot sector */
};

#define	BPBGAP 0, 0, 0, 0, 0, 0

static struct {
    const char *name;
    struct bpb bpb;
} const stdfmt[] = {
    {"160",  {512, 1, 1, 2,  64,  320, 0xfe, 1,  8, 1, BPBGAP}},
    {"180",  {512, 1, 1, 2,  64,  360, 0xfc, 2,  9, 1, BPBGAP}},
    {"320",  {512, 2, 1, 2, 112,  640, 0xff, 1,  8, 2, BPBGAP}},
    {"360",  {512, 2, 1, 2, 112,  720, 0xfd, 2,  9, 2, BPBGAP}},
    {"640",  {512, 2, 1, 2, 112, 1280, 0xfb, 2,  8, 2, BPBGAP}},
    {"720",  {512, 2, 1, 2, 112, 1440, 0xf9, 3,  9, 2, BPBGAP}},
    {"1200", {512, 1, 1, 2, 224, 2400, 0xf9, 7, 15, 2, BPBGAP}},
    {"1232", {1024,1, 1, 2, 192, 1232, 0xfe, 2,  8, 2, BPBGAP}},
    {"1440", {512, 1, 1, 2, 224, 2880, 0xf0, 9, 18, 2, BPBGAP}},
    {"2880", {512, 2, 1, 2, 240, 5760, 0xf0, 9, 36, 2, BPBGAP}}
};

static const u_int8_t bootcode[] = {
    0xfa,			/* cli		    */
    0x31, 0xc0, 		/* xor	   ax,ax    */
    0x8e, 0xd0, 		/* mov	   ss,ax    */
    0xbc, 0x00, 0x7c,		/* mov	   sp,7c00h */
    0xfb,			/* sti		    */
    0x8e, 0xd8, 		/* mov	   ds,ax    */
    0xe8, 0x00, 0x00,		/* call    $ + 3    */
    0x5e,			/* pop	   si	    */
    0x83, 0xc6, 0x19,		/* add	   si,+19h  */
    0xbb, 0x07, 0x00,		/* mov	   bx,0007h */
    0xfc,			/* cld		    */
    0xac,			/* lodsb	    */
    0x84, 0xc0, 		/* test    al,al    */
    0x74, 0x06, 		/* jz	   $ + 8    */
    0xb4, 0x0e, 		/* mov	   ah,0eh   */
    0xcd, 0x10, 		/* int	   10h	    */
    0xeb, 0xf5, 		/* jmp	   $ - 9    */
    0x30, 0xe4, 		/* xor	   ah,ah    */
    0xcd, 0x16, 		/* int	   16h	    */
    0xcd, 0x19, 		/* int	   19h	    */
    0x0d, 0x0a,
    'N', 'o', 'n', '-', 's', 'y', 's', 't',
    'e', 'm', ' ', 'd', 'i', 's', 'k',
    0x0d, 0x0a,
    'P', 'r', 'e', 's', 's', ' ', 'a', 'n',
    'y', ' ', 'k', 'e', 'y', ' ', 't', 'o',
    ' ', 'r', 'e', 'b', 'o', 'o', 't',
    0x0d, 0x0a,
    0
};

static volatile sig_atomic_t got_siginfo;
static void infohandler(int);

static ssize_t getchunksize(void);
static int getstdfmt(const char *, struct bpb *);
static int getdiskinfo(int, const char *, const char *, struct msdos_options *, struct bpb *);
static void print_bpb(struct bpb *);
static void mklabel(u_int8_t *, const char *, bool);
static int oklabel(const char *);
static void setstr(u_int8_t *, const char *, size_t);

int
mkfs_msdos(const char *fname, const char *dtype, const struct msdos_options *op)
{
    char buf[MAXPATHLEN];
    struct sigaction si_sa;
    struct stat sb;
    struct timeval tv;
    struct bpb bpb;
    struct tm timestamp, *tm = &timestamp;
    struct bs *bs;
    struct bsbpb *bsbpb;
    struct bsxbpb *bsxbpb;
    struct bsx *bsx;
    struct de *de;
    u_int8_t *img;
    u_int8_t *physbuf, *physbuf_end;
    const char *bname;
    ssize_t n;
    time_t now;
    u_int fat, bss, rds, cls, dir, lsn, x, x1;
    bool set_res, set_spf;
    int fd, fd1, rv;
    struct msdos_options o = *op;
    ssize_t chunksize;

    physbuf = NULL;
    rv = -1;
    fd = fd1 = -1;

    if (o.block_size && o.sectors_per_cluster) {
	warnx("Cannot specify both block size and sectors per cluster");
	goto done;
    }
    if (o.OEM_string && strlen(o.OEM_string) > 8) {
	warnx("%s: OEM string too long", o.OEM_string);
	goto done;
    }
    if (o.create_size && o.no_create) {
        warnx("create (-C) is incompatible with -N");
        goto done;
    }
    if (!o.no_create) {
        if (o.timestamp_set) {
            now = o.timestamp;
            tv.tv_usec = 0;
            if (!gmtime_r(&now, tm)) {
                warn("invalid format timestamp");
                goto done;
            }
        } else {
            if (gettimeofday(&tv, NULL) != 0) {
                warn("cannot get format timestamp");
                goto done;
            }
            now = tv.tv_sec;
            if (!localtime_r(&now, tm)) {
                warn("invalid format timestamp");
                goto done;
            }
        }
    }
    fd = bx_fat_open(fname, !o.no_create, o.create_size, o.dos_compat);
    if (fd == -1) {
        warn("%s", fname);
        goto done;
    }
    memset(&bpb, 0, sizeof(bpb));
    if (o.floppy) {
	if (getstdfmt(o.floppy, &bpb) == -1)
	    goto done;
	bpb.bpbHugeSectors = bpb.bpbSectors;
	bpb.bpbSectors = 0;
	bpb.bpbBigFATsecs = bpb.bpbFATsecs;
	bpb.bpbFATsecs = 0;
    }
    if (o.drive_heads)
	bpb.bpbHeads = o.drive_heads;
    if (o.sectors_per_track)
	bpb.bpbSecPerTrack = o.sectors_per_track;
    if (o.bytes_per_sector)
	bpb.bpbBytesPerSec = o.bytes_per_sector;
    if (o.size)
	bpb.bpbHugeSectors = o.size;
    if (o.hidden_sectors_set)
	bpb.bpbHiddenSecs = o.hidden_sectors;
    {
	if (getdiskinfo(fd, fname, dtype, &o, &bpb) == -1)
		goto done;
        if (!o.dos_compat && (o.offset % bpb.bpbBytesPerSec != 0 ||
            (uint64_t)o.offset / bpb.bpbBytesPerSec >= bpb.bpbHugeSectors)) {
            warnx("offset outside filesystem or not sector aligned");
            goto done;
        }
        if (!o.dos_compat)
            bpb.bpbHugeSectors -= (o.offset / bpb.bpbBytesPerSec);
        if (o.offset && lseek(fd, o.offset, SEEK_SET) != o.offset) {
            warn("cannot seek to filesystem offset");
            goto done;
        }
        uint64_t volume_bytes = (uint64_t)bpb.bpbHugeSectors * bpb.bpbBytesPerSec;
        if (o.dos_compat && !o.fat_type && volume_bytes >= 512ULL * 1024 * 1024)
            o.fat_type = 32;
        if (o.dos_compat && o.fat_type == 32) {
            unsigned cluster_bytes = volume_bytes <= 260ULL * 1024 * 1024 ? 512 :
                volume_bytes <= 8ULL * 1024 * 1024 * 1024 ? 4096 :
                volume_bytes <= 16ULL * 1024 * 1024 * 1024 ? 8192 :
                volume_bytes <= 32ULL * 1024 * 1024 * 1024 ? 16384 : 32768;
            bpb.bpbSecPerClust = MAX(1, cluster_bytes / bpb.bpbBytesPerSec);
            if (!o.backup_sector)
                o.backup_sector = 6;
        }
	if (bpb.bpbSecPerClust == 0) {	/* set defaults */
	    if (bpb.bpbHugeSectors <= 6000)	/* about 3MB -> 512 bytes */
		bpb.bpbSecPerClust = 1;
	    else if (bpb.bpbHugeSectors <= (1<<17)) /* 64M -> 4k */
		bpb.bpbSecPerClust = 8;
	    else if (bpb.bpbHugeSectors <= (1<<19)) /* 256M -> 8k */
		bpb.bpbSecPerClust = 16;
	    else if (bpb.bpbHugeSectors <= (1<<21)) /* 1G -> 16k */
		bpb.bpbSecPerClust = 32;
	    else
		bpb.bpbSecPerClust = 64;		/* otherwise 32k */
	}
    }
    if (bpb.bpbBytesPerSec < MINBPS ||
        bpb.bpbBytesPerSec > MAXBPS ||
	!powerof2(bpb.bpbBytesPerSec)) {
	warnx("Invalid bytes/sector (%u): must be 512, 1024, 2048 or 4096",
	    bpb.bpbBytesPerSec);
	goto done;
    }

    if (o.volume_label && !oklabel(o.volume_label)) {
	warnx("%s: bad volume label", o.volume_label);
	goto done;
    }
    if (!(fat = o.fat_type)) {
	if (o.floppy)
	    fat = 12;
	else if (!o.directory_entries && (o.info_sector || o.backup_sector))
	    fat = 32;
    }
    if ((fat == 32 && o.directory_entries) || (fat != 32 && (o.info_sector || o.backup_sector))) {
	warnx("-%c is not a legal FAT%s option",
	     fat == 32 ? 'e' : o.info_sector ? 'i' : 'k',
	     fat == 32 ? "32" : "12/16");
	goto done;
    }
    if (o.floppy && fat == 32)
	bpb.bpbRootDirEnts = 0;
    if (fat != 0 && fat != 12 && fat != 16 && fat != 32) {
	warnx("%d: bad FAT type", fat);
	goto done;
    }

    if (o.block_size) {
	if (!powerof2(o.block_size)) {
	    warnx("block size (%u) is not a power of 2", o.block_size);
	    goto done;
	}
	if (o.block_size < bpb.bpbBytesPerSec) {
	    warnx("block size (%u) is too small; minimum is %u",
		 o.block_size, bpb.bpbBytesPerSec);
	    goto done;
	}
	if (o.block_size > bpb.bpbBytesPerSec * MAXSPC) {
	    warnx("block size (%u) is too large; maximum is %u",
		 o.block_size, bpb.bpbBytesPerSec * MAXSPC);
	    goto done;
	}
	bpb.bpbSecPerClust = o.block_size / bpb.bpbBytesPerSec;
    }
    if (o.sectors_per_cluster) {
	if (!powerof2(o.sectors_per_cluster)) {
	    warnx("sectors/cluster (%u) is not a power of 2",
		o.sectors_per_cluster);
	    goto done;
	}
	bpb.bpbSecPerClust = o.sectors_per_cluster;
    }
    if (o.reserved_sectors)
	bpb.bpbResSectors = o.reserved_sectors;
    if (o.num_FAT) {
	if (o.num_FAT > MAXNFT) {
	    warnx("number of FATs (%u) is too large; maximum is %u",
		 o.num_FAT, MAXNFT);
	    goto done;
	}
	bpb.bpbFATs = o.num_FAT;
    }
    if (o.directory_entries) {
	bpb.bpbRootDirEnts = roundup(o.directory_entries,
	    bpb.bpbBytesPerSec / sizeof(struct de));
	if (bpb.bpbBytesPerSec == 0 || o.directory_entries >= MAXU16)
	    bpb.bpbRootDirEnts = MAXU16;
    }
    if (o.media_descriptor_set) {
	if (o.media_descriptor < 0xf0) {
	    warnx("illegal media descriptor (%#x)", o.media_descriptor);
	    goto done;
	}
	bpb.bpbMedia = o.media_descriptor;
    }
    if (o.sectors_per_fat)
	bpb.bpbBigFATsecs = o.sectors_per_fat;
    if (o.info_sector)
	bpb.bpbFSInfo = o.info_sector;
    if (o.backup_sector)
	bpb.bpbBackup = o.backup_sector;
    bss = 1;
    bname = NULL;
    fd1 = -1;
    if (o.bootstrap) {
	bname = o.bootstrap;
	if (!strchr(bname, '/')) {
	    snprintf(buf, sizeof(buf), "/boot/%s", bname);
	    bname = buf;
	}
	if ((fd1 = open(bname, O_RDONLY)) == -1 || fstat(fd1, &sb)) {
	    warn("%s", bname);
	    goto done;
	}
	if (!S_ISREG(sb.st_mode) || sb.st_size % bpb.bpbBytesPerSec ||
	    sb.st_size < bpb.bpbBytesPerSec ||
	    sb.st_size > bpb.bpbBytesPerSec * MAXU16) {
	    warnx("%s: inappropriate file type or format", bname);
	    goto done;
	}
	bss = sb.st_size / bpb.bpbBytesPerSec;
    }
    if (!bpb.bpbFATs)
	bpb.bpbFATs = 2;
    if (!fat) {
	if (bpb.bpbHugeSectors < (bpb.bpbResSectors ? bpb.bpbResSectors : bss) +
	    howmany((RESFTE + (bpb.bpbSecPerClust ? MINCLS16 : MAXCLS12 + 1)) *
		(bpb.bpbSecPerClust ? 16 : 12) / BPN,
		bpb.bpbBytesPerSec * NPB) *
	    bpb.bpbFATs +
	    howmany(bpb.bpbRootDirEnts ? bpb.bpbRootDirEnts : DEFRDE,
		    bpb.bpbBytesPerSec / sizeof(struct de)) +
	    (bpb.bpbSecPerClust ? MINCLS16 : MAXCLS12 + 1) *
	    (bpb.bpbSecPerClust ? bpb.bpbSecPerClust :
	     howmany(DEFBLK, bpb.bpbBytesPerSec)))
	    fat = 12;
	else if (bpb.bpbRootDirEnts || bpb.bpbHugeSectors <
		 (bpb.bpbResSectors ? bpb.bpbResSectors : bss) +
		 howmany((RESFTE + MAXCLS16) * 2, bpb.bpbBytesPerSec) *
		 bpb.bpbFATs +
		 howmany(DEFRDE, bpb.bpbBytesPerSec / sizeof(struct de)) +
		 (MAXCLS16 + 1) *
		 (bpb.bpbSecPerClust ? bpb.bpbSecPerClust :
		  howmany(8192, bpb.bpbBytesPerSec)))
	    fat = 16;
	else
	    fat = 32;
    }
    x = bss;
    if (fat == 32) {
	if (!bpb.bpbFSInfo) {
	    if (x == MAXU16 || x == bpb.bpbBackup) {
		warnx("no room for info sector");
		goto done;
	    }
	    bpb.bpbFSInfo = x;
	}
	if (bpb.bpbFSInfo != MAXU16 && x <= bpb.bpbFSInfo)
	    x = bpb.bpbFSInfo + 1;
	if (!bpb.bpbBackup) {
	    if (x == MAXU16) {
		warnx("no room for backup sector");
		goto done;
	    }
	    bpb.bpbBackup = x;
	} else if (bpb.bpbBackup != MAXU16 && bpb.bpbBackup == bpb.bpbFSInfo) {
	    warnx("backup sector would overwrite info sector");
	    goto done;
	}
	if (bpb.bpbBackup != MAXU16 && x <= bpb.bpbBackup)
	    x = bpb.bpbBackup + 1;
        if (o.dos_compat && bpb.bpbBackup != MAXU16) {
            if (bpb.bpbBackup > MAXU16 - 3 || bpb.bpbBackup < 3) {
                warnx("backup boot region overlaps primary or exceeds reserved range");
                goto done;
            }
            x = MAX(x, bpb.bpbBackup + 3);
        }
    }

    set_res = (bpb.bpbResSectors == 0 || o.dos_compat);
    set_spf = (bpb.bpbBigFATsecs == 0);
    if (set_res)
        bpb.bpbResSectors = MAX(o.reserved_sectors, fat == 32 ?
            MAX(x, o.dos_compat ? 32 : MAX(16384 / bpb.bpbBytesPerSec, 4)) : x);
    else if (bpb.bpbResSectors < x) {
        warnx("too few reserved sectors");
        goto done;
    }
    if (fat != 32 && !bpb.bpbRootDirEnts)
        bpb.bpbRootDirEnts = DEFRDE;
    rds = howmany(bpb.bpbRootDirEnts, bpb.bpbBytesPerSec / sizeof(struct de));
    if (o.dos_compat && o.align) {
        bpb.bpbResSectors = roundup(bpb.bpbResSectors, bpb.bpbSecPerClust);
        rds = roundup(rds, bpb.bpbSecPerClust);
        uint64_t entries = (uint64_t)rds * bpb.bpbBytesPerSec / sizeof(struct de);
        if (entries > MAXU16) {
            warnx("aligned root directory exceeds FAT entry limit");
            goto done;
        }
        bpb.bpbRootDirEnts = entries;
    }
    uint64_t base = (uint64_t)bpb.bpbResSectors + rds;
    if (base >= bpb.bpbHugeSectors) {
        warnx("metadata exceeds filesystem size");
        goto done;
    }
    if (set_spf) {
        uint64_t estimate = (bpb.bpbHugeSectors - base) * bpb.bpbBytesPerSec * 8 /
            ((uint64_t)bpb.bpbSecPerClust * bpb.bpbBytesPerSec * 8 + fat * bpb.bpbFATs);
        bpb.bpbBigFATsecs = howmany((estimate + RESFTE) * fat,
            (uint64_t)bpb.bpbBytesPerSec * 8);
    }
    /* Growing the FAT or reserved area only reduces the number of data clusters.
     * Recompute from the final layout, including the two reserved FAT entries. */
    for (;;) {
        if (o.dos_compat && o.align)
            bpb.bpbBigFATsecs = roundup(bpb.bpbBigFATsecs, bpb.bpbSecPerClust);
        uint64_t first = (uint64_t)bpb.bpbResSectors + rds +
            (uint64_t)bpb.bpbBigFATsecs * bpb.bpbFATs;
        if (first >= bpb.bpbHugeSectors || bpb.bpbResSectors > MAXU16 ||
            (fat != 32 && bpb.bpbBigFATsecs > MAXU16)) {
            warnx("FAT metadata exceeds filesystem limits");
            goto done;
        }
        if (set_res && (!o.dos_compat || o.align)) {
            unsigned remainder = first % bpb.bpbSecPerClust;
            if (remainder) {
                bpb.bpbResSectors += bpb.bpbSecPerClust - remainder;
                continue;
            }
        }
        x1 = first;
        cls = (bpb.bpbHugeSectors - x1) / bpb.bpbSecPerClust;
        uint64_t needed = howmany(((uint64_t)cls + RESFTE) * fat,
            (uint64_t)bpb.bpbBytesPerSec * 8);
        if (needed <= bpb.bpbBigFATsecs)
            break;
        if (!set_spf) {
            warnx("FAT cannot hold the declared data clusters");
            goto done;
        }
        bpb.bpbBigFATsecs = needed;
    }
    if (cls < mincls(fat)) {
	warnx("%u clusters too few clusters for FAT%u, need %u", cls, fat,
	    mincls(fat));
	goto done;
    }
    if (cls > maxcls(fat)) {
	cls = maxcls(fat);
	bpb.bpbHugeSectors = x1 + cls * bpb.bpbSecPerClust;
	warnx("warning: FAT type limits file system to %u sectors",
	      bpb.bpbHugeSectors);
    }
    printf("%s: %u sector%s in %u FAT%u cluster%s "
	   "(%u bytes/cluster)\n", fname, cls * bpb.bpbSecPerClust,
	   cls * bpb.bpbSecPerClust == 1 ? "" : "s", cls, fat,
	   cls == 1 ? "" : "s", bpb.bpbBytesPerSec * bpb.bpbSecPerClust);
    if (!bpb.bpbMedia)
	bpb.bpbMedia = !bpb.bpbHiddenSecs ? 0xf0 : 0xf8;
    if (fat == 32)
	bpb.bpbRootClust = RESFTE;
    if (bpb.bpbHugeSectors <= MAXU16) {
	bpb.bpbSectors = bpb.bpbHugeSectors;
	bpb.bpbHugeSectors = 0;
    }
    if (fat != 32) {
	bpb.bpbFATsecs = bpb.bpbBigFATsecs;
	bpb.bpbBigFATsecs = 0;
    }
    print_bpb(&bpb);
    if (!o.no_create) {
	chunksize = getchunksize();
	physbuf = malloc(chunksize);
	if (physbuf == NULL) {
	    warn(NULL);
	    goto done;
	}
	physbuf_end = physbuf + chunksize;
	img = physbuf;

	dir = bpb.bpbResSectors + (bpb.bpbFATsecs ? bpb.bpbFATsecs :
				   bpb.bpbBigFATsecs) * bpb.bpbFATs;
	memset(&si_sa, 0, sizeof(si_sa));
	si_sa.sa_handler = infohandler;
#ifdef SIGINFO
	if (sigaction(SIGINFO, &si_sa, NULL) == -1) {
	    warn("sigaction SIGINFO");
	    goto done;
	}
#endif
	for (lsn = 0; lsn < dir + (fat == 32 ? bpb.bpbSecPerClust : rds); lsn++) {
	    if (got_siginfo) {
		    fprintf(stderr,"%s: writing sector %u of %u (%u%%)\n",
			fname, lsn,
			(dir + (fat == 32 ? bpb.bpbSecPerClust: rds)),
			(lsn * 100) / (dir +
			    (fat == 32 ? bpb.bpbSecPerClust: rds)));
		    got_siginfo = 0;
	    }
	    x = lsn;
	    if (o.bootstrap &&
		fat == 32 && bpb.bpbBackup != MAXU16 &&
		bss <= bpb.bpbBackup && x >= bpb.bpbBackup) {
		x -= bpb.bpbBackup;
		if (!x && lseek(fd1, o.offset, SEEK_SET)) {
		    warn("%s", bname);
		    goto done;
		}
	    }
	    if (o.bootstrap && x < bss) {
		if ((n = bx_fat_read(fd1, img, bpb.bpbBytesPerSec)) == -1) {
		    warn("%s", bname);
		    goto done;
		}
		if ((unsigned)n != bpb.bpbBytesPerSec) {
		    warnx("%s: can't read sector %u", bname, x);
		    goto done;
		}
	    } else
		memset(img, 0, bpb.bpbBytesPerSec);
	    if (!lsn ||
		(fat == 32 && bpb.bpbBackup != MAXU16 &&
		 lsn == bpb.bpbBackup)) {
		x1 = sizeof(struct bs);
		bsbpb = (struct bsbpb *)(img + x1);
		mk2(bsbpb->bpbBytesPerSec, bpb.bpbBytesPerSec);
		mk1(bsbpb->bpbSecPerClust, bpb.bpbSecPerClust);
		mk2(bsbpb->bpbResSectors, bpb.bpbResSectors);
		mk1(bsbpb->bpbFATs, bpb.bpbFATs);
		mk2(bsbpb->bpbRootDirEnts, bpb.bpbRootDirEnts);
		mk2(bsbpb->bpbSectors, bpb.bpbSectors);
		mk1(bsbpb->bpbMedia, bpb.bpbMedia);
		mk2(bsbpb->bpbFATsecs, bpb.bpbFATsecs);
		mk2(bsbpb->bpbSecPerTrack, bpb.bpbSecPerTrack);
		mk2(bsbpb->bpbHeads, bpb.bpbHeads);
		mk4(bsbpb->bpbHiddenSecs, bpb.bpbHiddenSecs);
		mk4(bsbpb->bpbHugeSectors, bpb.bpbHugeSectors);
		x1 += sizeof(struct bsbpb);
		if (fat == 32) {
		    bsxbpb = (struct bsxbpb *)(img + x1);
		    mk4(bsxbpb->bpbBigFATsecs, bpb.bpbBigFATsecs);
		    mk2(bsxbpb->bpbExtFlags, 0);
		    mk2(bsxbpb->bpbFSVers, 0);
		    mk4(bsxbpb->bpbRootClust, bpb.bpbRootClust);
		    mk2(bsxbpb->bpbFSInfo, bpb.bpbFSInfo);
		    mk2(bsxbpb->bpbBackup, o.dos_compat && bpb.bpbBackup == MAXU16 ? 0 : bpb.bpbBackup);
		    x1 += sizeof(struct bsxbpb);
		}
		bsx = (struct bsx *)(img + x1);
		mk1(bsx->exBootSignature, 0x29);
                if (o.dos_compat)
                    mk1(bsx->exDriveNumber, o.drive_number);
		if (o.volume_id_set)
		    x = o.volume_id;
		else
		    x = (((u_int)(1 + tm->tm_mon) << 8 |
			  (u_int)tm->tm_mday) +
			 ((u_int)tm->tm_sec << 8 |
			  (u_int)(tv.tv_usec / 10))) << 16 |
			((1900U + (u_int)tm->tm_year) +
			 ((u_int)tm->tm_hour << 8 |
			  (u_int)tm->tm_min));
		mk4(bsx->exVolumeID, x);
		mklabel(bsx->exVolumeLabel, o.volume_label ? o.volume_label : "NO NAME", !o.dos_compat);
		snprintf(buf, sizeof(buf), "FAT%u", fat);
		setstr(bsx->exFileSysType, buf, sizeof(bsx->exFileSysType));
		if (!o.bootstrap) {
		    x1 += sizeof(struct bsx);
		    bs = (struct bs *)img;
		    mk1(bs->bsJump[0], 0xeb);
		    mk1(bs->bsJump[1], x1 - 2);
		    mk1(bs->bsJump[2], 0x90);
		    setstr(bs->bsOemName, o.OEM_string ? o.OEM_string : "BSD4.4  ",
			   sizeof(bs->bsOemName));
		    memcpy(img + x1, bootcode, sizeof(bootcode));
		    mk2(img + MINBPS - 2, DOSMAGIC);
		}
	    } else if (fat == 32 && bpb.bpbFSInfo != MAXU16 &&
		       (lsn == bpb.bpbFSInfo ||
			(bpb.bpbBackup != MAXU16 &&
			 lsn == bpb.bpbBackup + bpb.bpbFSInfo))) {
		mk4(img, 0x41615252);
		mk4(img + MINBPS - 28, 0x61417272);
		mk4(img + MINBPS - 24, o.dos_compat ? cls - 1 : 0xffffffff);
		mk4(img + MINBPS - 20, o.dos_compat ? 3 : 0xffffffff);
		mk2(img + MINBPS - 2, DOSMAGIC);
            } else if (o.dos_compat && fat == 32 &&
                       (lsn == 2 || (bpb.bpbBackup != MAXU16 && lsn == bpb.bpbBackup + 2))) {
                mk2(img + MINBPS - 2, DOSMAGIC);
	    } else if (lsn >= bpb.bpbResSectors && lsn < dir &&
		       !((lsn - bpb.bpbResSectors) %
			 (bpb.bpbFATsecs ? bpb.bpbFATsecs :
			  bpb.bpbBigFATsecs))) {
		mk1(img[0], bpb.bpbMedia);
		for (x = 1; x < fat * (fat == 32 ? 3 : 2) / 8; x++)
		    mk1(img[x], fat == 32 && x % 4 == 3 ? 0x0f : 0xff);
	    } else if (lsn == dir && o.volume_label) {
		de = (struct de *)img;
		mklabel(de->deName, o.volume_label, !o.dos_compat);
		mk1(de->deAttributes, 050);
		x = (u_int)tm->tm_hour << 11 |
		    (u_int)tm->tm_min << 5 |
		    (u_int)tm->tm_sec >> 1;
		mk2(de->deMTime, x);
		x = ((u_int)tm->tm_year - 80U) << 9 |
		    (u_int)(tm->tm_mon + 1) << 5 |
		    (u_int)tm->tm_mday;
		mk2(de->deMDate, x);
	    }
	    /*
	     * Issue a write of chunksize once we have collected
	     * enough sectors.
	     */
	    img += bpb.bpbBytesPerSec;
	    if (img >= physbuf_end) {
		n = bx_fat_write(fd, physbuf, chunksize);
		if (n != chunksize) {
		    warnx("%s: can't write sector %u", fname, lsn);
		    goto done;
		}
		img = physbuf;
	    }
	}
	/*
	 * Write remaining sectors, if the last write didn't end
	 * up filling a whole chunk.
	 */
	if (img != physbuf) {
		ssize_t tailsize = img - physbuf;

		n = bx_fat_write(fd, physbuf, tailsize);
		if (n != tailsize) {
		    warnx("%s: can't write sector %u", fname, lsn);
		    goto done;
		}
	}
    }
    rv = 0;
done:
    free(physbuf);
    if (fd != -1) {
	if (rv == 0 && !o.no_create && bx_fd_fsync(fd) < 0) {
	    warn("%s: flush", fname);
	    rv = -1;
	}
	if (close(fd) < 0 && rv == 0) {
	    warn("%s: close", fname);
	    rv = -1;
	}
    }
    if (fd1 != -1)
	    close(fd1);

    return rv;
}

static ssize_t
getchunksize(void)
{
    return 128 * 1024;
}

/*
 * Get a standard format.
 */
static int
getstdfmt(const char *fmt, struct bpb *bpb)
{
    u_int x, i;

    x = nitems(stdfmt);
    for (i = 0; i < x && strcmp(fmt, stdfmt[i].name); i++);
    if (i == x) {
	warnx("%s: unknown standard format", fmt);
	return -1;
    }
    *bpb = stdfmt[i].bpb;
    return 0;
}

static int
getdiskinfo(int fd, const char *fname, const char *dtype, struct msdos_options *o,
            struct bpb *bpb)
{
    uint64_t bytes, sectors;
    unsigned sector_size;

    if (dtype != NULL) {
        warnx("disk-type database lookup is not supported");
        return -1;
    }
    if (bx_fat_geometry(fd, &bytes, &sector_size) < 0) {
        warn("%s: cannot get media size", fname);
        return -1;
    }
    if (!bpb->bpbBytesPerSec)
        bpb->bpbBytesPerSec = sector_size;
    if (bpb->bpbBytesPerSec < MINBPS || bpb->bpbBytesPerSec > MAXBPS ||
        !powerof2(bpb->bpbBytesPerSec)) {
        warnx("invalid bytes/sector");
        return -1;
    }
    sectors = bytes / bpb->bpbBytesPerSec;
    if (o->dos_compat) {
        if (o->offset_sectors > (uint64_t)INT64_MAX / bpb->bpbBytesPerSec ||
            o->offset_sectors >= sectors) {
            warnx("offset outside media");
            return -1;
        }
        o->offset = o->offset_sectors * bpb->bpbBytesPerSec;
        sectors -= o->offset_sectors;
        uint64_t requested = o->size_bytes ? o->size_bytes / bpb->bpbBytesPerSec : sectors;
        if (!requested || requested > sectors || requested > UINT32_MAX) {
            warnx("filesystem size exceeds media or FAT sector count");
            return -1;
        }
        bpb->bpbHugeSectors = requested;
    }
    if (!bpb->bpbHugeSectors) {
        if (sectors > UINT32_MAX) {
            warnx("media too large for FAT sector count");
            return -1;
        }
        bpb->bpbHugeSectors = sectors;
    }
    if (!bpb->bpbHugeSectors || bpb->bpbHugeSectors > sectors) {
        warnx("filesystem size exceeds media");
        return -1;
    }
    if (!bpb->bpbSecPerTrack)
        bpb->bpbSecPerTrack = 63;
    if (!bpb->bpbHeads)
        bpb->bpbHeads = 255;
    return 0;
}

/*
 * Print out BPB values.
 */
static void
print_bpb(struct bpb *bpb)
{
    printf("BytesPerSec=%u SecPerClust=%u ResSectors=%u FATs=%u",
	   bpb->bpbBytesPerSec, bpb->bpbSecPerClust, bpb->bpbResSectors,
	   bpb->bpbFATs);
    if (bpb->bpbRootDirEnts)
	printf(" RootDirEnts=%u", bpb->bpbRootDirEnts);
    if (bpb->bpbSectors)
	printf(" Sectors=%u", bpb->bpbSectors);
    printf(" Media=%#x", bpb->bpbMedia);
    if (bpb->bpbFATsecs)
	printf(" FATsecs=%u", bpb->bpbFATsecs);
    printf(" SecPerTrack=%u Heads=%u HiddenSecs=%u", bpb->bpbSecPerTrack,
	   bpb->bpbHeads, bpb->bpbHiddenSecs);
    if (bpb->bpbHugeSectors)
	printf(" HugeSectors=%u", bpb->bpbHugeSectors);
    if (!bpb->bpbFATsecs) {
	printf(" FATsecs=%u RootCluster=%u", bpb->bpbBigFATsecs,
	       bpb->bpbRootClust);
	printf(" FSInfo=");
	printf(bpb->bpbFSInfo == MAXU16 ? "%#x" : "%u", bpb->bpbFSInfo);
	printf(" Backup=");
	printf(bpb->bpbBackup == MAXU16 ? "%#x" : "%u", bpb->bpbBackup);
    }
    printf("\n");
}

/*
 * Check a volume label.
 */
static int
oklabel(const char *src)
{
    int c, i;

    for (i = 0; i <= 11; i++) {
	c = (u_char)*src++;
	if (c < ' ' + !i || strchr("\"*+,./:;<=>?[\\]|", c))
	    break;
    }
    return i && !c;
}

/*
 * Make a volume label.
 */
static void
mklabel(u_int8_t *dest, const char *src, bool uppercase)
{
    int c, i;

    for (i = 0; i < 11; i++) {
	c = *src ? (unsigned char)*src++ : ' ';
        if (uppercase)
            c = toupper(c);
	*dest++ = !i && c == '\xe5' ? 5 : c;
    }
}

/*
 * Copy string, padding with spaces.
 */
static void
setstr(u_int8_t *dest, const char *src, size_t len)
{
    while (len--)
	*dest++ = *src ? *src++ : ' ';
}

static void
infohandler(int sig BX_FAT_UNUSED)
{

	got_siginfo = 1;
}
