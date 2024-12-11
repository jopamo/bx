#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "applets.h"
#include "diag.h"
#include "libbx.h"

struct bx_readlink_options {
    const char* progname;
    bool no_newline;
    bool zero_terminated;
    bool show_help;
    bool show_version;
};

static const char* bx_readlink_progname(const char* argv0) {
    if (argv0 == NULL || argv0[0] == '\0') {
        return "readlink";
    }

    const char* base = strrchr(argv0, '/');
    if (base != NULL && base[1] != '\0') {
        return base + 1;
    }
    return argv0;
}

static void bx_readlink_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... FILE...\n", progname);
    fprintf(stream, "Print value of each symbolic link FILE.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -n, --no-newline  do not output the trailing delimiter\n");
    fprintf(stream, "  -z, --zero        end each output line with NUL, not newline\n");
    fprintf(stream, "      --help        display this help and exit\n");
    fprintf(stream, "      --version     output version information and exit\n");
}

static void bx_readlink_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

static bool bx_readlink_parse_options(int argc, char** argv, struct bx_readlink_options* options, int* first_operand, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"no-newline", no_argument, NULL, 'n'}, {"zero", no_argument, NULL, 'z'}, {"help", no_argument, NULL, 1}, {"version", no_argument, NULL, 2}, {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_readlink_progname((argc > 0) ? argv[0] : NULL);
    diag->progname = options->progname;

    opterr = 0;
    optind = 1;

    while (true) {
        int option_index = 0;
        int c = getopt_long(argc, argv, "+nz", long_options, &option_index);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 'n':
                options->no_newline = true;
                break;
            case 'z':
                options->zero_terminated = true;
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

    *first_operand = optind;
    return true;
}

static bool bx_readlink_read_target(const char* path, char** target_out, struct bx_diag_ctx* diag) {
    size_t buffer_size = 128u;

    while (true) {
        char* buffer = xmalloc(buffer_size + 1u);
        ssize_t len = readlink(path, buffer, buffer_size);

        if (len < 0) {
            free(buffer);
            bx_diag(diag, "cannot read link '%s': %s", path, strerror(errno));
            return false;
        }

        if ((size_t)len < buffer_size) {
            buffer[(size_t)len] = '\0';
            *target_out = buffer;
            return true;
        }

        free(buffer);
        if (buffer_size > ((size_t)-1) / 2u) {
            bx_diag(diag, "cannot read link '%s': symbolic link value too large", path);
            return false;
        }
        buffer_size *= 2u;
    }
}

static bool bx_readlink_emit_target(const char* target, bool no_newline, bool zero_terminated, struct bx_diag_ctx* diag) {
    if (fputs(target, stdout) == EOF) {
        bx_diag(diag, "write error: %s", strerror(errno));
        return false;
    }

    if (!no_newline && fputc(zero_terminated ? '\0' : '\n', stdout) == EOF) {
        bx_diag(diag, "write error: %s", strerror(errno));
        return false;
    }

    return true;
}

int bx_readlink_main(int argc, char** argv) {
    struct bx_readlink_options options;
    struct bx_diag_ctx diag = {
        .progname = "readlink",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };
    int first_operand = 0;

    if (!bx_readlink_parse_options(argc, argv, &options, &first_operand, &diag)) {
        return diag.exit_status != 0 ? diag.exit_status : 1;
    }

    if (options.show_help) {
        bx_readlink_print_help(stdout, options.progname);
        return 0;
    }

    if (options.show_version) {
        bx_readlink_print_version(options.progname);
        return 0;
    }

    int operand_count = argc - first_operand;
    if (operand_count <= 0) {
        bx_diag(&diag, "missing operand");
        return diag.exit_status;
    }

    bool no_newline = options.no_newline;
    if (no_newline && operand_count > 1) {
        fprintf(stderr, "%s: ignoring --no-newline with multiple arguments\n", options.progname);
        no_newline = false;
    }

    char** operands = argv + first_operand;
    for (int i = 0; i < operand_count; i++) {
        char* target = NULL;
        if (!bx_readlink_read_target(operands[i], &target, &diag)) {
            continue;
        }

        if (!bx_readlink_emit_target(target, no_newline, options.zero_terminated, &diag)) {
            free(target);
            return diag.exit_status;
        }

        free(target);
    }

    if (fflush(stdout) == EOF) {
        bx_diag(&diag, "write error: %s", strerror(errno));
    }

    return diag.exit_status;
}
