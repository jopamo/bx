#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "applets.h"
#include "bx/diag.h"
#include "bx/libbx.h"

struct bx_basename_options {
    const char* progname;
    bool multiple;
    const char* suffix;
    bool zero_terminated;
    bool show_help;
    bool show_version;
};

static const char* bx_basename_progname(const char* argv0) {
    if (argv0 == NULL || argv0[0] == '\0') {
        return "basename";
    }

    const char* base = strrchr(argv0, '/');
    if (base != NULL && base[1] != '\0') {
        return base + 1;
    }
    return argv0;
}

static void bx_basename_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s NAME [SUFFIX]\n", progname);
    fprintf(stream, "  or:  %s OPTION... NAME...\n", progname);
    fprintf(stream, "Print NAME with any leading directory components removed.\n");
    fprintf(stream, "If specified, also remove a trailing SUFFIX.\n");
    fprintf(stream, "\n");
    fprintf(stream, "Mandatory arguments to long options are mandatory for short options too.\n");
    fprintf(stream, "  -a, --multiple\n");
    fprintf(stream, "         support multiple arguments and treat each as a NAME\n");
    fprintf(stream, "  -s, --suffix=SUFFIX\n");
    fprintf(stream, "         remove a trailing SUFFIX; implies -a\n");
    fprintf(stream, "  -z, --zero\n");
    fprintf(stream, "         end each output line with NUL, not newline\n");
    fprintf(stream, "      --help\n");
    fprintf(stream, "         display this help and exit\n");
    fprintf(stream, "      --version\n");
    fprintf(stream, "         output version information and exit\n");
    fprintf(stream, "\n");
    fprintf(stream, "Examples:\n");
    fprintf(stream, "  %s /usr/bin/sort          -> \"sort\"\n", progname);
    fprintf(stream, "  %s include/stdio.h .h     -> \"stdio\"\n", progname);
    fprintf(stream, "  %s -s .h include/stdio.h  -> \"stdio\"\n", progname);
    fprintf(stream, "  %s -a any/str1 any/str2   -> \"str1\" followed by \"str2\"\n", progname);
}

static void bx_basename_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

static bool bx_basename_parse_options(int argc, char** argv, struct bx_basename_options* options, int* first_operand, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"multiple", no_argument, NULL, 'a'}, {"suffix", required_argument, NULL, 's'}, {"zero", no_argument, NULL, 'z'},
        {"help", no_argument, NULL, 1},       {"version", no_argument, NULL, 2},        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_basename_progname((argc > 0) ? argv[0] : NULL);
    diag->progname = options->progname;

    opterr = 0;
    optind = 1;

    while (true) {
        int option_index = 0;
        int c = getopt_long(argc, argv, "+:as:z", long_options, &option_index);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 'a':
                options->multiple = true;
                break;
            case 's':
                options->suffix = optarg;
                options->multiple = true;
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
            case ':':
                if (optopt != 0) {
                    bx_diag(diag, "option requires an argument -- '%c'", optopt);
                }
                else {
                    bx_diag(diag, "option requires an argument");
                }
                return false;
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

static char* bx_basename_copy_range(const char* start, size_t len) {
    char* out = xmalloc(len + 1u);
    memcpy(out, start, len);
    out[len] = '\0';
    return out;
}

static char* bx_basename_component_dup(const char* name) {
    if (name == NULL || name[0] == '\0') {
        return xstrdup("");
    }

    const char* end = name + strlen(name);
    while (end > name + 1 && end[-1] == '/') {
        end--;
    }

    const char* base = end;
    while (base > name && base[-1] != '/') {
        base--;
    }

    if (base == end) {
        return xstrdup("/");
    }
    return bx_basename_copy_range(base, (size_t)(end - base));
}

static void bx_basename_strip_suffix(char* value, const char* suffix) {
    if (suffix == NULL || suffix[0] == '\0') {
        return;
    }

    size_t value_len = strlen(value);
    size_t suffix_len = strlen(suffix);

    if (suffix_len > value_len || strcmp(value, suffix) == 0) {
        return;
    }

    if (memcmp(value + value_len - suffix_len, suffix, suffix_len) == 0) {
        value[value_len - suffix_len] = '\0';
    }
}

static bool bx_basename_emit(const char* value, bool zero_terminated, struct bx_diag_ctx* diag) {
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

static bool bx_basename_process_name(const char* name, const char* suffix, bool zero_terminated, struct bx_diag_ctx* diag) {
    char* value = bx_basename_component_dup(name);
    bx_basename_strip_suffix(value, suffix);

    bool ok = bx_basename_emit(value, zero_terminated, diag);
    free(value);
    return ok;
}

int bx_basename_main(int argc, char** argv) {
    struct bx_basename_options options;
    struct bx_diag_ctx diag = {
        .progname = "basename",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };
    int first_operand = 0;

    if (!bx_basename_parse_options(argc, argv, &options, &first_operand, &diag)) {
        return diag.exit_status != 0 ? diag.exit_status : 1;
    }

    if (options.show_help) {
        bx_basename_print_help(stdout, options.progname);
        return 0;
    }

    if (options.show_version) {
        bx_basename_print_version(options.progname);
        return 0;
    }

    int operand_count = argc - first_operand;
    if (operand_count <= 0) {
        bx_diag(&diag, "missing operand");
        return diag.exit_status;
    }

    if (options.multiple) {
        for (int i = first_operand; i < argc; i++) {
            if (!bx_basename_process_name(argv[i], options.suffix, options.zero_terminated, &diag)) {
                return diag.exit_status;
            }
        }
    }
    else {
        if (operand_count > 2) {
            bx_diag(&diag, "extra operand '%s'", argv[first_operand + 2]);
            return diag.exit_status;
        }

        const char* name = argv[first_operand];
        const char* suffix = (operand_count == 2) ? argv[first_operand + 1] : NULL;
        if (!bx_basename_process_name(name, suffix, options.zero_terminated, &diag)) {
            return diag.exit_status;
        }
    }

    if (fflush(stdout) == EOF) {
        bx_diag(&diag, "write error: %s", strerror(errno));
    }

    return diag.exit_status;
}
