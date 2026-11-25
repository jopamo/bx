#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"
#include "bx/libbx.h"
#include "lib/cli_common.h"
#include "lib/fmt_engine.h"
#include "lib/fopen_dash.h"
#include "lib/line_writer.h"
#include "lib/option_scan.h"

#define BX_FMT_OUTPUT_BUFFER_SIZE 8192u

enum {
    BX_FMT_OPT_HELP = 256,
    BX_FMT_OPT_VERSION,
};

struct bx_fmt_options {
    const char* progname;
    struct bx_fmt_engine_options engine;
    char* prefix_storage;
    const char** files;
    size_t file_count;
    bool show_help;
    bool show_version;
};

static void bx_fmt_print_help(FILE *stream, const char *progname) {
    fprintf(stream, "Usage: %s [-WIDTH] [OPTION]... [FILE]...\n", progname);
    fprintf(stream, "Reformat each paragraph in the FILE(s), writing to standard output.\n");
    fprintf(stream, "The option -WIDTH is an abbreviated form of --width=DIGITS.\n");
    fprintf(stream, "\n");
    fprintf(stream, "With no FILE, or when FILE is -, read standard input.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -c, --crown-margin     preserve indentation of first two lines\n");
    fprintf(stream, "  -p, --prefix=STRING    reformat only lines beginning with STRING,\n");
    fprintf(stream, "                           reattaching the prefix to reformatted lines\n");
    fprintf(stream, "  -s, --split-only       split long lines, but do not refill\n");
    fprintf(stream, "  -t, --tagged-paragraph indentation of first line different from second\n");
    fprintf(stream, "  -u, --uniform-spacing  one space between words, two after sentences\n");
    fprintf(stream, "  -w, --width=WIDTH      maximum line width (default of 75 columns)\n");
    fprintf(stream, "  -g, --goal=WIDTH       goal width (default of 93%% of width)\n");
    fprintf(stream, "      --help             display this help and exit\n");
    fprintf(stream, "      --version          output version information and exit\n");
}

static void bx_fmt_options_init(struct bx_fmt_options *options, const char *progname) {
    memset(options, 0, sizeof(*options));
    options->progname = progname;
    options->engine.width = BX_FMT_ENGINE_DEFAULT_WIDTH;
    options->engine.goal = bx_fmt_engine_default_goal_width(options->engine.width);
    options->engine.prefix = "";
}

static void bx_fmt_options_dispose(struct bx_fmt_options *options) {
    free(options->prefix_storage);
    free(options->files);
}

static bool bx_fmt_is_legacy_width_arg(const char *arg) {
    if (arg == NULL || arg[0] != '-' || arg[1] == '\0' || arg[1] == '-') {
        return false;
    }
    for (size_t i = 1; arg[i] != '\0'; i++) {
        if (arg[i] < '0' || arg[i] > '9') {
            return false;
        }
    }
    return true;
}

static void bx_fmt_set_prefix(struct bx_fmt_options *options, const char *arg) {
    char *prefix;
    char *end;

    options->prefix_storage = xstrdup(arg);
    prefix = options->prefix_storage;
    options->engine.prefix_lead_space = 0u;
    while (*prefix == ' ') {
        options->engine.prefix_lead_space++;
        prefix++;
    }
    options->engine.prefix = prefix;
    options->engine.prefix_full_length = strlen(prefix);
    end = prefix + options->engine.prefix_full_length;
    while (end > prefix && end[-1] == ' ') {
        end--;
    }
    *end = '\0';
    options->engine.prefix_length = (size_t)(end - prefix);
}

static bool bx_fmt_parse_decimal_range(const char *text, size_t min_value,
                                       size_t max_value, int range_errno,
                                       size_t *value_out) {
    uintmax_t value;
    char *end = NULL;

    if (text == NULL || text[0] == '\0' || text[0] == '-' || value_out == NULL ||
        min_value > max_value) {
        errno = 0;
        return false;
    }

    errno = 0;
    value = strtoumax(text, &end, 10);
    if (errno == ERANGE) {
        return false;
    }
    if (end == text || end == NULL || end[0] != '\0') {
        errno = 0;
        return false;
    }
    if (value < (uintmax_t)min_value) {
        errno = 0;
        return false;
    }
    if (value > (uintmax_t)max_value) {
        errno = range_errno;
        return false;
    }

    *value_out = (size_t)value;
    return true;
}

static void bx_fmt_diag_invalid_width(struct bx_diag_ctx *diag, const char *text) {
    if (errno != 0) {
        const char *detail = strerror(errno);

        if (errno == ERANGE) {
            detail = "Numerical result out of range";
        } else if (errno == EOVERFLOW) {
            detail = "Value too large for defined data type";
        }
        bx_diag(diag, "invalid width: '%s': %s", text, detail);
    } else {
        bx_diag(diag, "invalid width: '%s'", text);
    }
}

static bool bx_fmt_parse_options(int argc, char** argv, struct bx_fmt_options* options, struct bx_diag_ctx* diag) {
    static const struct bx_option_spec specs[] = {
        {'c', 'c', "crown-margin", BX_OPTION_NO_VALUE},     {'p', 'p', "prefix", BX_OPTION_REQUIRED_VALUE},    {'s', 's', "split-only", BX_OPTION_NO_VALUE},
        {'t', 't', "tagged-paragraph", BX_OPTION_NO_VALUE}, {'u', 'u', "uniform-spacing", BX_OPTION_NO_VALUE}, {'w', 'w', "width", BX_OPTION_REQUIRED_VALUE},
        {'g', 'g', "goal", BX_OPTION_REQUIRED_VALUE},       {BX_FMT_OPT_HELP, 0, "help", BX_OPTION_NO_VALUE},  {BX_FMT_OPT_VERSION, 0, "version", BX_OPTION_NO_VALUE},
    };
    const char* max_width_text = NULL;
    const char* goal_width_text = NULL;
    const char* prefix_text = NULL;
    size_t first = argc > 0 ? 1u : 0u;
    if (argc > 1 && bx_fmt_is_legacy_width_arg(argv[1])) {
        max_width_text = argv[first] + 1;
        first++;
    }

    struct bx_option_cursor cursor;
    bx_option_cursor_init(&cursor, argv, first, (size_t)argc, BX_OPTION_ORDERED | BX_OPTION_ABBREVIATE, specs, sizeof(specs) / sizeof(specs[0]));
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
            else if (option.short_name >= '0' && option.short_name <= '9')
                bx_diag(diag, "invalid option -- %c; -WIDTH is recognized only when it is the first\noption; use -w N instead", option.short_name);
            else if (option.short_name)
                bx_diag(diag, "invalid option -- '%c'", option.short_name);
            else
                bx_diag(diag, "unrecognized option '%s'", option.token);
            bx_cli_print_try_help(options->progname);
            return false;
        }
        switch (option.spec->id) {
            case 'c':
                options->engine.crown_margin = true;
                break;
            case 's':
                options->engine.split_only = true;
                break;
            case 't':
                options->engine.tagged_paragraph = true;
                break;
            case 'u':
                options->engine.uniform_spacing = true;
                break;
            case 'w':
                max_width_text = option.value;
                break;
            case 'g':
                goal_width_text = option.value;
                break;
            case 'p':
                prefix_text = option.value;
                break;
            case BX_FMT_OPT_HELP:
                options->show_help = true;
                return true;
            case BX_FMT_OPT_VERSION:
                options->show_version = true;
                return true;
            default:
                return false;
        }
    }

    if (max_width_text != NULL) {
        size_t value = 0u;

        if (!bx_fmt_parse_decimal_range(max_width_text, 0u,
                                        BX_FMT_ENGINE_MAX_WIDTH, ERANGE,
                                        &value)) {
            bx_fmt_diag_invalid_width(diag, max_width_text);
            return false;
        }
        options->engine.width = value;
    }

    if (goal_width_text != NULL) {
        size_t value = 0u;

        if (!bx_fmt_parse_decimal_range(goal_width_text, 0u,
                                        options->engine.width, EOVERFLOW,
                                        &value)) {
            bx_fmt_diag_invalid_width(diag, goal_width_text);
            return false;
        }
        options->engine.goal = value;
        if (max_width_text == NULL) {
            options->engine.width = value + 10u;
        }
    } else {
        options->engine.goal =
            bx_fmt_engine_default_goal_width(options->engine.width);
    }

    if (prefix_text != NULL)
        bx_fmt_set_prefix(options, prefix_text);
    return true;
}

static int bx_fmt_process_stream(FILE *stream, bool is_stdio, const char *path,
                                 const struct bx_fmt_options *options,
                                 struct bx_line_writer *writer,
                                 struct bx_diag_ctx *diag) {
    bool ok = bx_fmt_engine_process_stream(stream, &options->engine, writer, diag);

    if (ok && ferror(stream)) {
        int saved_errno = errno != 0 ? errno : EIO;

        errno = saved_errno;
        if (is_stdio) {
            bx_diag(diag, "read error: %s", strerror(saved_errno));
        } else {
            bx_diag(diag, "error reading '%s': %s", path, strerror(saved_errno));
        }
        ok = false;
    }

    if (is_stdio) {
        clearerr(stream);
    } else if (fclose(stream) != 0) {
        if (ok) {
            bx_diag(diag, "error reading '%s': %s", path, strerror(errno));
        }
        ok = false;
    }

    return ok ? 0 : 1;
}

static int bx_fmt_process_path(const char *path, const struct bx_fmt_options *options,
                               struct bx_line_writer *writer,
                               struct bx_diag_ctx *diag) {
    bool is_stdio = false;
    FILE *stream = bx_fopen_dash(path, "r", &is_stdio);

    if (stream == NULL) {
        bx_diag(diag, "cannot open '%s' for reading: %s", path, strerror(errno));
        return 1;
    }

    return bx_fmt_process_stream(stream, is_stdio, path, options, writer, diag);
}

int bx_fmt_main(int argc, char **argv) {
    struct bx_fmt_options options;
    struct bx_diag_ctx diag;
    int exit_status = 0;
    char output_buffer[BX_FMT_OUTPUT_BUFFER_SIZE];
    struct bx_line_writer writer;
    const char *progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "fmt");

    bx_fmt_options_init(&options, progname);
    diag = (struct bx_diag_ctx){.progname = progname, .exit_status = 0};

    if (!bx_fmt_parse_options(argc, argv, &options, &diag)) {
        bx_fmt_options_dispose(&options);
        return 1;
    }
    if (options.show_help) {
        bx_fmt_print_help(stdout, options.progname);
        bx_fmt_options_dispose(&options);
        return 0;
    }
    if (options.show_version) {
        bx_cli_print_version(options.progname);
        bx_fmt_options_dispose(&options);
        return 0;
    }

    bx_line_writer_init(&writer, STDOUT_FILENO, output_buffer, sizeof(output_buffer));

    if (options.file_count == 0) {
        exit_status = bx_fmt_process_stream(stdin, true, "-", &options, &writer,
                                            &diag);
    }
    else {
        for (size_t i = 0; i < options.file_count; i++) {
            int rc = bx_fmt_process_path(options.files[i], &options, &writer, &diag);

            if (rc != 0) {
                exit_status = 1;
            }
            if (bx_line_writer_error(&writer) != 0) {
                exit_status = 1;
                break;
            }
        }
    }

    if (bx_line_writer_error(&writer) != 0 || !bx_line_writer_flush(&writer)) {
        if (bx_line_writer_error(&writer) == 0 && errno != 0) {
            bx_diag(&diag, "write error: %s", strerror(errno));
        }
        exit_status = 1;
    }

    bx_fmt_options_dispose(&options);
    return exit_status == 0 ? 0 : 1;
}
