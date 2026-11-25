#include <errno.h>
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
#include "lib/option_scan.h"
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
    const char** files;
    size_t file_count;
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
    if (value <= (intmax_t)LLONG_MIN || value > (intmax_t)LLONG_MAX) {
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

static bool bx_head_parse_options(int argc, char** argv, struct bx_head_options* options, struct bx_diag_ctx* diag) {
    static const struct bx_option_spec specs[] = {
        {'c', 'c', "bytes", BX_OPTION_REQUIRED_VALUE}, {'n', 'n', "lines", BX_OPTION_REQUIRED_VALUE},     {'q', 'q', "quiet", BX_OPTION_NO_VALUE}, {'q', 0, "silent", BX_OPTION_NO_VALUE},
        {'v', 'v', "verbose", BX_OPTION_NO_VALUE},     {'z', 'z', "zero-terminated", BX_OPTION_NO_VALUE}, {1, 0, "help", BX_OPTION_NO_VALUE},      {2, 0, "version", BX_OPTION_NO_VALUE},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "head");
    options->lines = 10;
    diag->progname = options->progname;

    size_t option_start = argc > 0 ? 1u : 0u;
    if (argc > 1) {
        long long legacy_lines = 0;
        if (bx_head_parse_legacy_lines_option(argv[1], &legacy_lines)) {
            options->lines = legacy_lines;
            option_start = 2;
        }
    }

    struct bx_option_cursor cursor;
    bx_option_cursor_init(&cursor, argv, option_start, (size_t)argc, BX_OPTION_ORDERED | BX_OPTION_ABBREVIATE, specs, sizeof(specs) / sizeof(specs[0]));
    struct bx_option_result option;
    while ((option = bx_option_next(&cursor)).event != BX_OPTION_END) {
        if (option.event == BX_OPTION_OPERAND) {
            if (!options->files) {
                options->files = calloc((size_t)argc - option.index, sizeof(*options->files));
                if (!options->files) {
                    bx_perror_path(diag, "allocate input operands");
                    return false;
                }
            }
            options->files[options->file_count++] = option.token;
            continue;
        }
        if (option.event != BX_OPTION_MATCH) {
            if (option.event == BX_OPTION_MISSING_VALUE && option.spec)
                bx_diag(diag, "option requires an argument -- '%c'", option.spec->short_name);
            else if (option.short_name)
                bx_diag(diag, "invalid option -- '%c'", option.short_name);
            else
                bx_diag(diag, "unrecognized option '%s'", option.token);
            return false;
        }
        switch (option.spec->id) {
            case 'c': {
                long long bytes = 0;
                if (!bx_head_parse_num(option.value, &bytes)) {
                    bx_diag(diag, "invalid number of bytes: '%s'", option.value);
                    return false;
                }
                options->bytes = bytes;
                options->lines = 0;
                break;
            }
            case 'n': {
                long long lines = 0;
                if (!bx_head_parse_num(option.value, &lines)) {
                    bx_diag(diag, "invalid number of lines: '%s'", option.value);
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
            default:
                return false;
        }
    }

    if ((options->bytes < 0 && (uintmax_t)-options->bytes > SIZE_MAX) ||
        (options->lines < 0 && ((uintmax_t)-options->lines > SIZE_MAX / sizeof(char*) || (uintmax_t)-options->lines > SIZE_MAX / sizeof(size_t)))) {
        bx_diag(diag, "count too large to buffer");
        return false;
    }
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

    if (!bx_head_parse_options(argc, argv, &options, &diag)) {
        free(options.files);
        return 1;
    }
    if (options.show_help) {
        bx_head_print_help(stdout, options.progname);
        free(options.files);
        return 0;
    }
    if (options.show_version) {
        bx_cli_print_version(options.progname);
        free(options.files);
        return 0;
    }

    size_t num_files = options.file_count;
    bool ok = true;
    char output_buffer[8192];
    struct bx_line_writer writer;
    bx_line_writer_init(&writer, STDOUT_FILENO, output_buffer, sizeof(output_buffer));

    for (size_t i = 0; ok && (i < num_files || (i == 0 && num_files == 0)); i++) {
        const char* filename = (num_files == 0) ? "-" : options.files[i];
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

    free(options.files);
    return diag.exit_status;
}
