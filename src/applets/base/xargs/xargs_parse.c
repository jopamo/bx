#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "applets.h"
#include "lib/argv_packer.h"
#include "xargs_parse.h"
#include "lib/args_common.h"
#include "lib/option_scan.h"

static void xargs_warn_mutex(const char *progname, const char *left,
                             const char *right, const char *ignored) {
    fprintf(stderr,
            "%s: warning: options %s and %s are mutually exclusive, ignoring previous %s value\n",
            progname, left, right, ignored);
}

static void xargs_disable_replace(struct xargs_opts *opts) {
    opts->replace_mode = false;
    opts->replace_marker = NULL;
}

static void xargs_set_max_args(struct xargs_opts *opts, const char *progname,
                               int value) {
    if (opts->replace_mode) {
        xargs_warn_mutex(progname, "--replace", "--max-args/-n",
                         "--replace");
        xargs_disable_replace(opts);
    }
    if (opts->max_lines > 0) {
        xargs_warn_mutex(progname, "--max-lines", "--max-args/-n",
                         "--max-lines");
        opts->max_lines = 0;
    }
    opts->max_args = value;
}

static void xargs_set_max_lines(struct xargs_opts *opts,
                                const char *progname, const char *optname,
                                int value) {
    if (opts->replace_mode) {
        xargs_warn_mutex(progname, "--replace", optname, "--replace");
        xargs_disable_replace(opts);
    }
    if (opts->max_args > 0) {
        xargs_warn_mutex(progname, "--max-args", optname, "--max-args");
        opts->max_args = 0;
    }
    opts->max_lines = value;
}

static void xargs_set_replace_mode(struct xargs_opts *opts,
                                   const char *progname,
                                   const char *marker) {
    if (opts->max_args > 0) {
        xargs_warn_mutex(progname, "--max-args", "--replace/-I/-i",
                         "--max-args");
        opts->max_args = 0;
    }
    if (opts->max_lines > 0) {
        xargs_warn_mutex(progname, "--max-lines", "--replace/-I/-i",
                         "--max-lines");
        opts->max_lines = 0;
    }
    opts->replace_mode = true;
    opts->replace_marker = marker;
}

static void xargs_print_help(const char *progname) {
    printf("Usage: %s [OPTION]... [COMMAND [INITIAL-ARGS]...]\n", progname);
    puts("Run COMMAND with arguments read from standard input.");
    puts("");
    puts("  -0, --null              items are separated by NUL, not whitespace");
    puts("  -a, --arg-file=FILE     read items from FILE instead of standard input");
    puts("  -d, --delimiter=CHAR    items are separated by CHAR");
    puts("  -E, --eof=END           stop reading input after END");
    puts("  -n, --max-args=MAX      use at most MAX input items per command line");
    puts("  -L, --max-lines=MAX     use at most MAX input lines per command line");
    puts("  -l[MAX]                 like -L, defaulting MAX to 1");
    puts("  -I, --replace=R         replace R in initial arguments with each input item");
    puts("  -i[R]                   like -I, defaulting R to {}");
    puts("  -o, --open-tty          reopen standard input as /dev/tty in the child");
    puts("  -p, --interactive       prompt before running commands");
    puts("  -t, --verbose           print each command before running it");
    puts("      --process-slot-var=VAR  set VAR to the worker slot number in each child");
    puts("  -s, --max-chars=MAX     use at most MAX command-line bytes");
    puts("  -x, --exit              fail if a command line would be too large");
    puts("  -P, --max-procs=MAX     run at most MAX commands at a time");
    puts("  -r, --no-run-if-empty   do not run command if there are no items");
    puts("      --help              display this help and exit");
    puts("      --show-limits       display command-line limits");
    puts("      --version           output version information and exit");
}

static void xargs_print_version(void) {
    printf("xargs (bx) %s\n", BX_VERSION);
}

static void xargs_print_limits(void) {
    long arg_max = sysconf(_SC_ARG_MAX);
    if (arg_max < 0)
        arg_max = 0;

    size_t env_bytes = bx_argv_environment_bytes();

    printf("Your environment variables take up %zu bytes\n", env_bytes);
    printf("POSIX upper limit on argument length (this system): %ld\n",
           arg_max);
    if (arg_max > 0 && env_bytes < (size_t)arg_max)
        printf("Maximum command length we could try to use: %ld\n",
               arg_max - (long)env_bytes);
}

static bool xargs_parse_int(const char *progname, const char *optname,
                            const char *text, int *out, bool allow_zero) {
    int min = allow_zero ? 0 : 1;
    if (!bx_args_parse_int_range(text, min, 100000, out)) {
        fprintf(stderr, "%s: invalid argument for %s: %s\n", progname,
                optname, text ? text : "(null)");
        return false;
    }
    return true;
}

static bool xargs_parse_delimiter_byte(const char *digits, int base,
                                       unsigned char *out) {
    if (!digits || digits[0] == '\0' || digits[0] == '-' || !out)
        return false;

    errno = 0;
    char *end = NULL;
    uintmax_t value = strtoumax(digits, &end, base);
    if (errno != 0 || end == digits || end == NULL || *end != '\0' ||
        value > 255)
        return false;

    *out = (unsigned char)value;
    return true;
}

static bool xargs_parse_delimiter(const char *progname, const char *text,
                                  char *out) {
    if (!text || *text == '\0') {
        fprintf(stderr, "%s: invalid delimiter: %s\n", progname,
                text ? text : "(null)");
        return false;
    }

    if (text[0] != '\\') {
        if (text[1] != '\0') {
            fprintf(stderr, "%s: invalid delimiter: %s\n", progname, text);
            return false;
        }
        *out = text[0];
        return true;
    }

    if (text[1] == '\0') {
        fprintf(stderr, "%s: invalid delimiter: %s\n", progname, text);
        return false;
    }

    if (text[1] == 'x') {
        unsigned char byte = 0;
        if (!xargs_parse_delimiter_byte(text + 2, 16, &byte)) {
            fprintf(stderr, "%s: invalid delimiter escape: %s\n", progname,
                    text);
            return false;
        }
        *out = (char)byte;
        return true;
    }

    if (text[1] >= '0' && text[1] <= '7') {
        unsigned char byte = 0;
        if (!xargs_parse_delimiter_byte(text + 1, 8, &byte)) {
            fprintf(stderr, "%s: invalid delimiter escape: %s\n", progname,
                    text);
            return false;
        }
        *out = (char)byte;
        return true;
    }

    switch (text[1]) {
    case 'n':
        *out = '\n';
        break;
    case 't':
        *out = '\t';
        break;
    case 'r':
        *out = '\r';
        break;
    case '\\':
        *out = '\\';
        break;
    default:
        fprintf(stderr, "%s: invalid delimiter escape: %s\n", progname,
                text);
        return false;
    }
    if (text[2] != '\0') {
        fprintf(stderr, "%s: invalid delimiter: %s\n", progname, text);
        return false;
    }
    return true;
}

bool xargs_parse_main_args(int argc, char **argv, struct xargs_main_args *out) {
    enum { OPT_HELP = 256, OPT_VERSION, OPT_SHOW_LIMITS, OPT_SLOT_VAR };
    static const struct bx_option_spec specs[] = {
        {OPT_HELP, 0, "help", BX_OPTION_NO_VALUE},
        {OPT_VERSION, 0, "version", BX_OPTION_NO_VALUE},
        {OPT_SHOW_LIMITS, 0, "show-limits", BX_OPTION_NO_VALUE},
        {'E', 'E', NULL, BX_OPTION_REQUIRED_VALUE},
        {'e', 'e', "eof", BX_OPTION_OPTIONAL_VALUE},
        {'0', '0', "null", BX_OPTION_NO_VALUE},
        {'d', 'd', "delimiter", BX_OPTION_REQUIRED_VALUE},
        {'x', 'x', "exit", BX_OPTION_NO_VALUE},
        {'r', 'r', "no-run-if-empty", BX_OPTION_NO_VALUE},
        {'n', 'n', "max-args", BX_OPTION_REQUIRED_VALUE},
        {'L', 'L', "max-lines", BX_OPTION_REQUIRED_VALUE},
        {'l', 'l', NULL, BX_OPTION_OPTIONAL_VALUE},
        {'s', 's', "max-chars", BX_OPTION_REQUIRED_VALUE},
        {'o', 'o', "open-tty", BX_OPTION_NO_VALUE},
        {'p', 'p', "interactive", BX_OPTION_NO_VALUE},
        {'t', 't', "verbose", BX_OPTION_NO_VALUE},
        {'I', 'I', NULL, BX_OPTION_REQUIRED_VALUE},
        {'i', 'i', "replace", BX_OPTION_OPTIONAL_VALUE},
        {OPT_SLOT_VAR, 0, "process-slot-var", BX_OPTION_REQUIRED_VALUE},
        {'P', 'P', "max-procs", BX_OPTION_REQUIRED_VALUE},
        {'a', 'a', "arg-file", BX_OPTION_REQUIRED_VALUE},
    };

    memset(out, 0, sizeof(*out));
    out->progname = argv[0] ? argv[0] : "xargs";
    out->opts.max_procs = 1;
    out->input = stdin;

    struct bx_option_cursor cursor;
    bx_option_cursor_init(&cursor, argv, 1, (size_t)argc, BX_OPTION_PREFIX | BX_OPTION_ABBREVIATE,
                          specs, sizeof(specs) / sizeof(specs[0]));
    struct bx_option_result result;
    while ((result = bx_option_next(&cursor)).event != BX_OPTION_END) {
        if (result.event != BX_OPTION_MATCH) {
            if (result.event == BX_OPTION_MISSING_VALUE) {
                if (result.spec->short_name)
                    fprintf(stderr, "%s: option requires an argument -- '-%c'\n",
                            out->progname, result.spec->short_name);
                else
                    fprintf(stderr, "%s: option requires an argument -- '%s'\n",
                            out->progname, result.token);
            } else if (result.short_name) {
                fprintf(stderr, "%s: invalid option -- '%c'\n", out->progname, result.short_name);
            } else {
                fprintf(stderr, "%s: unrecognized option '%s'\n", out->progname, result.token);
            }
            out->exit_code = 1;
            return false;
        }
        const char *value = result.value;
        switch (result.spec->id) {
        case 'r':
            out->opts.no_run_if_empty = true;
            break;
        case 'o':
            out->opts.open_tty = true;
            break;
        case 'p':
            out->opts.interactive = true;
            break;
        case 't':
            out->opts.verbose = true;
            break;
        case 'x':
            out->opts.exit_if_too_big = true;
            break;
        case '0':
            out->opts.nul_delim = true;
            break;
        case 'd':
            if (!xargs_parse_delimiter(out->progname, value,
                                       &out->opts.delimiter)) {
                out->exit_code = 1;
                return false;
            }
            out->opts.delimiter_mode = true;
            break;
        case 'I':
            xargs_set_replace_mode(&out->opts, out->progname, value);
            break;
        case 'i':
            xargs_set_replace_mode(&out->opts, out->progname,
                                   value ? value : "{}");
            break;
        case 'E':
        case 'e':
            out->opts.logical_eof = value;
            break;
        case 'a':
            out->opts.arg_file = value;
            break;
        case 'n':
            if (!xargs_parse_int(out->progname, "-n", value,
                                 &out->opts.max_args, false)) {
                out->exit_code = 1;
                return false;
            }
            xargs_set_max_args(&out->opts, out->progname, out->opts.max_args);
            break;
        case 's':
            if (!xargs_parse_int(out->progname, "-s", value,
                                 &out->opts.max_chars, false)) {
                out->exit_code = 1;
                return false;
            }
            break;
        case 'L':
            if (!xargs_parse_int(out->progname, "-L", value,
                                 &out->opts.max_lines, false)) {
                out->exit_code = 1;
                return false;
            }
            xargs_set_max_lines(&out->opts, out->progname, "-L",
                                out->opts.max_lines);
            break;
        case 'l':
            if (value) {
                if (!xargs_parse_int(out->progname, "-l", value,
                                     &out->opts.max_lines, false)) {
                    out->exit_code = 1;
                    return false;
                }
                xargs_set_max_lines(&out->opts, out->progname, "-l",
                                    out->opts.max_lines);
            } else {
                xargs_set_max_lines(&out->opts, out->progname, "-l", 1);
            }
            break;
        case 'P':
            if (!xargs_parse_int(out->progname, "-P", value,
                                 &out->opts.max_procs, true)) {
                out->exit_code = 1;
                return false;
            }
            break;
        case OPT_HELP:
            xargs_print_help(out->progname);
            out->exit_code = 0;
            return false;
        case OPT_VERSION:
            xargs_print_version();
            out->exit_code = 0;
            return false;
        case OPT_SHOW_LIMITS:
            xargs_print_limits();
            out->exit_code = 0;
            return false;
        case OPT_SLOT_VAR:
            if (value[0] == '\0' || strchr(value, '=') != NULL) {
                fprintf(stderr, "%s: invalid variable name for --process-slot-var: '%s'\n",
                        out->progname, value);
                out->exit_code = 1;
                return false;
            }
            out->opts.process_slot_var = value;
            break;
        default:
            out->exit_code = 1;
            return false;
        }
    }

    if (out->opts.nul_delim || out->opts.delimiter_mode)
        out->opts.logical_eof = NULL;
    if (out->opts.replace_mode && !out->opts.replace_marker)
        out->opts.replace_marker = "{}";

    static char *default_command[] = {"echo", NULL};
    int command_index = (int)result.index;
    out->command = (command_index < argc) ? &argv[command_index] : default_command;
    out->command_argc = (command_index < argc) ? (argc - command_index) : 1;

    if (out->opts.arg_file) {
        out->input = fopen(out->opts.arg_file,
                           out->opts.nul_delim ? "rb" : "r");
        if (!out->input) {
            fprintf(stderr, "%s: %s: %s\n", out->progname, out->opts.arg_file,
                    strerror(errno));
            out->exit_code = 1;
            return false;
        }
        out->close_input = true;
    }

    return true;
}

void xargs_free_main_args(struct xargs_main_args *args) {
    if (!args)
        return;
    if (args->close_input && args->input)
        fclose(args->input);
    args->input = NULL;
    args->close_input = false;
}
