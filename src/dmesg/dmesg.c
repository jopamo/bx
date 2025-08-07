#define _GNU_SOURCE

#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/klog.h>

#include "applets.h"
#include "bx/diag.h"
#include "bx/libbx.h"

#define BX_KLOG_ACTION_READ_ALL 3
#define BX_KLOG_ACTION_SIZE_BUFFER 10

struct bx_dmesg_options {
    const char* progname;
    bool show_help;
    bool show_version;
};

static const char* bx_dmesg_progname(const char* argv0) {
    if (argv0 == NULL || argv0[0] == '\0') {
        return "dmesg";
    }

    const char* base = strrchr(argv0, '/');
    if (base != NULL && base[1] != '\0') {
        return base + 1;
    }

    return argv0;
}

static void bx_dmesg_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]...\n", progname);
    fprintf(stream, "Print the kernel ring buffer.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -h, --help     display this help and exit\n");
    fprintf(stream, "  -V, --version  output version information and exit\n");
}

static void bx_dmesg_print_try_help(const char* progname) {
    fprintf(stderr, "Try '%s --help' for more information.\n", progname);
}

static void bx_dmesg_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

static bool bx_dmesg_parse_options(int argc, char** argv, struct bx_dmesg_options* options, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"help", no_argument, NULL, 'h'},
        {"version", no_argument, NULL, 'V'},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_dmesg_progname((argc > 0) ? argv[0] : NULL);
    diag->progname = options->progname;

    opterr = 0;
    optind = 1;

    while (true) {
        int c = getopt_long(argc, argv, "+hV", long_options, NULL);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 'h':
                options->show_help = true;
                return true;
            case 'V':
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

    if (optind < argc) {
        bx_diag(diag, "extra operand '%s'", argv[optind]);
        return false;
    }

    return true;
}

static bool bx_dmesg_print_buffer(struct bx_diag_ctx* diag) {
    int buffer_size = klogctl(BX_KLOG_ACTION_SIZE_BUFFER, NULL, 0);
    if (buffer_size < 0) {
        bx_diag(diag, "failed to query kernel log buffer size: %s", strerror(errno));
        return false;
    }

    if (buffer_size == 0) {
        return true;
    }

    char* buffer = xmalloc((size_t)buffer_size);
    int read_size = klogctl(BX_KLOG_ACTION_READ_ALL, buffer, buffer_size);
    if (read_size < 0) {
        bx_diag(diag, "failed to read kernel log buffer: %s", strerror(errno));
        free(buffer);
        return false;
    }

    if (read_size > 0) {
        size_t to_write = (size_t)read_size;
        if (fwrite(buffer, 1, to_write, stdout) != to_write) {
            bx_diag(diag, "failed to write output: %s", strerror(errno));
            free(buffer);
            return false;
        }
    }

    free(buffer);
    return true;
}

int bx_dmesg_main(int argc, char** argv) {
    struct bx_dmesg_options options;
    struct bx_diag_ctx diag = {
        .progname = "dmesg",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };

    if (!bx_dmesg_parse_options(argc, argv, &options, &diag)) {
        bx_dmesg_print_try_help(options.progname);
        return (diag.exit_status != 0) ? diag.exit_status : 1;
    }

    if (options.show_help) {
        bx_dmesg_print_help(stdout, options.progname);
        return 0;
    }

    if (options.show_version) {
        bx_dmesg_print_version(options.progname);
        return 0;
    }

    if (!bx_dmesg_print_buffer(&diag)) {
        return (diag.exit_status != 0) ? diag.exit_status : 1;
    }

    return 0;
}
