#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "applets.h"
#include "lib/xreadwrite.h"

struct bx_cat_options {
    const char* progname;
    bool show_nonprinting;
    bool show_ends;
    bool show_tabs;
    bool number_nonblank;
    bool number_all;
    bool squeeze_blank;
    bool show_help;
    bool show_version;
};

struct bx_cat_state {
    unsigned long long line_number;
    bool at_line_start;
    bool previous_output_line_blank;
};

static const char* bx_cat_progname(const char* argv0) {
    if (argv0 == NULL || argv0[0] == '\0') {
        return "cat";
    }

    const char* base = strrchr(argv0, '/');
    if (base != NULL && base[1] != '\0') {
        return base + 1;
    }
    return argv0;
}

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

static void bx_cat_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
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

static bool bx_cat_emit_byte(unsigned char ch, const struct bx_cat_options* options) {
    if (fputc((int)ch, stdout) == EOF) {
        bx_cat_print_write_error(options);
        return false;
    }
    return true;
}

static bool bx_cat_emit_line_number(struct bx_cat_state* state, const struct bx_cat_options* options) {
    char buffer[64];
    int len = snprintf(buffer, sizeof(buffer), "%6llu\t", state->line_number);
    if (len < 0) {
        errno = EIO;
        bx_cat_print_write_error(options);
        return false;
    }

    if (fwrite(buffer, 1, (size_t)len, stdout) != (size_t)len) {
        bx_cat_print_write_error(options);
        return false;
    }

    if (state->line_number != ~0ULL) {
        state->line_number++;
    }
    return true;
}

static bool bx_cat_emit_visible_byte(unsigned char ch, const struct bx_cat_options* options) {
    if (ch == '\t' && options->show_tabs) {
        return bx_cat_emit_byte('^', options) && bx_cat_emit_byte('I', options);
    }

    if (!options->show_nonprinting || ch == '\t') {
        return bx_cat_emit_byte(ch, options);
    }

    if (ch < 32u) {
        return bx_cat_emit_byte('^', options) && bx_cat_emit_byte((unsigned char)(ch + 64u), options);
    }

    if (ch == 127u) {
        return bx_cat_emit_byte('^', options) && bx_cat_emit_byte('?', options);
    }

    if (ch >= 128u) {
        if (!bx_cat_emit_byte('M', options) || !bx_cat_emit_byte('-', options)) {
            return false;
        }

        ch = (unsigned char)(ch - 128u);

        if (ch < 32u) {
            return bx_cat_emit_byte('^', options) && bx_cat_emit_byte((unsigned char)(ch + 64u), options);
        }
        if (ch == 127u) {
            return bx_cat_emit_byte('^', options) && bx_cat_emit_byte('?', options);
        }
    }

    return bx_cat_emit_byte(ch, options);
}

static bool bx_cat_process_byte(unsigned char ch, const struct bx_cat_options* options, struct bx_cat_state* state) {
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

        if (number_this_line && !bx_cat_emit_line_number(state, options)) {
            return false;
        }
    }

    if (ch == '\n') {
        if (options->show_ends && !bx_cat_emit_byte('$', options)) {
            return false;
        }
        if (!bx_cat_emit_byte('\n', options)) {
            return false;
        }
        state->at_line_start = true;
        state->previous_output_line_blank = blank_line;
        return true;
    }

    if (!bx_cat_emit_visible_byte(ch, options)) {
        return false;
    }

    state->at_line_start = false;
    state->previous_output_line_blank = false;
    return true;
}

static bool bx_cat_process_fd(int fd, const char* path, const struct bx_cat_options* options, struct bx_cat_state* state, int* exit_status) {
    unsigned char buffer[8192];

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

        for (ssize_t i = 0; i < nread; i++) {
            if (!bx_cat_process_byte(buffer[i], options, state)) {
                return false;
            }
        }
    }
}

static bool bx_cat_process_path(const char* path, const struct bx_cat_options* options, struct bx_cat_state* state, int* exit_status) {
    int fd = STDIN_FILENO;
    bool must_close = false;

    if (strcmp(path, "-") != 0) {
        fd = open(path, O_RDONLY);
        if (fd < 0) {
            bx_cat_print_file_error(options, path);
            *exit_status = 1;
            return true;
        }
        must_close = true;
    }

    bool ok = bx_cat_process_fd(fd, path, options, state, exit_status);

    if (must_close && close(fd) < 0) {
        bx_cat_print_file_error(options, path);
        *exit_status = 1;
    }

    return ok;
}

static bool bx_cat_parse_options(int argc, char** argv, struct bx_cat_options* options, int* first_operand) {
    static const struct option long_options[] = {
        {"show-all", no_argument, NULL, 'A'},
        {"number-nonblank", no_argument, NULL, 'b'},
        {"show-ends", no_argument, NULL, 'E'},
        {"number", no_argument, NULL, 'n'},
        {"squeeze-blank", no_argument, NULL, 's'},
        {"show-tabs", no_argument, NULL, 'T'},
        {"show-nonprinting", no_argument, NULL, 'v'},
        {"help", no_argument, NULL, 1},
        {"version", no_argument, NULL, 2},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_cat_progname((argc > 0) ? argv[0] : NULL);

    opterr = 0;
    optind = 1;

    while (true) {
        int option_index = 0;
        int c = getopt_long(argc, argv, "AbeEnstTuv", long_options, &option_index);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 'A':
                options->show_nonprinting = true;
                options->show_ends = true;
                options->show_tabs = true;
                break;
            case 'b':
                options->number_nonblank = true;
                break;
            case 'e':
                options->show_nonprinting = true;
                options->show_ends = true;
                break;
            case 'E':
                options->show_ends = true;
                break;
            case 'n':
                options->number_all = true;
                break;
            case 's':
                options->squeeze_blank = true;
                break;
            case 't':
                options->show_nonprinting = true;
                options->show_tabs = true;
                break;
            case 'T':
                options->show_tabs = true;
                break;
            case 'u':
                break;
            case 'v':
                options->show_nonprinting = true;
                break;
            case 1:
                options->show_help = true;
                return true;
            case 2:
                options->show_version = true;
                return true;
            case '?':
                if (optopt != 0) {
                    fprintf(stderr, "%s: invalid option -- '%c'\n", options->progname, optopt);
                }
                else if (optind > 0 && optind <= argc && argv[optind - 1] != NULL) {
                    fprintf(stderr, "%s: unrecognized option '%s'\n", options->progname, argv[optind - 1]);
                }
                else {
                    fprintf(stderr, "%s: unrecognized option\n", options->progname);
                }
                return false;
            default:
                return false;
        }
    }

    *first_operand = optind;
    return true;
}

int bx_cat_main(int argc, char** argv) {
    struct bx_cat_options options;
    int first_operand = 1;

    if (!bx_cat_parse_options(argc, argv, &options, &first_operand)) {
        return 1;
    }

    if (options.show_help) {
        bx_cat_print_help(stdout, options.progname);
        return 0;
    }

    if (options.show_version) {
        bx_cat_print_version(options.progname);
        return 0;
    }

    struct bx_cat_state state = {
        .line_number = 1,
        .at_line_start = true,
        .previous_output_line_blank = false,
    };
    int exit_status = 0;

    if (first_operand >= argc) {
        if (!bx_cat_process_path("-", &options, &state, &exit_status)) {
            return 1;
        }
    }
    else {
        for (int i = first_operand; i < argc; i++) {
            if (!bx_cat_process_path(argv[i], &options, &state, &exit_status)) {
                return 1;
            }
        }
    }

    if (fflush(stdout) == EOF) {
        bx_cat_print_write_error(&options);
        return 1;
    }

    return exit_status;
}
