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
#include "lib/cli_common.h"
#include "lib/fopen_dash.h"
#include "lib/size_parse.h"
#include "lib/args_common.h"
#include "lib/line_writer.h"

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

static bool bx_head_parse_num(const char* str, long long* value_out) {
    if (str == NULL || str[0] == '\0') {
        return false;
    }

    if (str[0] == '+') {
        return false;
    }

    intmax_t value = 0;
    if (!bx_size_parse_scaled_count(str, &value)) {
        return false;
    }
    if (value < (intmax_t)LLONG_MIN || value > (intmax_t)LLONG_MAX) {
        return false;
    }

    *value_out = (long long)value;
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
    options->progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "head");
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

    bx_args_getopt_reset_at(option_start);

    while (true) {
        int option_index = 0;
        int c = bx_args_getopt_long(argc, argv, "c:n:qvz", long_options, &option_index);
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

static bool bx_head_write_error(struct bx_diag_ctx* diag) {
    bx_diag(diag, "write error: %s", strerror(errno));
    return false;
}

static bool bx_head_write(
    struct bx_line_writer* writer,
    const void* data,
    size_t length,
    struct bx_diag_ctx* diag
) {
    if (!bx_line_writer_write(writer, data, length)) {
        return bx_head_write_error(diag);
    }
    return true;
}

static bool bx_head_putc(struct bx_line_writer* writer, int c, struct bx_diag_ctx* diag) {
    if (!bx_line_writer_putc(writer, (char)c)) {
        return bx_head_write_error(diag);
    }
    return true;
}

static bool bx_head_puts(struct bx_line_writer* writer, const char* text, struct bx_diag_ctx* diag) {
    if (!bx_line_writer_puts(writer, text)) {
        return bx_head_write_error(diag);
    }
    return true;
}

static bool bx_head_write_header(
    struct bx_line_writer* writer,
    const char* filename,
    bool leading_newline,
    struct bx_diag_ctx* diag
) {
    if (leading_newline && !bx_head_putc(writer, '\n', diag)) {
        return false;
    }
    return bx_head_puts(writer, "==> ", diag) &&
           bx_head_puts(writer, filename, diag) &&
           bx_head_puts(writer, " <==\n", diag);
}

static bool head_file(FILE* f, struct bx_head_options* options, struct bx_line_writer* writer, struct bx_diag_ctx* diag) {
    int delimiter = options->zero_terminated ? '\0' : '\n';

    if (options->bytes > 0) {
        long long count = options->bytes;
        int c;
        while (count-- > 0 && (c = getc(f)) != EOF) {
            if (!bx_head_putc(writer, c, diag)) {
                return false;
            }
        }
    }
    else if (options->bytes < 0) {
        // All but last N bytes
        long long skip = -options->bytes;
        char* buf = xmalloc(skip);
        size_t n = fread(buf, 1, skip, f);
        if (n < (size_t)skip) {
            free(buf);
            return true;
        }

        int c;
        while ((c = getc(f)) != EOF) {
            if (!bx_head_putc(writer, buf[0], diag)) {
                free(buf);
                return false;
            }
            memmove(buf, buf + 1, skip - 1);
            buf[skip - 1] = c;
        }
        free(buf);
    }
    else if (options->lines > 0) {
        long long count = options->lines;
        int c;
        while (count > 0 && (c = getc(f)) != EOF) {
            if (!bx_head_putc(writer, c, diag)) {
                return false;
            }
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
                if (!bx_head_write(writer, ring[idx], lens[idx], diag)) {
                    for (long long i = 0; i < skip; i++)
                        free(ring[i]);
                    free(ring);
                    free(lens);
                    free(line);
                    return false;
                }
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

    return true;
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
        bx_cli_print_version(options.progname);
        return 0;
    }

    int num_files = argc - first_operand;
    bool ok = true;
    char output_buffer[8192];
    struct bx_line_writer writer;
    bx_line_writer_init(&writer, STDOUT_FILENO, output_buffer, sizeof(output_buffer));

    for (int i = 0; ok && (i < num_files || (i == 0 && num_files == 0)); i++) {
        const char* filename = (num_files == 0) ? "-" : argv[first_operand + i];
        bool is_stdio = false;
        FILE* f = bx_fopen_dash(filename, "r", &is_stdio);
        if (!f) {
            bx_diag(&diag, "%s: %s", filename, strerror(errno));
            continue;
        }

        if (num_files > 1 && !options.quiet) {
            ok = bx_head_write_header(&writer, filename, i > 0, &diag);
        }
        else if (options.verbose) {
            ok = bx_head_write_header(&writer, filename, i > 0, &diag);
        }

        if (ok) {
            ok = head_file(f, &options, &writer, &diag);
        }
        bx_fclose_nonstdio(f, is_stdio);
    }

    if (ok && bx_line_writer_error(&writer) == 0 && !bx_line_writer_flush(&writer)) {
        bx_head_write_error(&diag);
    }

    return diag.exit_status;
}
