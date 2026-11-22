#define _GNU_SOURCE

#include <stdio.h>

#include "applets.h"
#include "bx/diag.h"
#include "lib/cli_common.h"

static void usage(FILE* out, const char* name) {
    fprintf(out, "Usage: %s [-p PRIORITY] [--] DEVICE|FILE|TAG ...\n"
                 "       %s -a [-T FILE]\n"
                 "Enable explicitly named Linux swap areas. Use -1 for the default priority.\n"
                 "-a enables swap from /etc/fstab (or -T FILE), respecting noauto, nofail and pri=.\n"
                 "Tags: LABEL, UUID, PARTLABEL, PARTUUID (TAG=VALUE).\n", name, name);
}

#ifdef __linux__
#include <getopt.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/swap.h>

#include "lib/args_common.h"
#include "lib/disk_tag.h"
#include "lib/mount_table.h"
#include "lib/size_parse.h"
#include "lib/swap_table.h"

static int swap_priority_flags(const char* text) {
    if (strcmp(text, "-1") == 0)
        return 0;
    uintmax_t priority;
    if (!bx_size_parse_uint(text, &priority) ||
        priority > (SWAP_FLAG_PRIO_MASK >> SWAP_FLAG_PRIO_SHIFT))
        return -1;
    return SWAP_FLAG_PREFER | (int)(priority << SWAP_FLAG_PRIO_SHIFT);
}

struct swap_options {
    int flags;
    bool skip;
    bool nofail;
};

static bool swap_parse_fstab_options(char* list, struct swap_options* options) {
    if (strpbrk(list, "\"'") != NULL)
        return false;
    bool valid = true;
    char* save;
    for (char* token = strtok_r(list, ",", &save); token; token = strtok_r(NULL, ",", &save)) {
        if (strcmp(token, "noauto") == 0)
            options->skip = true;
        else if (strcmp(token, "auto") == 0)
            options->skip = false;
        else if (strcmp(token, "nofail") == 0)
            options->nofail = true;
        else if (strncmp(token, "pri=", 4) == 0) {
            options->flags = swap_priority_flags(token + 4);
            valid = valid && options->flags >= 0;
        } else if (strcmp(token, "defaults") != 0 && strcmp(token, "sw") != 0)
            valid = false;
    }
    return options->skip || valid;
}

/* EBUSY can also mean a block device is claimed for another purpose. */
static bool swap_is_active(const char* path) {
    struct stat target;
    if (stat(path, &target) < 0)
        return false;
    struct bx_swap_table active;
    if (!bx_swap_table_load(&active))
        return false;
    bool found = false;
    for (size_t i = 0; i < active.len && !found; i++) {
        struct stat entry;
        if (stat(active.paths[i], &entry) < 0)
            continue;
        if (S_ISBLK(target.st_mode))
            found = S_ISBLK(entry.st_mode) && target.st_rdev == entry.st_rdev;
        else if (S_ISREG(target.st_mode))
            found = S_ISREG(entry.st_mode) && target.st_dev == entry.st_dev && target.st_ino == entry.st_ino;
    }
    bx_swap_table_free(&active);
    return found;
}

static bool swap_fstab_source_valid(const char* spec) {
    if (*spec == '/')
        return true;
    const char* value = bx_disk_tag_value(spec);
    return value && *value;
}

static int swap_enable_fstab(const char* path, struct bx_diag_ctx* diag) {
    struct bx_mount_table table;
    size_t line;
    if (!bx_mount_table_load_fstab(&table, path, &line)) {
        if (line)
            bx_diag(diag, "%s:%zu: cannot read fstab record: %s", path, line, bx_strerror(errno));
        else
            bx_perror_path(diag, path);
        return 1;
    }
    struct swap_options* plan = calloc(table.len ? table.len : 1, sizeof(*plan));
    int status = 1;
    if (!plan) {
        bx_perror_path(diag, "swap plan");
        goto done;
    }
    for (size_t i = 0; i < table.len; i++) {
        struct bx_mount_entry* entry = &table.entries[i];
        struct swap_options* opts = &plan[i];
        opts->skip = strcmp(entry->fstype, "swap") != 0;
        if (!opts->skip && (!swap_parse_fstab_options(entry->options, opts) ||
                           (!opts->skip && !swap_fstab_source_valid(entry->source)))) {
            bx_diag(diag, "%s:%zu: unsupported swap entry for '%s'", path, entry->line, entry->source);
            goto done;
        }
    }
    status = 0;
    for (size_t i = 0; i < table.len; i++) {
        if (plan[i].skip)
            continue;
        const char* spec = table.entries[i].source;
        char* resolved = NULL;
        int error = 0;
        if (bx_disk_tag_resolve(spec, &resolved) < 0) {
            error = errno;
        } else {
            const char* device = resolved ? resolved : spec;
            struct stat st;
            if (stat(device, &st) < 0)
                error = errno;
            else if (!S_ISBLK(st.st_mode) && !S_ISREG(st.st_mode))
                error = EINVAL;
            else if (swapon(device, plan[i].flags) < 0) {
                error = errno;
                if (error == EBUSY && swap_is_active(device))
                    error = 0;
            }
        }
        free(resolved);
        if (error && !(plan[i].nofail && (error == ENOENT || error == ENOTDIR))) {
            bx_diag(diag, "'%s': %s", spec, bx_strerror(error));
            status = 1;
        }
    }
done:
    free(plan);
    bx_mount_table_free(&table);
    return status;
}
#endif

int bx_swapon_main(int argc, char** argv) {
    int handled = bx_cli_maybe_handle_help_or_version(argc, argv, "swapon", "-h", "-V", usage);
    if (handled >= 0)
        return handled;
    struct bx_diag_ctx diag = {.progname = "swapon"};
#ifdef __linux__
    static const struct option opts[] = {
        {"all", no_argument, NULL, 'a'},
        {"fstab", required_argument, NULL, 'T'},
        {"priority", required_argument, NULL, 'p'}, {NULL, 0, NULL, 0},
    };
    int flags = 0;
    bool all = false;
    bool priority = false;
    const char* fstab = NULL;
    bx_args_getopt_reset();
    for (int opt; (opt = bx_args_getopt_long(argc, argv, "+:aT:p:", opts, NULL)) != -1;) {
        if (opt == 'a') {
            all = true;
        } else if (opt == 'T') {
            fstab = optarg;
        } else if (opt == 'p') {
            priority = true;
            flags = swap_priority_flags(optarg);
            if (flags < 0) {
                bx_diag(&diag, "invalid priority: '%s'", optarg);
                return 1;
            }
        } else {
            if (opt == ':')
                bx_cli_diag_option_requires_arg(&diag, optopt, optind, argc, argv);
            else
                bx_cli_diag_unrecognized_option(&diag, optopt, optind, argc, argv);
            return 1;
        }
    }
    if (all || fstab) {
        if (!all || (fstab && !*fstab) || priority || optind != argc) {
            bx_diag(&diag, "use -a with only -T and no operands");
            return 1;
        }
        return swap_enable_fstab(fstab ? fstab : "/etc/fstab", &diag);
    }
    if (optind == argc) {
        bx_diag(&diag, "a swap area is required");
        return 1;
    }
    size_t count = (size_t)(argc - optind);
    char** devices = calloc(count, sizeof(*devices));
    if (devices == NULL) {
        bx_perror_path(&diag, "swap areas");
        return 1;
    }
    char** specs = argv + optind;
    int status = 1;
    /* Validate every operand and resolve tags before asking the kernel to activate swap. */
    for (size_t i = 0; i < count; i++) {
        if (*specs[i] == '\0' || *specs[i] == '-') {
            bx_diag(&diag, "invalid swap area: '%s'", specs[i]);
            goto done;
        }
        if (bx_disk_tag_resolve(specs[i], &devices[i]) < 0) {
            bx_diag(&diag, "cannot resolve '%s': %s", specs[i], bx_strerror(errno));
            goto done;
        }
    }
    for (size_t i = 0; i < count; i++) {
        if (swapon(devices[i] ? devices[i] : specs[i], flags) < 0) {
            bx_perror_path(&diag, specs[i]);
            goto done;
        }
    }
    status = 0;
done:
    for (size_t i = 0; i < count; i++)
        free(devices[i]);
    free(devices);
    return status;
#else
    bx_diag(&diag, "Linux swap activation is not supported on this platform");
    return 1;
#endif
}
