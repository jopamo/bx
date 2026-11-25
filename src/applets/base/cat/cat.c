#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "applets.h"
#include "lib/cli_common.h"
#include "lib/fd_ops.h"
#include "lib/line_writer.h"
#include "lib/option_scan.h"
#include "lib/xreadwrite.h"

enum {
    BX_CAT_OPT_HELP = 256,
    BX_CAT_OPT_VERSION,
};

struct bx_cat_options {
    const char* progname;
    bool show_nonprinting;
    bool show_ends;
    bool show_tabs;
    bool number_nonblank;
    bool number_all;
    bool squeeze_blank;
};

struct bx_cat_state {
    unsigned long long line_number;
    bool at_line_start;
    bool previous_output_line_blank;
};

static void bx_cat_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... [FILE]...\n", progname);
    fprintf(stream, "Concatenate FILE(s), or standard input, to standard output.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -A, --show-all           equivalent to -vET\n");
    fprintf(stream, "  -b, --number-nonblank    number nonempty output lines, overrides -n\n");
    fprintf(stream, "  -e                       equivalent to -vE\n");
    fprintf(stream, "  -E, --show-ends          display $ at end of each line\n");
    fprintf(stream, "  -n, --number             number all output lines\n");
    fprintf(stream, "  -s, --squeeze-blank      suppress repeated empty output lines\n");
    fprintf(stream, "  -t                       equivalent to -vT\n");
    fprintf(stream, "  -T, --show-tabs          display TAB characters as ^I\n");
    fprintf(stream, "  -u                       (ignored)\n");
    fprintf(stream, "  -v, --show-nonprinting   use ^ and M- notation, except for LFD and TAB\n");
    fprintf(stream, "      --help               display this help and exit\n");
    fprintf(stream, "      --version            output version information and exit\n");
}

static void bx_cat_print_file_error(const struct bx_cat_options* options, const char* path) {
    fprintf(stderr, "%s: %s: %s\n", options->progname, path, strerror(errno));
}

static void bx_cat_print_write_error(const struct bx_cat_options* options) {
    int saved_errno = errno;
    if (saved_errno == 0) {
        saved_errno = EIO;
    }
    fprintf(stderr, "%s: write error: %s\n", options->progname, strerror(saved_errno));
}

static bool bx_cat_write(struct bx_line_writer* writer, const void* data, size_t len, const struct bx_cat_options* options) {
    if (!bx_line_writer_write(writer, data, len)) {
        bx_cat_print_write_error(options);
        return false;
    }
    return true;
}

static bool bx_cat_emit_byte(struct bx_line_writer* writer, unsigned char ch, const struct bx_cat_options* options) {
    return bx_cat_write(writer, &ch, 1u, options);
}

static bool bx_cat_emit_line_number(struct bx_line_writer* writer, struct bx_cat_state* state, const struct bx_cat_options* options) {
    char buffer[64];
    int len = snprintf(buffer, sizeof(buffer), "%6llu\t", state->line_number);
    if (len < 0) {
        errno = EIO;
        bx_cat_print_write_error(options);
        return false;
    }

    if (!bx_cat_write(writer, buffer, (size_t)len, options)) {
        return false;
    }

    if (state->line_number != ~0ULL) {
        state->line_number++;
    }
    return true;
}

static bool bx_cat_emit_visible_byte(struct bx_line_writer* writer, unsigned char ch, const struct bx_cat_options* options) {
    if (ch == '\t' && options->show_tabs) {
        return bx_cat_emit_byte(writer, '^', options) && bx_cat_emit_byte(writer, 'I', options);
    }

    if (!options->show_nonprinting || ch == '\t') {
        return bx_cat_emit_byte(writer, ch, options);
    }

    if (ch < 32u) {
        return bx_cat_emit_byte(writer, '^', options) && bx_cat_emit_byte(writer, (unsigned char)(ch + 64u), options);
    }

    if (ch == 127u) {
        return bx_cat_emit_byte(writer, '^', options) && bx_cat_emit_byte(writer, '?', options);
    }

    if (ch >= 128u) {
        if (!bx_cat_emit_byte(writer, 'M', options) || !bx_cat_emit_byte(writer, '-', options)) {
            return false;
        }

        ch = (unsigned char)(ch - 128u);

        if (ch < 32u) {
            return bx_cat_emit_byte(writer, '^', options) && bx_cat_emit_byte(writer, (unsigned char)(ch + 64u), options);
        }
        if (ch == 127u) {
            return bx_cat_emit_byte(writer, '^', options) && bx_cat_emit_byte(writer, '?', options);
        }
    }

    return bx_cat_emit_byte(writer, ch, options);
}

static bool bx_cat_process_byte(unsigned char ch, const struct bx_cat_options* options, struct bx_cat_state* state, struct bx_line_writer* writer) {
    bool blank_line = state->at_line_start && ch == '\n';

    if (blank_line && options->squeeze_blank && state->previous_output_line_blank) {
        return true;
    }

    if (state->at_line_start) {
        bool number_this_line = false;
        if (options->number_nonblank) {
            number_this_line = !blank_line;
        }
        else if (options->number_all) {
            number_this_line = true;
        }

        if (number_this_line && !bx_cat_emit_line_number(writer, state, options)) {
            return false;
        }
    }

    if (ch == '\n') {
        if (options->show_ends && !bx_cat_emit_byte(writer, '$', options)) {
            return false;
        }
        if (!bx_cat_emit_byte(writer, '\n', options)) {
            return false;
        }
        state->at_line_start = true;
        state->previous_output_line_blank = blank_line;
        return true;
    }

    if (!bx_cat_emit_visible_byte(writer, ch, options)) {
        return false;
    }

    state->at_line_start = false;
    state->previous_output_line_blank = false;
    return true;
}

static bool bx_cat_raw_mode(const struct bx_cat_options* options) {
    return !options->show_nonprinting &&
           !options->show_ends &&
           !options->show_tabs &&
           !options->number_nonblank &&
           !options->number_all &&
           !options->squeeze_blank;
}

static bool bx_cat_process_fd(int fd, const char* path, const struct bx_cat_options* options, struct bx_cat_state* state, struct bx_line_writer* writer, int* exit_status) {
    unsigned char buffer[8192];
    bool raw_mode = bx_cat_raw_mode(options);

    while (true) {
        ssize_t nread = bx_xread(fd, buffer, sizeof(buffer));
        if (nread == 0) {
            return true;
        }
        if (nread < 0) {
            bx_cat_print_file_error(options, path);
            *exit_status = 1;
            return true;
        }

        if (raw_mode) {
            if (!bx_cat_write(writer, buffer, (size_t)nread, options)) {
                return false;
            }
        }
        else {
            for (ssize_t i = 0; i < nread; i++) {
                if (!bx_cat_process_byte(buffer[i], options, state, writer)) {
                    return false;
                }
            }
        }

        if (!bx_line_writer_flush(writer)) {
            bx_cat_print_write_error(options);
            return false;
        }
    }
}

static bool bx_cat_process_path(const char* path, const struct bx_cat_options* options, struct bx_cat_state* state, struct bx_line_writer* writer, int* exit_status) {
    int fd = STDIN_FILENO;
    bool must_close = false;

    if (strcmp(path, "-") != 0) {
        fd = bx_fd_open_cloexec(path, O_RDONLY, 0);
        if (fd < 0) {
            bx_cat_print_file_error(options, path);
            *exit_status = 1;
            return true;
        }
        must_close = true;
    }

    bool ok = bx_cat_process_fd(fd, path, options, state, writer, exit_status);

    if (must_close && close(fd) < 0) {
        bx_cat_print_file_error(options, path);
        *exit_status = 1;
    }

    return ok;
}

int bx_cat_main(int argc, char** argv) {
    static const struct bx_option_spec specs[] = {
        {'A', 'A', "show-all", BX_OPTION_NO_VALUE},
        {'b', 'b', "number-nonblank", BX_OPTION_NO_VALUE},
        {'e', 'e', NULL, BX_OPTION_NO_VALUE},
        {'E', 'E', "show-ends", BX_OPTION_NO_VALUE},
        {'n', 'n', "number", BX_OPTION_NO_VALUE},
        {'s', 's', "squeeze-blank", BX_OPTION_NO_VALUE},
        {'t', 't', NULL, BX_OPTION_NO_VALUE},
        {'T', 'T', "show-tabs", BX_OPTION_NO_VALUE},
        {'u', 'u', NULL, BX_OPTION_NO_VALUE},
        {'v', 'v', "show-nonprinting", BX_OPTION_NO_VALUE},
        {BX_CAT_OPT_HELP, 0, "help", BX_OPTION_NO_VALUE},
        {BX_CAT_OPT_VERSION, 0, "version", BX_OPTION_NO_VALUE},
    };

    struct bx_cat_options options = {
        .progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "cat"),
    };
    const char** paths = argc > 1 ? calloc((size_t)(argc - 1), sizeof(*paths)) : NULL;
    if (argc > 1 && paths == NULL) {
        fprintf(stderr, "%s: memory exhausted\n", options.progname);
        return 1;
    }
    size_t count = 0;
    int exit_status = 0;
    struct bx_option_cursor cursor;
    bx_option_cursor_init(&cursor, argv, argc > 0 ? 1u : 0u, (size_t)argc,
                          BX_OPTION_ORDERED | BX_OPTION_ABBREVIATE, specs, sizeof(specs) / sizeof(specs[0]));
    struct bx_option_result option;
    while ((option = bx_option_next(&cursor)).event != BX_OPTION_END) {
        if (option.event == BX_OPTION_OPERAND) {
            paths[count++] = option.token;
            continue;
        }
        if (option.event != BX_OPTION_MATCH) {
            if (option.short_name)
                fprintf(stderr, "%s: invalid option -- '%c'\n", options.progname, option.short_name);
            else
                fprintf(stderr, "%s: unrecognized option '%s'\n", options.progname, option.token);
            exit_status = 1;
            goto done;
        }

        switch (option.spec->id) {
            case 'A':
                options.show_nonprinting = true;
                options.show_ends = true;
                options.show_tabs = true;
                break;
            case 'b':
                options.number_nonblank = true;
                break;
            case 'e':
                options.show_nonprinting = true;
                options.show_ends = true;
                break;
            case 'E':
                options.show_ends = true;
                break;
            case 'n':
                options.number_all = true;
                break;
            case 's':
                options.squeeze_blank = true;
                break;
            case 't':
                options.show_nonprinting = true;
                options.show_tabs = true;
                break;
            case 'T':
                options.show_tabs = true;
                break;
            case 'u':
                break;
            case 'v':
                options.show_nonprinting = true;
                break;
            case BX_CAT_OPT_HELP:
                bx_cat_print_help(stdout, options.progname);
                goto done;
            case BX_CAT_OPT_VERSION:
                bx_cli_print_version(options.progname);
                goto done;
        }
    }

    struct bx_cat_state state = {
        .line_number = 1,
        .at_line_start = true,
        .previous_output_line_blank = false,
    };
    char output_buffer[8192];
    struct bx_line_writer writer;
    bx_line_writer_init(&writer, STDOUT_FILENO, output_buffer, sizeof(output_buffer));

    if (count == 0) {
        if (!bx_cat_process_path("-", &options, &state, &writer, &exit_status)) {
            exit_status = 1;
            goto done;
        }
    }
    else {
        for (size_t i = 0; i < count; i++) {
            if (!bx_cat_process_path(paths[i], &options, &state, &writer, &exit_status)) {
                exit_status = 1;
                goto done;
            }
        }
    }

    if (bx_line_writer_error(&writer) == 0 && !bx_line_writer_flush(&writer)) {
        bx_cat_print_write_error(&options);
        exit_status = 1;
    }

done:
    free(paths);
    return exit_status;
}
