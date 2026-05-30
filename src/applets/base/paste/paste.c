#include <errno.h>
#include <getopt.h>
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
#include "lib/args_common.h"
#include "lib/line_writer.h"

struct bx_paste_options {
    const char* progname;
    int* delimiters;
    size_t delimiters_len;
    const char* delimiter_spec;
    bool serial;
    bool zero_terminated;
    bool show_help;
    bool show_version;
};

enum bx_paste_parse_status {
    BX_PASTE_PARSE_OK = 0,
    BX_PASTE_PARSE_ERROR = 1,
    BX_PASTE_PARSE_ERROR_TRY_HELP = 2,
};

static void bx_paste_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... [FILE]...\n", progname);
    fprintf(stream, "Write lines consisting of the sequentially corresponding lines from\n");
    fprintf(stream, "each FILE, separated by TABs, to standard output.\n");
    fprintf(stream, "\n");
    fprintf(stream, "With no FILE, or when FILE is -, read standard input.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -d, --delimiters=LIST   reuse characters from LIST instead of TABs;\n");
    fprintf(stream, "                            backslash escapes are supported\n");
    fprintf(stream, "  -s, --serial            paste one file at a time instead of in parallel\n");
    fprintf(stream, "  -z, --zero-terminated   line delimiter is NUL, not newline\n");
    fprintf(stream, "      --help     display this help and exit\n");
    fprintf(stream, "      --version  output version information and exit\n");
}

static bool bx_paste_parse_delimiters(const char* spec, struct bx_paste_options* options, struct bx_diag_ctx* diag) {
    size_t spec_len = strlen(spec);
    size_t capacity = spec_len == 0 ? 1 : spec_len;
    int* delimiters = xmalloc(capacity * sizeof(*delimiters));
    size_t count = 0;

    if (spec_len == 0) {
        delimiters[count++] = -1;
    }

    for (size_t i = 0; i < spec_len; i++) {
        unsigned char ch = (unsigned char)spec[i];
        if (ch != '\\') {
            delimiters[count++] = (int)ch;
            continue;
        }

        if (i + 1 >= spec_len) {
            free(delimiters);
            bx_diag(diag, "delimiter list ends with an unescaped backslash: %s", spec);
            return false;
        }

        i++;
        switch (spec[i]) {
            case '0':
                delimiters[count++] = -1;
                break;
            case 'b':
                delimiters[count++] = '\b';
                break;
            case 'f':
                delimiters[count++] = '\f';
                break;
            case 'n':
                delimiters[count++] = '\n';
                break;
            case 'r':
                delimiters[count++] = '\r';
                break;
            case 't':
                delimiters[count++] = '\t';
                break;
            case 'v':
                delimiters[count++] = '\v';
                break;
            case '\\':
                delimiters[count++] = '\\';
                break;
            default:
                delimiters[count++] = (unsigned char)spec[i];
                break;
        }
    }

    options->delimiters = delimiters;
    options->delimiters_len = count;
    return true;
}

static enum bx_paste_parse_status bx_paste_parse_options(
    int argc,
    char** argv,
    struct bx_paste_options* options,
    int* first_operand,
    struct bx_diag_ctx* diag
) {
    static const struct option long_options[] = {
        {"delimiters", required_argument, NULL, 'd'},
        {"serial", no_argument, NULL, 's'},
        {"zero-terminated", no_argument, NULL, 'z'},
        {"help", no_argument, NULL, 1},
        {"version", no_argument, NULL, 2},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "paste");
    options->delimiter_spec = "\t";
    diag->progname = options->progname;

    bx_args_getopt_reset();

    while (true) {
        int option_index = 0;
        int c = bx_args_getopt_long(argc, argv, "d:sz", long_options, &option_index);
        if (c == -1)
            break;

        switch (c) {
            case 'd':
                options->delimiter_spec = optarg;
                break;
            case 's':
                options->serial = true;
                break;
            case 'z':
                options->zero_terminated = true;
                break;
            case 1:
                options->show_help = true;
                return BX_PASTE_PARSE_OK;
            case 2:
                options->show_version = true;
                return BX_PASTE_PARSE_OK;
            case '?':
                if (optopt == 'd') {
                    bx_cli_diag_option_requires_arg(diag, optopt, optind, argc, argv);
                }
                else {
                    bx_cli_diag_unrecognized_option(diag, optopt, optind, argc, argv);
                }
                return BX_PASTE_PARSE_ERROR_TRY_HELP;
            default:
                return BX_PASTE_PARSE_ERROR;
        }
    }

    *first_operand = optind;
    if (!bx_paste_parse_delimiters(options->delimiter_spec, options, diag)) {
        return BX_PASTE_PARSE_ERROR;
    }
    return BX_PASTE_PARSE_OK;
}

static bool bx_paste_write_error(struct bx_diag_ctx* diag) {
    bx_diag(diag, "write error: %s", strerror(errno));
    return false;
}

static bool bx_paste_emit_delimiter(
    struct bx_line_writer* writer,
    const struct bx_paste_options* options,
    size_t index,
    struct bx_diag_ctx* diag
) {
    int delim = options->delimiters[index % options->delimiters_len];
    if (delim >= 0) {
        if (!bx_line_writer_putc(writer, (char)delim)) {
            return bx_paste_write_error(diag);
        }
    }
    return true;
}

static bool paste_serial(
    int num_files,
    char** filenames,
    struct bx_paste_options* options,
    struct bx_diag_ctx* diag,
    struct bx_line_writer* writer
) {
    int delimiter = options->zero_terminated ? '\0' : '\n';
    bool ok = true;

    for (int i = 0; i < num_files; i++) {
        bool is_stdio = false;
        FILE* f = bx_fopen_dash(filenames[i], "r", &is_stdio);
        if (!f) {
            bx_diag(diag, "%s: %s", filenames[i], strerror(errno));
            continue;
        }

        char* line = NULL;
        size_t cap = 0;
        ssize_t len;
        bool first_line = true;
        size_t delim_idx = 0;
        while ((len = getdelim(&line, &cap, delimiter, f)) != -1) {
            if (!first_line) {
                if (!bx_paste_emit_delimiter(writer, options, delim_idx, diag)) {
                    ok = false;
                    break;
                }
                delim_idx++;
            }
            if (line[len - 1] == delimiter)
                line[len - 1] = '\0';
            if (!bx_line_writer_puts(writer, line)) {
                ok = bx_paste_write_error(diag);
                break;
            }
            first_line = false;
        }
        if (ok && !bx_line_writer_putc(writer, (char)delimiter)) {
            ok = bx_paste_write_error(diag);
        }
        free(line);
        bx_fclose_nonstdio(f, is_stdio);
        if (!ok) {
            break;
        }
    }

    return ok;
}

static bool paste_parallel(
    int num_files,
    char** filenames,
    struct bx_paste_options* options,
    struct bx_diag_ctx* diag,
    struct bx_line_writer* writer
) {
    FILE** files = xmalloc(num_files * sizeof(FILE*));
    bool* is_stdio = xmalloc(num_files * sizeof(bool));
    bool open_failed = false;
    bool ok = true;
    for (int i = 0; i < num_files; i++) {
        files[i] = bx_fopen_dash(filenames[i], "r", &is_stdio[i]);
        if (!files[i]) {
            bx_diag(diag, "%s: %s", filenames[i], strerror(errno));
            open_failed = true;
        }
    }

    if (open_failed) {
        for (int i = 0; i < num_files; i++) {
            bx_fclose_nonstdio(files[i], is_stdio[i]);
        }
        free(files);
        free(is_stdio);
        return true;
    }

    int delimiter = options->zero_terminated ? '\0' : '\n';

    char* line = NULL;
    size_t line_cap = 0;
    char** row_fields = xmalloc(num_files * sizeof(char*));

    while (true) {
        bool any_active = false;
        for (int i = 0; i < num_files; i++) {
            row_fields[i] = NULL;
            if (files[i]) {
                ssize_t len = getdelim(&line, &line_cap, delimiter, files[i]);
                if (len != -1) {
                    any_active = true;
                    if (line[len - 1] == delimiter)
                        line[len - 1] = '\0';
                    row_fields[i] = xstrdup(line);
                }
                else {
                    bx_fclose_nonstdio(files[i], is_stdio[i]);
                    files[i] = NULL;
                }
            }
        }

        if (!any_active)
            break;

        for (int i = 0; i < num_files; i++) {
            if (ok && row_fields[i] && !bx_line_writer_puts(writer, row_fields[i])) {
                ok = bx_paste_write_error(diag);
            }
            if (i + 1 < num_files) {
                if (ok && !bx_paste_emit_delimiter(writer, options, (size_t)i, diag)) {
                    ok = false;
                }
            }
            free(row_fields[i]);
        }
        if (ok && !bx_line_writer_putc(writer, (char)delimiter)) {
            ok = bx_paste_write_error(diag);
        }
        if (!ok) {
            break;
        }
    }

    for (int i = 0; i < num_files; i++)
        bx_fclose_nonstdio(files[i], is_stdio[i]);
    free(files);
    free(is_stdio);
    free(line);
    free(row_fields);
    return ok;
}

int bx_paste_main(int argc, char** argv) {
    struct bx_paste_options options;
    struct bx_diag_ctx diag = {.progname = "paste", .exit_status = 0};
    int first_operand = 0;

    enum bx_paste_parse_status parse_status =
        bx_paste_parse_options(argc, argv, &options, &first_operand, &diag);
    if (parse_status != BX_PASTE_PARSE_OK) {
        if (parse_status == BX_PASTE_PARSE_ERROR_TRY_HELP) {
            bx_cli_print_try_help(options.progname);
        }
        return 1;
    }
    if (options.show_help) {
        bx_paste_print_help(stdout, options.progname);
        return 0;
    }
    if (options.show_version) {
        bx_cli_print_version(options.progname);
        return 0;
    }

    int num_files = argc - first_operand;
    char* default_filenames[] = {"-"};
    char** filenames = (num_files == 0) ? default_filenames : &argv[first_operand];
    int real_num_files = (num_files == 0) ? 1 : num_files;

    char output_buffer[8192];
    struct bx_line_writer writer;
    bx_line_writer_init(&writer, STDOUT_FILENO, output_buffer, sizeof(output_buffer));

    bool ok = options.serial
        ? paste_serial(real_num_files, filenames, &options, &diag, &writer)
        : paste_parallel(real_num_files, filenames, &options, &diag, &writer);

    if (ok && bx_line_writer_error(&writer) == 0 && !bx_line_writer_flush(&writer)) {
        bx_paste_write_error(&diag);
    }

    free(options.delimiters);
    return diag.exit_status;
}
