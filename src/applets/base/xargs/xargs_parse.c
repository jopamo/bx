#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "applets.h"
#include "lib/argv_packer.h"
#include "xargs_parse.h"

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
    char *end = NULL;
    long v = strtol(text, &end, 10);
    long min = allow_zero ? 0 : 1;
    if (!text || *text == '\0' || (end && *end != '\0') || v < min ||
        v > 100000) {
        fprintf(stderr, "%s: invalid argument for %s: %s\n", progname,
                optname, text ? text : "(null)");
        return false;
    }
    *out = (int)v;
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
        char *end = NULL;
        long v = strtol(text + 2, &end, 16);
        if (!end || end == text + 2 || *end != '\0' || v < 0 || v > 255) {
            fprintf(stderr, "%s: invalid delimiter escape: %s\n", progname,
                    text);
            return false;
        }
        *out = (char)v;
        return true;
    }

    if (text[1] >= '0' && text[1] <= '7') {
        char *end = NULL;
        long v = strtol(text + 1, &end, 8);
        if (!end || end == text + 1 || *end != '\0' || v < 0 || v > 255) {
            fprintf(stderr, "%s: invalid delimiter escape: %s\n", progname,
                    text);
            return false;
        }
        *out = (char)v;
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
    static struct option long_opts[] = {
        {"help", no_argument, NULL, 200},
        {"version", no_argument, NULL, 201},
        {"show-limits", no_argument, NULL, 202},
        {"eof", optional_argument, NULL, 203},
        {"null", no_argument, NULL, '0'},
        {"delimiter", required_argument, NULL, 'd'},
        {"exit", no_argument, NULL, 'x'},
        {"no-run-if-empty", no_argument, NULL, 'r'},
        {"max-args", required_argument, NULL, 'n'},
        {"max-lines", required_argument, NULL, 'L'},
        {"max-chars", required_argument, NULL, 's'},
        {"open-tty", no_argument, NULL, 'o'},
        {"interactive", no_argument, NULL, 'p'},
        {"verbose", no_argument, NULL, 't'},
        {"replace", optional_argument, NULL, 'i'},
        {"process-slot-var", required_argument, NULL, 204},
        {"max-procs", required_argument, NULL, 'P'},
        {"arg-file", required_argument, NULL, 'a'},
        {NULL, 0, NULL, 0},
    };

    memset(out, 0, sizeof(*out));
    out->progname = argv[0] ? argv[0] : "xargs";
    out->opts.max_procs = 1;
    out->input = stdin;

    opterr = 0;
    optind = 1;

    int c;
    while ((c = getopt_long(argc, argv, "+0d:E:e::I:i::l::L:oprs:txa:n:P:",
                            long_opts, NULL)) != -1) {
        switch (c) {
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
            if (!xargs_parse_delimiter(out->progname, optarg,
                                       &out->opts.delimiter)) {
                out->exit_code = 1;
                return false;
            }
            out->opts.delimiter_mode = true;
            break;
        case 'I':
            xargs_set_replace_mode(&out->opts, out->progname, optarg);
            break;
        case 'i':
            xargs_set_replace_mode(&out->opts, out->progname,
                                   optarg ? optarg : "{}");
            break;
        case 'E':
            out->opts.logical_eof = optarg;
            break;
        case 'e':
            out->opts.logical_eof = optarg ? optarg : NULL;
            break;
        case 'a':
            out->opts.arg_file = optarg;
            break;
        case 'n':
            if (!xargs_parse_int(out->progname, "-n", optarg,
                                 &out->opts.max_args, false)) {
                out->exit_code = 1;
                return false;
            }
            xargs_set_max_args(&out->opts, out->progname, out->opts.max_args);
            break;
        case 's':
            if (!xargs_parse_int(out->progname, "-s", optarg,
                                 &out->opts.max_chars, false)) {
                out->exit_code = 1;
                return false;
            }
            break;
        case 'L':
            if (!xargs_parse_int(out->progname, "-L", optarg,
                                 &out->opts.max_lines, false)) {
                out->exit_code = 1;
                return false;
            }
            xargs_set_max_lines(&out->opts, out->progname, "-L",
                                out->opts.max_lines);
            break;
        case 'l':
            if (optarg) {
                if (!xargs_parse_int(out->progname, "-l", optarg,
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
            if (!xargs_parse_int(out->progname, "-P", optarg,
                                 &out->opts.max_procs, true)) {
                out->exit_code = 1;
                return false;
            }
            break;
        case 200:
            xargs_print_help(out->progname);
            out->exit_code = 0;
            return false;
        case 201:
            xargs_print_version();
            out->exit_code = 0;
            return false;
        case 202:
            xargs_print_limits();
            out->exit_code = 0;
            return false;
        case 203:
            out->opts.logical_eof = optarg ? optarg : NULL;
            break;
        case 204:
            out->opts.process_slot_var = optarg;
            break;
        case '?':
            if (optopt == 'n' || optopt == 'L' || optopt == 'P' ||
                optopt == 'a' || optopt == 'd' || optopt == 'E' ||
                optopt == 'I' || optopt == 's') {
                fprintf(stderr,
                        "%s: option requires an argument -- '-%c'\n",
                        out->progname, optopt);
            } else if (optopt) {
                fprintf(stderr, "%s: invalid option -- '%c'\n",
                        out->progname, optopt);
            } else if (optind > 0 && optind <= argc) {
                fprintf(stderr, "%s: unrecognized option '%s'\n",
                        out->progname, argv[optind - 1]);
            } else {
                fprintf(stderr, "%s: unrecognized option\n", out->progname);
            }
            out->exit_code = 1;
            return false;
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
    out->command = (optind < argc) ? &argv[optind] : default_command;
    out->command_argc = (optind < argc) ? (argc - optind) : 1;

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
