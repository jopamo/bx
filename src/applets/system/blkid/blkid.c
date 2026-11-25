/* SPDX-License-Identifier: BSD-2-Clause */
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"
#include "lib/fd_ops.h"
#include "lib/fs_probe.h"
#include "lib/option_scan.h"
#include "scan.h"

enum output { FULL, EXPORT, VALUE, DEVICE };
enum tag { FAT_BOOT_LABEL, LABEL, UUID, BLOCK_SIZE, VERSION, TYPE, USAGE, TAG_COUNT };

static const char *const tag_names[TAG_COUNT] = {
    "LABEL_FATBOOT", "LABEL", "UUID", "BLOCK_SIZE", "VERSION", "TYPE", "USAGE",
};

static void usage(FILE *out)
{
    fputs("Usage: blkid [-p | [-l] -t TAG=value] [-u filesystem|other|crypto]"
          " [-o full|export|value|device] [-s TAG]... [--] [DEVICE...]\n"
          "Probe block devices or images for FAT, ext, ISO 9660, SquashFS, EROFS, Linux swap, or LUKS.\n"
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

static const char *usage_name(enum bx_probe_usage usage)
{
    switch (usage) {
    case BX_PROBE_FILESYSTEM: return "filesystem";
    case BX_PROBE_OTHER: return "other";
    case BX_PROBE_CRYPTO: return "crypto";
    }
    return "";
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
    if (fs->block_size)
        snprintf(size, sizeof(size), "%u", fs->block_size);
    else
        size[0] = 0;
    const char *const values[TAG_COUNT] = {
        fs->fat_boot_label, fs->label, fs->uuid, size,
        lowlevel && fs->version ? fs->version : "", fs->type,
        lowlevel ? usage_name(fs->usage) : "",
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
    unsigned tags, usages;
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
    int match = bx_fs_probe(fd, run->usages, &fs);
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
        bx_diag(&run->diag, "%s: conflicting volume signatures", path);
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

static int blkid_command(int argc, char **argv, const char **paths)
{
    static const struct bx_option_spec options[] = {
        {'h', 'h', "help", BX_OPTION_NO_VALUE},
        {'p', 'p', "probe", BX_OPTION_NO_VALUE},
        {'u', 'u', "usages", BX_OPTION_REQUIRED_VALUE},
        {'o', 'o', "output", BX_OPTION_REQUIRED_VALUE},
        {'s', 's', "match-tag", BX_OPTION_REQUIRED_VALUE},
        {'t', 't', "match-token", BX_OPTION_REQUIRED_VALUE},
        {'l', 'l', "list-one", BX_OPTION_NO_VALUE},
    };
    struct probe_run run = {.diag = {.progname = "blkid"}, .format = FULL};
    bool filtered = false;
    size_t count = 0;
    struct bx_option_cursor cursor;
    bx_option_cursor_init(&cursor, argv, 1, (size_t)argc, BX_OPTION_ABBREVIATE,
                          options, sizeof(options) / sizeof(options[0]));
    for (;;) {
        struct bx_option_result result = bx_option_next(&cursor);
        if (result.event == BX_OPTION_END)
            break;
        if (result.event == BX_OPTION_OPERAND) {
            paths[count++] = result.token;
            continue;
        }
        if (result.event != BX_OPTION_MATCH) {
            if (result.event == BX_OPTION_MISSING_VALUE)
                bx_diag(&run.diag, "option requires an argument -- '%c'", result.spec->short_name);
            else if (result.short_name)
                bx_diag(&run.diag, "invalid option -- '%c'", result.short_name);
            else
                bx_diag(&run.diag, "unrecognized option '%s'", result.token);
            usage(stderr);
            return 4;
        }
        switch (result.spec->id) {
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
            if (!parse_match(&run, result.value))
                return 4;
            break;
        case 'u': {
            unsigned selected;
            if (strcmp(result.value, "filesystem") == 0)
                selected = BX_PROBE_FILESYSTEM;
            else if (strcmp(result.value, "other") == 0)
                selected = BX_PROBE_OTHER;
            else if (strcmp(result.value, "crypto") == 0)
                selected = BX_PROBE_CRYPTO;
            else {
                bx_diag(&run.diag, "unsupported usage: '%s'", result.value);
                return 4;
            }
            if (run.usages && run.usages != selected) {
                bx_diag(&run.diag, "only one usage may be specified");
                return 4;
            }
            run.usages = selected;
            break;
        }
        case 'o':
            if (strcmp(result.value, "full") == 0)
                run.format = FULL;
            else if (strcmp(result.value, "export") == 0)
                run.format = EXPORT;
            else if (strcmp(result.value, "value") == 0)
                run.format = VALUE;
            else if (strcmp(result.value, "device") == 0)
                run.format = DEVICE;
            else {
                bx_diag(&run.diag, "unsupported output format: '%s'", result.value);
                return 4;
            }
            break;
        case 's':
            filtered = true;
            for (unsigned i = 0; i < TAG_COUNT; i++)
                if (strcmp(result.value, tag_names[i]) == 0)
                    run.tags |= 1u << i;
            break;
        }
    }
    run.scanning = count == 0;
    if ((run.lowlevel && (run.scanning || run.match_value)) ||
        (run.list_one && !run.match_value)) {
        usage(stderr);
        return 4;
    }
    if (!filtered)
        run.tags = (1u << TAG_COUNT) - 1;
    if (!run.usages)
        run.usages = BX_PROBE_ALL;

    if (run.scanning && bx_blkid_scan(probe_device, &run) < 0) {
        bx_perror_path(&run.diag, "scan block devices");
        run.failed = true;
    }
    for (size_t i = 0; i < count; i++) {
        const char *path = paths[i];
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

int bx_blkid_main(int argc, char **argv)
{
    const char **paths = NULL;
    if (argc > 1) {
        paths = calloc((size_t)argc - 1, sizeof(*paths));
        if (!paths) {
            struct bx_diag_ctx diag = {.progname = "blkid"};
            bx_perror_path(&diag, "device operands");
            return 4;
        }
    }
    int status = blkid_command(argc, argv, paths);
    free(paths);
    return status;
}
