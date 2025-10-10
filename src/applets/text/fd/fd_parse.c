#define _GNU_SOURCE
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "applets.h"
#include "fd_exec.h"
#include "fd_parse.h"
#include "search/metadata.h"

static bool fd_parse_nonnegative_int(const char *progname, const char *optname,
                                     const char *text, int *out) {
    char *end = NULL;
    long v = strtol(text, &end, 10);
    if (!text || *text == '\0' || (end && *end != '\0') || v < 0 ||
        v > (1 << 20)) {
        fprintf(stderr, "%s: invalid argument for %s: %s\n", progname, optname,
                text ? text : "(null)");
        return false;
    }
    *out = (int)v;
    return true;
}

static bool fd_parse_strip_cwd_prefix(const char *progname, const char *text,
                                      enum fd_strip_cwd_prefix_mode *out) {
    if (!text || strcmp(text, "auto") == 0) {
        *out = FD_STRIP_CWD_PREFIX_AUTO;
        return true;
    }
    if (strcmp(text, "always") == 0) {
        *out = FD_STRIP_CWD_PREFIX_ALWAYS;
        return true;
    }
    if (strcmp(text, "never") == 0) {
        *out = FD_STRIP_CWD_PREFIX_NEVER;
        return true;
    }

    fprintf(stderr, "%s: invalid argument for --strip-cwd-prefix: %s\n",
            progname, text);
    return false;
}

static int fd_find_exec_option(int argc, char **argv, enum fd_exec_mode *mode,
                               int *command_start,
                               const char **inline_command) {
    bool end_of_options = false;

    *mode = FD_EXEC_NONE;
    *command_start = argc;
    *inline_command = NULL;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (!end_of_options && strcmp(arg, "--") == 0) {
            end_of_options = true;
            continue;
        }
        if (end_of_options)
            continue;

        if (strcmp(arg, "-x") == 0 || strcmp(arg, "--exec") == 0) {
            *mode = FD_EXEC_EACH;
            *command_start = i + 1;
            return i;
        }
        if (strcmp(arg, "-X") == 0 || strcmp(arg, "--exec-batch") == 0) {
            *mode = FD_EXEC_BATCH;
            *command_start = i + 1;
            return i;
        }
        if (strncmp(arg, "--exec=", 7) == 0) {
            *mode = FD_EXEC_EACH;
            *command_start = i + 1;
            *inline_command = arg + 7;
            return i;
        }
        if (strncmp(arg, "--exec-batch=", 13) == 0) {
            *mode = FD_EXEC_BATCH;
            *command_start = i + 1;
            *inline_command = arg + 13;
            return i;
        }
    }

    return argc;
}

static bool fd_parse_type_filter(const char *progname, const char *text,
                                 const char **out) {
    char type_filter = '\0';
    if (!bx_walk_parse_named_type_filter(text, &type_filter)) {
        fprintf(stderr, "%s: invalid argument for --type: %s\n", progname,
                text);
        return false;
    }

    switch (type_filter) {
    case 'f':
        *out = "f";
        break;
    case 'd':
        *out = "d";
        break;
    case 'l':
        *out = "l";
        break;
    case 'x':
        *out = "x";
        break;
    case 'e':
        *out = "e";
        break;
    case 'p':
        *out = "p";
        break;
    case 's':
        *out = "s";
        break;
    case 'b':
        *out = "b";
        break;
    case 'c':
        *out = "c";
        break;
    default:
        fprintf(stderr, "%s: invalid argument for --type: %s\n", progname,
                text);
        return false;
    }
    return true;
}

static void fd_print_help(const char *progname) {
    printf("Usage: %s [OPTIONS] [PATTERN] [PATH]...\n", progname);
    puts("fd - find entries in the filesystem");
    puts("");
    puts("  -H, --hidden        search hidden files and directories");
    puts("  -I, --no-ignore     do not respect ignore files");
    puts("  -u                  search hidden and ignored files");
    puts("      --no-ignore-parent do not respect ignore files in parent directories");
    puts("      --no-ignore-vcs do not respect VCS ignore files");
    puts("      --no-require-git use .gitignore outside git repositories");
    puts("  -a, --absolute-path show absolute paths");
    puts("      --relative-path show relative paths");
    puts("      --path-separator SEP replace '/' in rendered paths with SEP");
    puts("      --show-errors    print permission and traversal errors");
    puts("      --strip-cwd-prefix[=WHEN] control leading ./ rendering (auto, always, never)");
    puts("      --format FMT     print results according to a template");
    puts("  -l, --list-details   use a detailed listing format");
    puts("  -p, --full-path     match against full path, not basename");
    puts("  -i, --ignore-case   case-insensitive matching");
    puts("  -s, --case-sensitive  case-sensitive matching");
    puts("  -F, --fixed-strings treat pattern as literal string");
    puts("  -g, --glob          glob-based matching");
    puts("  -E, --exclude GLOB  exclude paths matching GLOB");
    puts("      --regex         treat pattern as a regular expression");
    puts("      --and PATTERN   require PATTERN to match too");
    puts("  -d, --max-depth N   limit recursive depth");
    puts("      --min-depth N   skip matches shallower than N");
    puts("      --exact-depth N match only entries exactly at depth N");
    puts("  -t, --type TYPE     filter by type: f,d,l,x,e,p,s,b,c");
    puts("  -e, --extension EXT filter by file extension");
    puts("  -0, --print0        separate results by NUL byte");
    puts("  -q, --quiet         suppress normal output");
    puts("  -1                  alias for --max-results=1");
    puts("  -L, --follow        follow symlinks");
    puts("  -x, --exec CMD ...  run CMD once per search result");
    puts("  -X, --exec-batch CMD ... run CMD once with batched search results");
    puts("      --batch-size N   limit results per --exec-batch invocation");
    puts("      --max-results N limit number of results");
    puts("      --help           display this help and exit");
    puts("      --version        output version information and exit");
}

static bool fd_prepare_exec_argv(const char *progname, int argc, char **argv,
                                 struct fd_main_args *out,
                                 int *parse_argc_out) {
    const char *inline_exec_command = NULL;
    int exec_command_start = argc;
    int parse_argc = fd_find_exec_option(argc, argv, &out->opts.exec_mode,
                                         &exec_command_start,
                                         &inline_exec_command);

    if (out->opts.exec_mode == FD_EXEC_NONE) {
        *parse_argc_out = parse_argc;
        return true;
    }

    int exec_argc =
        argc - exec_command_start + (inline_exec_command ? 1 : 0);
    if (exec_argc <= 0 ||
        (inline_exec_command && inline_exec_command[0] == '\0')) {
        fprintf(stderr, "%s: %s requires a command\n", progname,
                out->opts.exec_mode == FD_EXEC_BATCH ? "--exec-batch"
                                                     : "--exec");
        out->exit_code = 2;
        return false;
    }

    out->exec_argv_storage =
        calloc((size_t)exec_argc + 1, sizeof(*out->exec_argv_storage));
    if (!out->exec_argv_storage) {
        out->exit_code = 1;
        return false;
    }

    if (inline_exec_command) {
        out->exec_argv_storage[0] = inline_exec_command;
        for (int i = 1; i < exec_argc; i++)
            out->exec_argv_storage[i] = argv[exec_command_start + i - 1];
    } else {
        for (int i = 0; i < exec_argc; i++)
            out->exec_argv_storage[i] = argv[exec_command_start + i];
    }

    out->opts.exec_argv = out->exec_argv_storage;
    out->opts.exec_argc = exec_argc;
    *parse_argc_out = parse_argc;
    return true;
}

static bool fd_validate_main_args(struct fd_main_args *out) {
    const char *progname = out->progname;
    struct fd_opts *opts = &out->opts;

    if (opts->strip_cwd_prefix != FD_STRIP_CWD_PREFIX_UNSET &&
        !out->using_implicit_root) {
        fprintf(stderr,
                "%s: --strip-cwd-prefix cannot be used with explicit search paths\n",
                progname);
        out->exit_code = 2;
        return false;
    }

    if (opts->exec_mode != FD_EXEC_NONE) {
        if (opts->quiet) {
            fprintf(stderr, "%s: --quiet cannot be used with %s\n", progname,
                    opts->exec_mode == FD_EXEC_BATCH ? "--exec-batch"
                                                     : "--exec");
            out->exit_code = 2;
            return false;
        }
        if (opts->max_results > 0) {
            fprintf(stderr, "%s: --max-results cannot be used with %s\n",
                    progname,
                    opts->exec_mode == FD_EXEC_BATCH ? "--exec-batch"
                                                     : "--exec");
            out->exit_code = 2;
            return false;
        }
        if (opts->print0) {
            fprintf(stderr, "%s: --print0 cannot be used with %s\n", progname,
                    opts->exec_mode == FD_EXEC_BATCH ? "--exec-batch"
                                                     : "--exec");
            out->exit_code = 2;
            return false;
        }
        if (opts->exec_mode == FD_EXEC_BATCH &&
            fd_count_placeholder_args(opts) > 1) {
            fprintf(stderr,
                    "%s: only one placeholder-bearing argument is allowed with --exec-batch\n",
                    progname);
            out->exit_code = 2;
            return false;
        }
    } else if (opts->batch_size_set) {
        fprintf(stderr, "%s: --batch-size requires --exec-batch\n", progname);
        out->exit_code = 2;
        return false;
    }

    if (opts->list_details) {
        if (opts->output_format) {
            fprintf(stderr,
                    "%s: --list-details cannot be used with --format\n",
                    progname);
            out->exit_code = 2;
            return false;
        }
        if (opts->exec_mode != FD_EXEC_NONE) {
            fprintf(stderr, "%s: --list-details cannot be used with %s\n",
                    progname,
                    opts->exec_mode == FD_EXEC_BATCH ? "--exec-batch"
                                                     : "--exec");
            out->exit_code = 2;
            return false;
        }
        if (opts->print0) {
            fprintf(stderr,
                    "%s: --list-details cannot be used with --print0\n",
                    progname);
            out->exit_code = 2;
            return false;
        }
        if (opts->quiet) {
            fprintf(stderr, "%s: --list-details cannot be used with --quiet\n",
                    progname);
            out->exit_code = 2;
            return false;
        }
        if (opts->max_results > 0) {
            fprintf(stderr,
                    "%s: --list-details cannot be used with --max-results\n",
                    progname);
            out->exit_code = 2;
            return false;
        }
        if (opts->absolute_path) {
            fprintf(stderr,
                    "%s: --list-details cannot be used with --absolute-path\n",
                    progname);
            out->exit_code = 2;
            return false;
        }
    }

    return true;
}

bool fd_parse_main_args(int argc, char **argv, struct fd_main_args *out) {
    static struct option long_opts[] = {
        {"help", no_argument, NULL, 'h'},
        {"version", no_argument, NULL, 'V'},
        {"hidden", no_argument, NULL, 'H'},
        {"no-ignore", no_argument, NULL, 'I'},
        {"no-ignore-parent", no_argument, NULL, 210},
        {"no-ignore-vcs", no_argument, NULL, 211},
        {"no-require-git", no_argument, NULL, 212},
        {"absolute-path", no_argument, NULL, 'a'},
        {"relative-path", no_argument, NULL, 205},
        {"follow", no_argument, NULL, 'L'},
        {"full-path", no_argument, NULL, 'p'},
        {"ignore-case", no_argument, NULL, 'i'},
        {"case-sensitive", no_argument, NULL, 's'},
        {"fixed-strings", no_argument, NULL, 'F'},
        {"glob", no_argument, NULL, 'g'},
        {"exclude", required_argument, NULL, 'E'},
        {"regex", no_argument, NULL, 204},
        {"max-depth", required_argument, NULL, 'd'},
        {"min-depth", required_argument, NULL, 201},
        {"exact-depth", required_argument, NULL, 202},
        {"type", required_argument, NULL, 't'},
        {"extension", required_argument, NULL, 'e'},
        {"max-results", required_argument, NULL, 200},
        {"and", required_argument, NULL, 203},
        {"path-separator", required_argument, NULL, 207},
        {"batch-size", required_argument, NULL, 206},
        {"show-errors", no_argument, NULL, 209},
        {"strip-cwd-prefix", optional_argument, NULL, 208},
        {"format", required_argument, NULL, 213},
        {"list-details", no_argument, NULL, 'l'},
        {"exec", required_argument, NULL, 'x'},
        {"exec-batch", required_argument, NULL, 'X'},
        {"print0", no_argument, NULL, '0'},
        {"quiet", no_argument, NULL, 'q'},
        {NULL, 0, NULL, 0},
    };

    memset(out, 0, sizeof(*out));
    out->progname = argv[0] ? argv[0] : "fd";
    out->opts.max_depth = -1;
    out->opts.exact_depth = -1;
    out->opts.smart_case = true;
    out->using_implicit_root = true;
    out->search_path_count = 1;

    int parse_argc = argc;
    if (!fd_prepare_exec_argv(out->progname, argc, argv, out, &parse_argc))
        return false;

    bool show_help = false;
    int opt;

    optind = 1;
    opterr = 0;
    while ((opt = getopt_long(parse_argc, argv,
                              "hVHIuaplisSFgE:d:t:e:x:X:0qL1", long_opts,
                              NULL)) != -1) {
        switch (opt) {
        case 'h':
            show_help = true;
            break;
        case 'V':
            printf("fd (bx) %s\n", BX_VERSION);
            out->exit_code = 0;
            return false;
        case 'H':
            out->opts.hidden = true;
            break;
        case 'I':
            out->opts.no_ignore = true;
            break;
        case 'u':
            if (out->opts.unrestrict_level < 3)
                out->opts.unrestrict_level++;
            break;
        case 210:
            out->opts.no_ignore_parent = true;
            break;
        case 211:
            out->opts.no_ignore_vcs = true;
            break;
        case 212:
            out->opts.no_require_git = true;
            break;
        case 'a':
            out->opts.absolute_path = true;
            break;
        case 205:
            out->opts.absolute_path = false;
            break;
        case 'l':
            out->opts.list_details = true;
            break;
        case 'p':
            out->opts.full_path = true;
            break;
        case 'i':
            out->opts.ignore_case = true;
            out->opts.smart_case = false;
            break;
        case 's':
            out->opts.case_sensitive = true;
            out->opts.smart_case = false;
            break;
        case 'F':
            out->opts.fixed_strings = true;
            break;
        case 'g':
            out->opts.glob_match = true;
            break;
        case 'E':
            if (out->opts.num_exclude_patterns < FD_MAX_EXCLUDE_PATTERNS)
                out->opts.exclude_patterns[out->opts.num_exclude_patterns++] =
                    optarg;
            break;
        case 204:
            out->opts.fixed_strings = false;
            out->opts.glob_match = false;
            break;
        case 'd':
            if (!fd_parse_nonnegative_int(out->progname, "--max-depth", optarg,
                                          &out->opts.max_depth)) {
                out->exit_code = 2;
                return false;
            }
            break;
        case 201:
            if (!fd_parse_nonnegative_int(out->progname, "--min-depth", optarg,
                                          &out->opts.min_depth)) {
                out->exit_code = 2;
                return false;
            }
            break;
        case 202:
            if (!fd_parse_nonnegative_int(out->progname, "--exact-depth",
                                          optarg, &out->opts.exact_depth)) {
                out->exit_code = 2;
                return false;
            }
            out->opts.max_depth = out->opts.exact_depth;
            break;
        case 't':
            if (!fd_parse_type_filter(out->progname, optarg,
                                      &out->opts.type_filter)) {
                out->exit_code = 2;
                return false;
            }
            break;
        case 'e':
            out->opts.extension = optarg;
            break;
        case '0':
            out->opts.print0 = true;
            break;
        case 'q':
            out->opts.quiet = true;
            break;
        case 'L':
            out->opts.follow_symlinks = true;
            break;
        case '1':
            out->opts.max_results = 1;
            break;
        case 'x':
        case 'X':
            break;
        case 206:
            if (!fd_parse_nonnegative_int(out->progname, "--batch-size", optarg,
                                          &out->opts.batch_size)) {
                out->exit_code = 2;
                return false;
            }
            out->opts.batch_size_set = true;
            break;
        case 207:
            out->opts.path_separator = optarg;
            break;
        case 209:
            out->opts.show_errors = true;
            break;
        case 208:
            if (!fd_parse_strip_cwd_prefix(out->progname, optarg,
                                           &out->opts.strip_cwd_prefix)) {
                out->exit_code = 2;
                return false;
            }
            break;
        case 213:
            out->opts.output_format = optarg;
            break;
        case 203:
            if (out->opts.num_and_patterns < FD_MAX_AND_PATTERNS)
                out->opts.and_patterns[out->opts.num_and_patterns++] = optarg;
            break;
        case 200:
            if (!fd_parse_nonnegative_int(out->progname, "--max-results",
                                          optarg, &out->opts.max_results)) {
                out->exit_code = 2;
                return false;
            }
            break;
        case '?':
            if (optind > 0 && optind <= argc)
                fprintf(stderr, "%s: unrecognized option '%s'\n",
                        out->progname, argv[optind - 1]);
            else
                fprintf(stderr, "%s: unrecognized option\n", out->progname);
            out->exit_code = 2;
            return false;
        }
    }

    if (show_help) {
        fd_print_help(out->progname);
        out->exit_code = 0;
        return false;
    }

    out->opts.pattern = NULL;
    static char *default_paths[] = {"."};
    out->search_paths = default_paths;
    int positional = parse_argc - optind;
    if (positional == 1) {
        out->opts.pattern = argv[optind++];
    } else if (positional > 1) {
        out->opts.pattern = argv[optind++];
        out->search_paths = &argv[optind];
        out->search_path_count = parse_argc - optind;
        out->using_implicit_root = false;
    }

    if (out->opts.unrestrict_level >= 1) {
        out->opts.no_ignore = true;
        out->opts.hidden = true;
        out->opts.no_require_git = true;
    }

    if (!fd_validate_main_args(out))
        return false;

    return true;
}

void fd_free_main_args(struct fd_main_args *args) {
    if (!args)
        return;
    free((void *)args->exec_argv_storage);
    args->exec_argv_storage = NULL;
    args->opts.exec_argv = NULL;
    args->opts.exec_argc = 0;
}
