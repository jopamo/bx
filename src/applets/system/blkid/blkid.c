/* SPDX-License-Identifier: BSD-2-Clause */
#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"
#include "lib/fd_ops.h"
#include "lib/fs_probe.h"

enum output { FULL, EXPORT, VALUE };
enum tag { FAT_BOOT_LABEL, LABEL, UUID, BLOCK_SIZE, TYPE, USAGE, TAG_COUNT };

static const char *const tag_names[TAG_COUNT] = {
    "LABEL_FATBOOT", "LABEL", "UUID", "BLOCK_SIZE", "TYPE", "USAGE",
};

static void usage(FILE *out)
{
    fputs("Usage: blkid [-p] [-u filesystem] [-o full|export|value]"
          " [-s TAG]... [--] DEVICE...\n"
          "Probe explicit block devices or images for FAT and ext filesystems.\n", out);
}

/* Keep full/export records on one line even for untrusted volume labels
 * or paths. Only value output deliberately returns the unescaped tag. */
static void print_escaped(const char *value, enum output format)
{
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        unsigned char c = *p;
        if (format == FULL && c >= ' ' && c <= '~') {
            if (c == '"' || c == '\\')
                putchar('\\');
            putchar(c);
        } else if (format == EXPORT && c == ' ') {
            fputs("\\ ", stdout);
        } else if (format == EXPORT && ((c >= 'A' && c <= 'Z') ||
                   (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                   strchr("_./:+-", c))) {
            putchar(c);
        } else {
            printf("\\x%02x", (unsigned)c);
        }
    }
}

static bool print_signature(const char *path, const struct bx_fs_signature *fs,
                            unsigned tags, enum output format, bool lowlevel,
                            bool prior_record)
{
    char size[16];
    snprintf(size, sizeof(size), "%u", fs->block_size);
    const char *const values[TAG_COUNT] = {
        fs->fat_boot_label, fs->label, fs->uuid, size, fs->type,
        lowlevel ? "filesystem" : "",
    };
    bool printed = false;
    for (unsigned i = 0; i < TAG_COUNT; i++) {
        if (!(tags & (1u << i)) || !values[i][0])
            continue;
        if (!printed && format == FULL) {
            print_escaped(path, format);
            putchar(':');
        } else if (!printed && format == EXPORT) {
            if (prior_record)
                putchar('\n');
            fputs("DEVNAME=", stdout);
            print_escaped(path, format);
            putchar('\n');
        }
        printed = true;
        if (format == FULL) {
            printf(" %s=\"", tag_names[i]);
            print_escaped(values[i], format);
            putchar('"');
        } else if (format == EXPORT) {
            printf("%s=", tag_names[i]);
            print_escaped(values[i], format);
            putchar('\n');
        } else {
            puts(values[i]);
        }
    }
    if (printed && format == FULL)
        putchar('\n');
    return printed;
}

int bx_blkid_main(int argc, char **argv)
{
    static const struct option options[] = {
        {"help", no_argument, NULL, 'h'},
        {"probe", no_argument, NULL, 'p'},
        {"usages", required_argument, NULL, 'u'},
        {"output", required_argument, NULL, 'o'},
        {"match-tag", required_argument, NULL, 's'},
        {NULL, 0, NULL, 0},
    };
    struct bx_diag_ctx diag = {.progname = "blkid"};
    enum output format = FULL;
    unsigned tags = 0;
    bool filtered = false, lowlevel = false;
    int opt;
    while ((opt = getopt_long(argc, argv, "+hpu:o:s:", options, NULL)) != -1) {
        switch (opt) {
        case 'h':
            usage(stdout);
            return 0;
        case 'p':
            lowlevel = true;
            break;
        case 'u':
            if (strcmp(optarg, "filesystem") != 0) {
                bx_diag(&diag, "unsupported usage: '%s'", optarg);
                return 4;
            }
            break;
        case 'o':
            if (strcmp(optarg, "full") == 0)
                format = FULL;
            else if (strcmp(optarg, "export") == 0)
                format = EXPORT;
            else if (strcmp(optarg, "value") == 0)
                format = VALUE;
            else {
                bx_diag(&diag, "unsupported output format: '%s'", optarg);
                return 4;
            }
            break;
        case 's':
            filtered = true;
            for (unsigned i = 0; i < TAG_COUNT; i++)
                if (strcmp(optarg, tag_names[i]) == 0)
                    tags |= 1u << i;
            break;
        default:
            usage(stderr);
            return 4;
        }
    }
    if (optind == argc) {
        usage(stderr);
        return 4;
    }
    if (!filtered)
        tags = (1u << TAG_COUNT) - 1;

    bool found = false, ambiguous = false, prior_record = false;
    for (int i = optind; i < argc; i++) {
        const char *path = argv[i];
        int fd = bx_fd_open_cloexec(path, O_RDONLY | O_NONBLOCK | O_NOCTTY, 0);
        if (fd < 0) {
            bx_perror_path(&diag, path);
            continue;
        }
        struct bx_fs_signature fs;
        int match = bx_fs_probe(fd, &fs);
        if (match < 0)
            bx_perror_path(&diag, path);
        close(fd);
        if (match == 1) {
            found = true;
            if (print_signature(path, &fs, tags, format, lowlevel, prior_record))
                prior_record = true;
        } else if (match == 2 && lowlevel) {
            ambiguous = true;
            bx_diag(&diag, "%s: conflicting filesystem signatures", path);
        }
    }
    if (fflush(stdout) == EOF || ferror(stdout)) {
        bx_perror_path(&diag, "standard output");
        return 4;
    }
    return ambiguous ? 8 : found ? 0 : 2;
}
