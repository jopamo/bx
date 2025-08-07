#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "applets.h"
#include "bx/diag.h"
#include "bx/libbx.h"

struct bx_dirname_options {
    const char* progname;
    bool zero_terminated;
    bool show_help;
    bool show_version;
};

static const char* bx_dirname_progname(const char* argv0) {
    if (argv0 == NULL || argv0[0] == '\0') {
        return "dirname";
    }

    const char* base = strrchr(argv0, '/');
    if (base != NULL && base[1] != '\0') {
        return base + 1;
    }
    return argv0;
}

static void bx_dirname_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION] NAME...\n", progname);
    fprintf(stream, "Output each NAME with its last non-slash component and trailing slashes\n");
    fprintf(stream, "removed; if NAME contains no /'s, output '.' (meaning the current directory).\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -z, --zero\n");
    fprintf(stream, "         end each output line with NUL, not newline\n");
    fprintf(stream, "      --help\n");
    fprintf(stream, "         display this help and exit\n");
    fprintf(stream, "      --version\n");
    fprintf(stream, "         output version information and exit\n");
    fprintf(stream, "\n");
    fprintf(stream, "Examples:\n");
    fprintf(stream, "  %s /usr/bin/          -> \"/usr\"\n", progname);
    fprintf(stream, "  %s dir1/str dir2/str  -> \"dir1\" followed by \"dir2\"\n", progname);
    fprintf(stream, "  %s stdio.h            -> \".\"\n", progname);
}

static void bx_dirname_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

static bool bx_dirname_parse_options(int argc, char** argv, struct bx_dirname_options* options, int* first_operand, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"zero", no_argument, NULL, 'z'},
        {"help", no_argument, NULL, 1},
        {"version", no_argument, NULL, 2},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_dirname_progname((argc > 0) ? argv[0] : NULL);
    diag->progname = options->progname;

    opterr = 0;
    optind = 1;

    while (true) {
        int option_index = 0;
        int c = getopt_long(argc, argv, "+z", long_options, &option_index);
        if (c == -1) {
            break;
        }

        switch (c) {
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

static char* bx_dirname_copy_range(const char* start, size_t len) {
    char* out = xmalloc(len + 1u);
    memcpy(out, start, len);
    out[len] = '\0';
    return out;
}

static char* bx_dirname_component_dup(const char* name) {
    if (name == NULL || name[0] == '\0') {
        return xstrdup(".");
    }

    size_t end = strlen(name);
    while (end > 0 && name[end - 1u] == '/') {
        end--;
    }

    if (end == 0) {
        return xstrdup("/");
    }

    size_t slash_index = end;
    while (slash_index > 0 && name[slash_index - 1u] != '/') {
        slash_index--;
    }

    if (slash_index == 0) {
        return xstrdup(".");
    }

    size_t dir_len = slash_index;
    while (dir_len > 1u && name[dir_len - 1u] == '/') {
        dir_len--;
    }

    return bx_dirname_copy_range(name, dir_len);
}

static bool bx_dirname_emit(const char* value, bool zero_terminated, struct bx_diag_ctx* diag) {
    if (fputs(value, stdout) == EOF) {
        bx_diag(diag, "write error: %s", strerror(errno));
        return false;
    }

    int delimiter = zero_terminated ? '\0' : '\n';
    if (fputc(delimiter, stdout) == EOF) {
        bx_diag(diag, "write error: %s", strerror(errno));
        return false;
    }

    return true;
}

static bool bx_dirname_process_name(const char* name, bool zero_terminated, struct bx_diag_ctx* diag) {
    char* value = bx_dirname_component_dup(name);
    bool ok = bx_dirname_emit(value, zero_terminated, diag);
    free(value);
    return ok;
}

int bx_dirname_main(int argc, char** argv) {
    struct bx_dirname_options options;
    struct bx_diag_ctx diag = {
        .progname = "dirname",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };
    int first_operand = 0;

    if (!bx_dirname_parse_options(argc, argv, &options, &first_operand, &diag)) {
        return diag.exit_status != 0 ? diag.exit_status : 1;
    }

    if (options.show_help) {
        bx_dirname_print_help(stdout, options.progname);
        return 0;
    }

    if (options.show_version) {
        bx_dirname_print_version(options.progname);
        return 0;
    }

    int operand_count = argc - first_operand;
    if (operand_count <= 0) {
        bx_diag(&diag, "missing operand");
        return diag.exit_status;
    }

    for (int i = first_operand; i < argc; i++) {
        if (!bx_dirname_process_name(argv[i], options.zero_terminated, &diag)) {
            return diag.exit_status;
        }
    }

    if (fflush(stdout) == EOF) {
        bx_diag(&diag, "write error: %s", strerror(errno));
    }

    return diag.exit_status;
}
