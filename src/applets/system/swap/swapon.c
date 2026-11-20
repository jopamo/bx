#define _GNU_SOURCE

#include <stdio.h>

#include "applets.h"
#include "bx/diag.h"
#include "lib/cli_common.h"

static void usage(FILE* out, const char* name) {
    fprintf(out, "Usage: %s [-p PRIORITY] [--] DEVICE|FILE|TAG ...\n"
                 "Enable explicitly named Linux swap areas. Use -1 for the default priority.\n"
                 "Tags: LABEL, UUID, PARTLABEL, PARTUUID (TAG=VALUE).\n", name);
}

#ifdef __linux__
#include <getopt.h>
#include <stdlib.h>
#include <string.h>
#include <sys/swap.h>

#include "lib/disk_tag.h"

static int swap_priority_flags(const char* text) {
    if (strcmp(text, "-1") == 0)
        return 0;
    if (*text == '\0' || text[strspn(text, "0123456789")] != '\0')
        return -1;
    errno = 0;
    unsigned long priority = strtoul(text, NULL, 10);
    if (errno == ERANGE || priority > (SWAP_FLAG_PRIO_MASK >> SWAP_FLAG_PRIO_SHIFT))
        return -1;
    return SWAP_FLAG_PREFER | (int)(priority << SWAP_FLAG_PRIO_SHIFT);
}
#endif

int bx_swapon_main(int argc, char** argv) {
    int handled = bx_cli_maybe_handle_help_or_version(argc, argv, "swapon", "-h", "-V", usage);
    if (handled >= 0)
        return handled;
    struct bx_diag_ctx diag = {.progname = "swapon"};
#ifdef __linux__
    static const struct option opts[] = {
        {"priority", required_argument, NULL, 'p'}, {NULL, 0, NULL, 0},
    };
    int flags = 0;
    optind = 1;
    opterr = 0;
    for (int opt; (opt = getopt_long(argc, argv, "+:p:", opts, NULL)) != -1;) {
        if (opt == 'p') {
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
