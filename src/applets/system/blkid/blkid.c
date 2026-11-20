/* SPDX-License-Identifier: BSD-2-Clause */
#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"
#include "lib/fd_ops.h"
#include "lib/fs_probe.h"
#include "scan.h"

enum output { FULL, EXPORT, VALUE, DEVICE };
enum tag { FAT_BOOT_LABEL, LABEL, UUID, BLOCK_SIZE, TYPE, USAGE, TAG_COUNT };

static const char *const tag_names[TAG_COUNT] = {
    "LABEL_FATBOOT", "LABEL", "UUID", "BLOCK_SIZE", "TYPE", "USAGE",
};

static void usage(FILE *out)
{
    fputs("Usage: blkid [-p | [-l] -t TAG=value] [-u filesystem]"
          " [-o full|export|value|device] [-s TAG]... [--] [DEVICE...]\n"
          "Probe block devices or images for FAT, ext and ISO 9660 filesystems.\n"
          "With no devices, scan Linux block devices. -p requires a device.\n"
          "-t matches an exact LABEL or UUID. -l requires one matching path.\n", out);
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
    if (format == DEVICE) {
        puts(path);
        return true;
    }

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

struct probe_run {
    struct bx_diag_ctx diag;
    enum output format;
    unsigned tags;
    const char *match_value;
    enum tag match_tag;
    bool list_one, lowlevel, scanning, found, ambiguous, prior_record;
    bool duplicate, failed;
    char *first_path;
    struct bx_fs_signature first_signature;
};

static bool remember_match(struct probe_run *run, const char *path,
                           const struct bx_fs_signature *fs)
{
    if (run->first_path) {
        if (strcmp(run->first_path, path) == 0)
            return true;
        run->duplicate = true;
        bx_diag(&run->diag, "multiple matching device paths");
        return false;
    }
    run->first_path = strdup(path);
    if (!run->first_path) {
        bx_perror_path(&run->diag, path);
        run->failed = true;
        return false;
    }
    run->first_signature = *fs;
    return true;
}

static bool probe_device(int fd, const char *path, void *ctx)
{
    struct probe_run *run = ctx;
    struct bx_fs_signature fs;
    int match = bx_fs_probe(fd, &fs);
    if (match < 0 && !run->scanning)
        bx_perror_path(&run->diag, path);
    if (match == 1) {
        if (run->match_value) {
            const char *actual = run->match_tag == LABEL ? fs.label : fs.uuid;
            if (strcmp(actual, run->match_value) != 0)
                return true;
        }
        if (run->list_one)
            return remember_match(run, path, &fs);

        run->found = true;
        if (print_signature(path, &fs, run->tags, run->format,
                            run->lowlevel, run->prior_record))
            run->prior_record = true;
    } else if (match == 2 && run->lowlevel) {
        run->ambiguous = true;
        bx_diag(&run->diag, "%s: conflicting filesystem signatures", path);
    }
    return !ferror(stdout);
}

static bool parse_match(struct probe_run *run, const char *token)
{
    if (run->match_value) {
        bx_diag(&run->diag, "only one match token may be specified");
        return false;
    }
    const char *value = strchr(token, '=');
    if (!value || value == token || !value[1]) {
        bx_diag(&run->diag, "match token requires a nonempty TAG=value pair");
        return false;
    }
    size_t len = (size_t)(value - token);
    if (len == 5 && memcmp(token, "LABEL", len) == 0)
        run->match_tag = LABEL;
    else if (len == 4 && memcmp(token, "UUID", len) == 0)
        run->match_tag = UUID;
    else {
        bx_diag(&run->diag, "unsupported match tag; expected LABEL or UUID");
        return false;
    }
    run->match_value = value + 1;
    return true;
}

int bx_blkid_main(int argc, char **argv)
{
    static const struct option options[] = {
        {"help", no_argument, NULL, 'h'},
        {"probe", no_argument, NULL, 'p'},
        {"usages", required_argument, NULL, 'u'},
        {"output", required_argument, NULL, 'o'},
        {"match-tag", required_argument, NULL, 's'},
        {"match-token", required_argument, NULL, 't'},
        {"list-one", no_argument, NULL, 'l'},
        {NULL, 0, NULL, 0},
    };
    struct probe_run run = {.diag = {.progname = "blkid"}, .format = FULL};
    bool filtered = false;
    int opt;
    while ((opt = getopt_long(argc, argv, "+hlpu:o:s:t:", options, NULL)) != -1) {
        switch (opt) {
        case 'h':
            usage(stdout);
            return 0;
        case 'p':
            run.lowlevel = true;
            break;
        case 'l':
            run.list_one = true;
            break;
        case 't':
            if (!parse_match(&run, optarg))
                return 4;
            break;
        case 'u':
            if (strcmp(optarg, "filesystem") != 0) {
                bx_diag(&run.diag, "unsupported usage: '%s'", optarg);
                return 4;
            }
            break;
        case 'o':
            if (strcmp(optarg, "full") == 0)
                run.format = FULL;
            else if (strcmp(optarg, "export") == 0)
                run.format = EXPORT;
            else if (strcmp(optarg, "value") == 0)
                run.format = VALUE;
            else if (strcmp(optarg, "device") == 0)
                run.format = DEVICE;
            else {
                bx_diag(&run.diag, "unsupported output format: '%s'", optarg);
                return 4;
            }
            break;
        case 's':
            filtered = true;
            for (unsigned i = 0; i < TAG_COUNT; i++)
                if (strcmp(optarg, tag_names[i]) == 0)
                    run.tags |= 1u << i;
            break;
        default:
            usage(stderr);
            return 4;
        }
    }
    run.scanning = optind == argc;
    if ((run.lowlevel && (run.scanning || run.match_value)) ||
        (run.list_one && !run.match_value)) {
        usage(stderr);
        return 4;
    }
    if (!filtered)
        run.tags = (1u << TAG_COUNT) - 1;

    if (run.scanning && bx_blkid_scan(probe_device, &run) < 0) {
        bx_perror_path(&run.diag, "scan block devices");
        run.failed = true;
    }
    for (int i = optind; i < argc; i++) {
        const char *path = argv[i];
        int fd = bx_fd_open_cloexec(path, O_RDONLY | O_NONBLOCK | O_NOCTTY, 0);
        if (fd < 0) {
            bx_perror_path(&run.diag, path);
            continue;
        }
        bool more = probe_device(fd, path, &run);
        close(fd);
        if (!more)
            break;
    }
    if (run.first_path) {
        if (!run.duplicate && !run.failed) {
            run.found = true;
            print_signature(run.first_path, &run.first_signature, run.tags,
                            run.format, false, false);
        }
        free(run.first_path);
    }
    if (fflush(stdout) == EOF || ferror(stdout)) {
        bx_perror_path(&run.diag, "standard output");
        return 4;
    }
    return run.failed ? 4 : run.ambiguous ? 8 : run.found ? 0 : 2;
}
