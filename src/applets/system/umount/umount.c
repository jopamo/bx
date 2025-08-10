#define _DEFAULT_SOURCE

#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>

#include "applets.h"
#include "bx/diag.h"
#include "bx/libbx.h"

#ifndef MNT_FORCE
#define MNT_FORCE 0
#endif

struct bx_umount_options {
    const char* progname;
    bool show_help;
    bool show_version;
    bool force;
    bool lazy;
    int operand_index;
    const char* target;
};

static const char* bx_umount_progname(const char* argv0) {
    if (argv0 == NULL || argv0[0] == '\0') {
        return "umount";
    }

    const char* base = strrchr(argv0, '/');
    if (base != NULL && base[1] != '\0') {
        return base + 1;
    }

    return argv0;
}

static void bx_umount_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... TARGET\n", progname);
    fprintf(stream, "Unmount a filesystem at TARGET.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -f, --force    force unmount (filesystem support required)\n");
    fprintf(stream, "  -l, --lazy     detach filesystem now, clean up references later\n");
    fprintf(stream, "      --help     display this help and exit\n");
    fprintf(stream, "      --version  output version information and exit\n");
    fprintf(stream, "\n");
    fprintf(stream, "This phase supports explicit TARGET unmounting only.\n");
}

static void bx_umount_print_try_help(const char* progname) {
    fprintf(stderr, "Try '%s --help' for more information.\n", progname);
}

static void bx_umount_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

static bool bx_umount_parse_options(int argc, char** argv, struct bx_umount_options* options, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"force", no_argument, NULL, 'f'}, {"lazy", no_argument, NULL, 'l'}, {"help", no_argument, NULL, 1}, {"version", no_argument, NULL, 2}, {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_umount_progname((argc > 0) ? argv[0] : NULL);
    diag->progname = options->progname;

    opterr = 0;
    optind = 1;

    while (true) {
        int c = getopt_long(argc, argv, "+fl", long_options, NULL);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 'f':
                options->force = true;
                break;
            case 'l':
                options->lazy = true;
                break;
            case 1:
                options->show_help = true;
                return true;
            case 2:
                options->show_version = true;
                return true;
            case '?':
                if (optopt != 0) {
                    bx_diag(diag, "invalid option -- '%c'", optopt);
                }
                else if (optind > 0 && optind <= argc && argv[optind - 1] != NULL) {
                    bx_diag(diag, "unrecognized option '%s'", argv[optind - 1]);
                }
                else {
                    bx_diag(diag, "unrecognized option");
                }
                return false;
            default:
                return false;
        }
    }

    options->operand_index = optind;
    return true;
}

static bool bx_umount_validate_request(int argc, char** argv, struct bx_umount_options* options, struct bx_diag_ctx* diag) {
    int remaining = argc - options->operand_index;
    if (remaining <= 0) {
        bx_diag(diag, "missing target operand");
        return false;
    }
    if (remaining > 1) {
        bx_diag(diag, "too many operands");
        return false;
    }

    options->target = argv[options->operand_index];
    if (options->target == NULL || options->target[0] == '\0') {
        bx_diag(diag, "target may not be empty");
        return false;
    }

    return true;
}

static bool bx_umount_perform(const struct bx_umount_options* options, struct bx_diag_ctx* diag) {
    int flags = 0;
    if (options->force) {
        flags |= MNT_FORCE;
    }
    if (options->lazy) {
        flags |= MNT_DETACH;
    }
    if (umount2(options->target, flags) == 0) {
        return true;
    }

    int umount_error = errno;
    if (umount_error == EBUSY) {
        bx_diag(diag, "cannot unmount '%s': target is busy: %s", options->target, strerror(umount_error));
    }
    else if (umount_error == ENOENT || umount_error == ENOTDIR) {
        bx_diag(diag, "cannot unmount '%s': mount target not found: %s", options->target, strerror(umount_error));
    }
    else {
        bx_diag(diag, "cannot unmount '%s': %s", options->target, strerror(umount_error));
    }

    return false;
}

int bx_umount_main(int argc, char** argv) {
    struct bx_umount_options options;
    struct bx_diag_ctx diag = {
        .progname = "umount",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };

    if (!bx_umount_parse_options(argc, argv, &options, &diag)) {
        bx_umount_print_try_help(options.progname);
        return 1;
    }

    if (options.show_help) {
        bx_umount_print_help(stdout, options.progname);
        return 0;
    }

    if (options.show_version) {
        bx_umount_print_version(options.progname);
        return 0;
    }

    if (!bx_umount_validate_request(argc, argv, &options, &diag)) {
        bx_umount_print_try_help(options.progname);
        return 1;
    }

    if (!bx_umount_perform(&options, &diag)) {
        return (diag.exit_status != 0) ? diag.exit_status : 1;
    }

    return 0;
}
