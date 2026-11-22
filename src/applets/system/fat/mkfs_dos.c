/* SPDX-License-Identifier: BSD-2-Clause */
#include "dos.h"
#include "lib/fat/format/mkfs_msdos.h"

#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static uint64_t number(const char *text, int base, uint64_t min, uint64_t max)
{
    char *end;
    errno = 0;
    if (!*text || *text == '-' || *text == '+' || isspace((unsigned char)*text))
        errx(1, "invalid number: %s", text);
    unsigned long long value = strtoull(text, &end, base);
    if (errno || end == text || *end || value < min || value > max)
        errx(1, "number out of range: %s", text);
    return value;
}

static void help(void)
{
    puts("usage: mkfs.fat [OPTIONS] TARGET [BLOCKS]\n"
         "  -C          create TARGET; BLOCKS is required (1024-byte units)\n"
         "  -F 12|16|32 FAT type\n"
         "  -n LABEL    volume label (up to 11 ASCII characters)\n"
         "  -i HEX      hexadecimal volume ID\n"
         "  -s COUNT    sectors per cluster (power of two, 1..128)\n"
         "  -S BYTES    sector size (512, 1024, 2048, 4096)\n"
         "  -f COUNT    number of FATs\n"
         "  -R COUNT    minimum reserved sectors\n"
         "  -r COUNT    minimum root entries (FAT12/16)\n"
         "  -b SECTOR   FAT32 backup boot sector (0 disables)\n"
         "  -h COUNT    hidden sectors\n"
         "  -D NUMBER   BIOS drive number\n"
         "  -M TYPE     media descriptor\n"
         "  -g H/S      geometry: heads/sectors per track\n"
         "  -a          disable alignment\n"
         "  -v          verbose output\n"
         "  --offset=N  start at filesystem sector N\n"
         "  --invariant use fixed volume ID and timestamp\n"
         "  --help      show help\n"
         "Unsupported options fail before opening TARGET. BSD options: newfs_msdos.");
}

int bx_fat_mkfs_dos(int argc, char **argv)
{
    enum { OFFSET = 256, INVARIANT, VARIANT, CODEPAGE, MBR, HELP };
    static const struct option longs[] = {
        {"offset", required_argument, NULL, OFFSET},
        {"invariant", no_argument, NULL, INVARIANT},
        {"variant", required_argument, NULL, VARIANT},
        {"codepage", required_argument, NULL, CODEPAGE},
        {"mbr", optional_argument, NULL, MBR},
        {"help", no_argument, NULL, HELP},
        {NULL, 0, NULL, 0}
    };
    struct msdos_options o = {0};
    bool create = false, invariant = false;
    int ch;
    o.dos_compat = true;
    o.align = true;
    o.media_descriptor = 0xf8;
    o.media_descriptor_set = 1;
    o.drive_number = 0x80;
    while ((ch = getopt_long(argc, argv, "aAb:cCD:f:F:g:h:i:Il:m:M:n:r:R:s:S:v",
                             longs, NULL)) != -1) {
        switch (ch) {
        case 'a': o.align = false; break;
        case 'C': create = true; break;
        case 'F':
            o.fat_type = number(optarg, 10, 12, 32);
            if (o.fat_type != 12 && o.fat_type != 16 && o.fat_type != 32)
                errx(1, "FAT type must be 12, 16 or 32");
            break;
        case 'n': o.volume_label = optarg; break;
        case 'i':
            o.volume_id = number(optarg, 16, 0, UINT32_MAX);
            o.volume_id_set = 1;
            break;
        case 's':
            o.sectors_per_cluster = number(optarg, 10, 1, 128);
            if (!powerof2(o.sectors_per_cluster))
                errx(1, "sectors per cluster must be a power of two");
            break;
        case 'S':
            o.bytes_per_sector = number(optarg, 10, 512, 4096);
            if (!powerof2(o.bytes_per_sector))
                errx(1, "sector size must be a power of two");
            break;
        case 'f': o.num_FAT = number(optarg, 10, 1, 16); break;
        case 'R': o.reserved_sectors = number(optarg, 10, 1, UINT16_MAX); break;
        case 'r': o.directory_entries = number(optarg, 10, 1, UINT16_MAX); break;
        case 'b':
            o.backup_sector = number(optarg, 10, 0, UINT16_MAX - 1);
            if (!o.backup_sector)
                o.backup_sector = UINT16_MAX;
            break;
        case 'h':
            o.hidden_sectors = number(optarg, 10, 0, UINT32_MAX);
            o.hidden_sectors_set = 1;
            break;
        case 'D': o.drive_number = number(optarg, 0, 0, UINT8_MAX); break;
        case 'M': o.media_descriptor = number(optarg, 0, 0xf0, 0xff); break;
        case 'g': {
            char *geometry = strdup(optarg);
            if (!geometry)
                err(1, "geometry");
            char *slash = strchr(geometry, '/');
            if (!slash)
                errx(1, "geometry must be heads/sectors_per_track");
            *slash++ = '\0';
            o.drive_heads = number(geometry, 10, 1, UINT16_MAX);
            o.sectors_per_track = number(slash, 10, 1, UINT16_MAX);
            free(geometry);
            break;
        }
        case 'v': break; /* The formatter already prints its geometry. */
        case OFFSET: o.offset_sectors = number(optarg, 10, 0, INT64_MAX / 512); break;
        case INVARIANT: invariant = true; break;
        case VARIANT:
            if (strcasecmp(optarg, "standard"))
                errx(1, "only the standard FAT variant is supported");
            break;
        case CODEPAGE:
            if (strcmp(optarg, "850"))
                errx(1, "only ASCII labels with codepage 850 are supported");
            break;
        case MBR:
            if (!optarg || strcmp(optarg, "n"))
                errx(1, "partition-table generation is not supported");
            break;
        case HELP: help(); return 0;
        default:
            warnx("unsupported option; target not opened (use --help)");
            return 1;
        }
    }
    if (argc - optind < 1 || argc - optind > 2 || (create && argc - optind != 2)) {
        help();
        return 1;
    }
    if (o.volume_label) {
        size_t length = strlen(o.volume_label);
        if (!length || length > 11 || o.volume_label[0] == ' ')
            errx(1, "invalid volume label");
        for (size_t i = 0; i < length; i++) {
            unsigned char c = o.volume_label[i];
            if (c < 32 || c > 126 || strchr("\"*+,./:;<=>?[\\]|", c))
                errx(1, "volume label must contain valid ASCII characters");
        }
    }
    if (argc - optind == 2)
        o.size_bytes = number(argv[optind + 1], 10, 1, INT64_MAX / 1024) * 1024;
    if (create) {
        uint64_t sector = o.bytes_per_sector ? o.bytes_per_sector : 512;
        if (o.offset_sectors > (INT64_MAX - o.size_bytes) / sector)
            errx(1, "image size and offset exceed 64-bit offsets");
        o.create_size = o.size_bytes + o.offset_sectors * sector;
    }
    if (invariant) {
        if (!o.volume_id_set) {
            o.volume_id = 0x1234abcd;
            o.volume_id_set = 1;
        }
        o.timestamp = 315532800; /* FAT epoch: 1980-01-01 UTC. */
        o.timestamp_set = 1;
    }
    return mkfs_msdos(argv[optind], NULL, &o) != 0;
}
