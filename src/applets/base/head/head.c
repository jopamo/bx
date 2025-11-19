#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"
#include "bx/libbx.h"
#include "lib/fopen_dash.h"

struct bx_head_options {
    const char* progname;
    long long lines;
    long long bytes;
    bool quiet;
    bool verbose;
    bool zero_terminated;
    bool show_help;
    bool show_version;
};

static void bx_head_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... [FILE]...\n", progname);
    fprintf(stream, "Print the first 10 lines of each FILE to standard output.\n");
    fprintf(stream, "With more than one FILE, precede each with a header giving the file name.\n");
    fprintf(stream, "\n");
    fprintf(stream, "With no FILE, or when FILE is -, read standard input.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -c, --bytes=[-]NUM       print the first NUM bytes of each file;\n");
    fprintf(stream, "                             with the leading '-', print all but the last\n");
    fprintf(stream, "                             NUM bytes of each file\n");
    fprintf(stream, "  -n, --lines=[-]NUM       print the first NUM lines instead of the first 10;\n");
    fprintf(stream, "                             with the leading '-', print all but the last\n");
    fprintf(stream, "                             NUM lines of each file\n");
    fprintf(stream, "  -q, --quiet, --silent    never print headers giving file names\n");
    fprintf(stream, "  -v, --verbose            always print headers giving file names\n");
    fprintf(stream, "  -z, --zero-terminated    line delimiter is NUL, not newline\n");
    fprintf(stream, "      --help     display this help and exit\n");
    fprintf(stream, "      --version  output version information and exit\n");
    fprintf(stream, "\n");
    fprintf(stream, "NUM may have a multiplier suffix:\n");
    fprintf(stream, "b 512, kB 1000, K 1024, MB 1000*1000, M 1024*1024,\n");
    fprintf(stream, "GB 1000*1000*1000, G 1024*1024*1024, and so on for T, P, E, Z, Y.\n");
}

static void bx_head_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

// Simplified suffix parser for now
static bool bx_head_apply_multiplier(long long* value, long long multiplier) {
    if (*value > LLONG_MAX / multiplier) {
        return false;
    }

    *value *= multiplier;
    return true;
}

// Simplified suffix parser for now
static bool bx_head_parse_num(const char* str, long long* value_out) {
    char* endptr = NULL;
    bool negative = false;
    const char* magnitude = str;

    if (magnitude == NULL || magnitude[0] == '\0') {
        return false;
    }

    if (magnitude[0] == '-') {
        negative = true;
        magnitude++;
        if (magnitude[0] == '\0') {
            return false;
        }
    }

    errno = 0;
    long long val = strtoll(magnitude, &endptr, 10);
    if (errno != 0 || endptr == magnitude) {
        return false;
    }

    if (*endptr != '\0') {
        // Multiplier suffixes
        if (strcmp(endptr, "K") == 0) {
            if (!bx_head_apply_multiplier(&val, 1024LL)) {
                return false;
            }
        }
        else if (strcmp(endptr, "M") == 0) {
            if (!bx_head_apply_multiplier(&val, 1024LL * 1024LL)) {
                return false;
            }
        }
        else if (strcmp(endptr, "G") == 0) {
            if (!bx_head_apply_multiplier(&val, 1024LL * 1024LL * 1024LL)) {
                return false;
            }
        }
        else {
            return false;
        }
    }

    *value_out = negative ? -val : val;
    return true;
}

static bool bx_head_parse_legacy_lines_option(const char* arg, long long* lines_out) {
    if (arg == NULL || arg[0] != '-' || arg[1] == '\0' || arg[1] == '-') {
        return false;
    }

    return bx_head_parse_num(arg + 1, lines_out);
}

static bool bx_head_parse_options(int argc, char** argv, struct bx_head_options* options, int* first_operand, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"bytes", required_argument, NULL, 'c'}, {"lines", required_argument, NULL, 'n'}, {"quiet", no_argument, NULL, 'q'},
        {"silent", no_argument, NULL, 'q'},      {"verbose", no_argument, NULL, 'v'},     {"zero-terminated", no_argument, NULL, 'z'},
        {"help", no_argument, NULL, 1},          {"version", no_argument, NULL, 2},       {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = "head";
    options->lines = 10;
    diag->progname = options->progname;

    int option_start = 1;
    if (argc > 1) {
        long long legacy_lines = 0;
        if (bx_head_parse_legacy_lines_option(argv[1], &legacy_lines)) {
            options->lines = legacy_lines;
            option_start = 2;
        }
    }

    opterr = 0;
    optind = option_start;

    while (true) {
        int option_index = 0;
        int c = getopt_long(argc, argv, "c:n:qvz", long_options, &option_index);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 'c': {
                long long bytes = 0;
                if (!bx_head_parse_num(optarg, &bytes)) {
                    bx_diag(diag, "invalid number of bytes: '%s'", optarg);
                    return false;
                }
                options->bytes = bytes;
                options->lines = 0;
                break;
            }
            case 'n': {
                long long lines = 0;
                if (!bx_head_parse_num(optarg, &lines)) {
                    bx_diag(diag, "invalid number of lines: '%s'", optarg);
                    return false;
                }
                options->lines = lines;
                options->bytes = 0;
                break;
            }
            case 'q':
                options->quiet = true;
                options->verbose = false;
                break;
            case 'v':
                options->verbose = true;
                options->quiet = false;
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
                if (optopt == 'c' || optopt == 'n') {
                    bx_diag(diag, "option requires an argument -- '%c'", optopt);
                }
                else if (optopt != 0) {
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

static void head_file(FILE* f, struct bx_head_options* options) {
    int delimiter = options->zero_terminated ? '\0' : '\n';

    if (options->bytes > 0) {
        long long count = options->bytes;
        int c;
        while (count-- > 0 && (c = getc(f)) != EOF) {
            putchar(c);
        }
    }
    else if (options->bytes < 0) {
        // All but last N bytes
        long long skip = -options->bytes;
        char* buf = xmalloc(skip);
        size_t n = fread(buf, 1, skip, f);
        if (n < (size_t)skip) {
            free(buf);
            return;
        }

        int c;
        while ((c = getc(f)) != EOF) {
            putchar(buf[0]);
            memmove(buf, buf + 1, skip - 1);
            buf[skip - 1] = c;
        }
        free(buf);
    }
    else if (options->lines > 0) {
        long long count = options->lines;
        int c;
        while (count > 0 && (c = getc(f)) != EOF) {
            putchar(c);
            if (c == delimiter)
                count--;
        }
    }
    else if (options->lines < 0) {
        // All but last N lines
        long long skip = -options->lines;
        char** ring = xmalloc(skip * sizeof(char*));
        size_t* lens = xmalloc(skip * sizeof(size_t));
        memset(ring, 0, skip * sizeof(char*));

        char* line = NULL;
        size_t line_cap = 0;
        ssize_t len;
        long long idx = 0;
        bool full = false;

        while ((len = getdelim(&line, &line_cap, delimiter, f)) != -1) {
            if (full) {
                fwrite(ring[idx], 1, lens[idx], stdout);
                free(ring[idx]);
            }
            ring[idx] = xstrdup(line);
            lens[idx] = len;
            idx = (idx + 1) % skip;
            if (idx == 0)
                full = true;
        }
        for (long long i = 0; i < skip; i++)
            free(ring[i]);
        free(ring);
        free(lens);
        free(line);
    }
}

int bx_head_main(int argc, char** argv) {
    struct bx_head_options options;
    struct bx_diag_ctx diag = {.progname = "head", .exit_status = 0};
    int first_operand = 0;

    if (!bx_head_parse_options(argc, argv, &options, &first_operand, &diag))
        return 1;
    if (options.show_help) {
        bx_head_print_help(stdout, options.progname);
        return 0;
    }
    if (options.show_version) {
        bx_head_print_version(options.progname);
        return 0;
    }

    int num_files = argc - first_operand;
    for (int i = 0; i < num_files || (i == 0 && num_files == 0); i++) {
        const char* filename = (num_files == 0) ? "-" : argv[first_operand + i];
        bool is_stdio = false;
        FILE* f = bx_fopen_dash(filename, "r", &is_stdio);
        if (!f) {
            bx_diag(&diag, "%s: %s", filename, strerror(errno));
            continue;
        }

        if (num_files > 1 && !options.quiet) {
            if (i > 0)
                printf("\n");
            printf("==> %s <==\n", filename);
        }
        else if (options.verbose) {
            if (i > 0)
                printf("\n");
            printf("==> %s <==\n", filename);
        }

        head_file(f, &options);
        bx_fclose_nonstdio(f, is_stdio);
    }

    return diag.exit_status;
}
