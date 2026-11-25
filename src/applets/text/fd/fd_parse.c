#define _GNU_SOURCE
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "applets.h"
#include "fd_exec.h"
#include "fd_parse.h"
#include "search/metadata.h"
#include "lib/args_common.h"
#include "lib/glob_syntax.h"
#include "lib/option_scan.h"
#include "lib/output_policy.h"

static bool fd_parse_nonnegative_int(const char *progname, const char *optname,
                                     const char *text, int *out) {
    if (!bx_args_parse_int_range(text, 0, 1 << 20, out)) {
        fprintf(stderr, "%s: invalid argument for %s: %s\n", progname, optname,
                text ? text : "(null)");
        return false;
    }
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

static bool fd_prepare_exec_argv(struct fd_main_args* out, int argc, char** argv, int command_start, const char* inline_command) {
    int tail_count = argc - command_start;
    const char* command = inline_command ? inline_command : tail_count > 0 ? argv[command_start] : NULL;
    if (!command || !*command) {
        fprintf(stderr, "%s: %s requires a command\n", out->progname, out->opts.exec_mode == FD_EXEC_BATCH ? "--exec-batch" : "--exec");
        out->exit_code = 2;
        return false;
    }

    int exec_argc = tail_count + (inline_command != NULL);
    out->exec_argv_storage =
        calloc((size_t)exec_argc + 1, sizeof(*out->exec_argv_storage));
    if (!out->exec_argv_storage) {
        fprintf(stderr, "%s: out of memory\n", out->progname);
        out->exit_code = 1;
        return false;
    }
    int offset = 0;
    if (inline_command)
        out->exec_argv_storage[offset++] = inline_command;
    for (int i = 0; i < tail_count; i++)
        out->exec_argv_storage[offset + i] = argv[command_start + i];
    out->opts.exec_argv = out->exec_argv_storage;
    out->opts.exec_argc = exec_argc;
    return true;
}

/* Exec transitions to the child tail only on the exact standalone spellings. */
static bool fd_exact_exec_option(const struct bx_option_result* option) {
    if (option->short_name)
        return option->token[1] == option->short_name && !option->token[2];
    size_t len = strcspn(option->token + 2, "=");
    return len == strlen(option->spec->long_name);
}

static bool fd_validate_main_args(struct fd_main_args *out) {
    const char *progname = out->progname;
    struct fd_opts *opts = &out->opts;

    if (opts->strip_cwd_prefix != FD_STRIP_CWD_PREFIX_UNSET && out->search_paths) {
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
    static const struct bx_option_spec specs[] = {
        {'h', 'h', "help", BX_OPTION_NO_VALUE},
        {'V', 'V', "version", BX_OPTION_NO_VALUE},
        {'H', 'H', "hidden", BX_OPTION_NO_VALUE},
        {'I', 'I', "no-ignore", BX_OPTION_NO_VALUE},
        {210, 0, "no-ignore-parent", BX_OPTION_NO_VALUE},
        {211, 0, "no-ignore-vcs", BX_OPTION_NO_VALUE},
        {212, 0, "no-require-git", BX_OPTION_NO_VALUE},
        {'a', 'a', "absolute-path", BX_OPTION_NO_VALUE},
        {205, 0, "relative-path", BX_OPTION_NO_VALUE},
        {'L', 'L', "follow", BX_OPTION_NO_VALUE},
        {'p', 'p', "full-path", BX_OPTION_NO_VALUE},
        {'i', 'i', "ignore-case", BX_OPTION_NO_VALUE},
        {'s', 's', "case-sensitive", BX_OPTION_NO_VALUE},
        {'F', 'F', "fixed-strings", BX_OPTION_NO_VALUE},
        {'g', 'g', "glob", BX_OPTION_NO_VALUE},
        {'E', 'E', "exclude", BX_OPTION_REQUIRED_VALUE},
        {204, 0, "regex", BX_OPTION_NO_VALUE},
        {'d', 'd', "max-depth", BX_OPTION_REQUIRED_VALUE},
        {201, 0, "min-depth", BX_OPTION_REQUIRED_VALUE},
        {202, 0, "exact-depth", BX_OPTION_REQUIRED_VALUE},
        {'t', 't', "type", BX_OPTION_REQUIRED_VALUE},
        {'e', 'e', "extension", BX_OPTION_REQUIRED_VALUE},
        {200, 0, "max-results", BX_OPTION_REQUIRED_VALUE},
        {203, 0, "and", BX_OPTION_REQUIRED_VALUE},
        {207, 0, "path-separator", BX_OPTION_REQUIRED_VALUE},
        {206, 0, "batch-size", BX_OPTION_REQUIRED_VALUE},
        {209, 0, "show-errors", BX_OPTION_NO_VALUE},
        {208, 0, "strip-cwd-prefix", BX_OPTION_OPTIONAL_VALUE},
        {213, 0, "format", BX_OPTION_REQUIRED_VALUE},
        {'l', 'l', "list-details", BX_OPTION_NO_VALUE},
        {'x', 'x', "exec", BX_OPTION_OPTIONAL_VALUE},
        {'X', 'X', "exec-batch", BX_OPTION_OPTIONAL_VALUE},
        {'0', '0', "print0", BX_OPTION_NO_VALUE},
        {'q', 'q', "quiet", BX_OPTION_NO_VALUE},
        {'u', 'u', NULL, BX_OPTION_NO_VALUE},
        {'1', '1', NULL, BX_OPTION_NO_VALUE},
    };

    memset(out, 0, sizeof(*out));
    out->progname = argc > 0 && argv[0] ? argv[0] : "fd";
    out->opts.max_depth = -1;
    out->opts.exact_depth = -1;
    out->opts.smart_case = true;
    bool show_help = false;
    struct bx_option_cursor cursor;
    bx_option_cursor_init(&cursor, argv, argc > 0 ? 1u : 0u, (size_t)argc, BX_OPTION_ORDERED | BX_OPTION_ABBREVIATE, specs, sizeof(specs) / sizeof(specs[0]));
    struct bx_option_result option;
    while ((option = bx_option_next(&cursor)).event != BX_OPTION_END) {
        if (option.event == BX_OPTION_OPERAND) {
            if (!out->opts.pattern) {
                out->opts.pattern = option.token;
                continue;
            }
            if (!out->search_paths) {
                out->search_paths = calloc((size_t)argc - option.index, sizeof(*out->search_paths));
                if (!out->search_paths) {
                    fprintf(stderr, "%s: out of memory\n", out->progname);
                    out->exit_code = 1;
                    return false;
                }
            }
            out->search_paths[out->search_path_count++] = argv[option.index];
            continue;
        }
        if (option.event != BX_OPTION_MATCH) {
            fprintf(stderr, "%s: %s '%s'\n", out->progname, option.event == BX_OPTION_MISSING_VALUE ? "option requires a value:" : "unrecognized option", option.token);
            out->exit_code = 2;
            return false;
        }
        int id = option.spec->id;
        if (id == 'x' || id == 'X') {
            if (!fd_exact_exec_option(&option)) {
                fprintf(stderr, "%s: use -x, -X, --exec or --exec-batch as a standalone option\n", out->progname);
                out->exit_code = 2;
                return false;
            }
            out->opts.exec_mode = id == 'x' ? FD_EXEC_EACH : FD_EXEC_BATCH;
            if (!fd_prepare_exec_argv(out, argc, argv, (int)cursor.index, option.value))
                return false;
            break; /* Every remaining argument belongs to the child. */
        }
        switch (id) {
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
            case 'E': {
                if (out->opts.num_exclude_patterns == FD_MAX_EXCLUDE_PATTERNS) {
                    fprintf(stderr, "%s: too many --exclude patterns (limit %d)\n", out->progname, FD_MAX_EXCLUDE_PATTERNS);
                    out->exit_code = 2;
                    return false;
                }
                size_t length = strlen(option.value);
                char *pattern = malloc(length + 1u + BX_GLOB_CARET_SPARE);
                if (!pattern) {
                    fprintf(stderr, "%s: out of memory\n", out->progname);
                    out->exit_code = 2;
                    return false;
                }
                memcpy(pattern, option.value, length + 1u);
                bx_glob_normalize_caret(pattern);
                out->opts.exclude_patterns[out->opts.num_exclude_patterns++] = pattern;
                break;
            }
            case 204:
                out->opts.fixed_strings = false;
                out->opts.glob_match = false;
                break;
            case 'd':
                if (!fd_parse_nonnegative_int(out->progname, "--max-depth", option.value, &out->opts.max_depth)) {
                    out->exit_code = 2;
                    return false;
                }
                break;
            case 201:
                if (!fd_parse_nonnegative_int(out->progname, "--min-depth", option.value, &out->opts.min_depth)) {
                    out->exit_code = 2;
                    return false;
                }
                break;
            case 202:
                if (!fd_parse_nonnegative_int(out->progname, "--exact-depth", option.value, &out->opts.exact_depth)) {
                    out->exit_code = 2;
                    return false;
                }
                out->opts.max_depth = out->opts.exact_depth;
                break;
            case 't':
                if (!fd_parse_type_filter(out->progname, option.value, &out->opts.type_filter)) {
                    out->exit_code = 2;
                    return false;
                }
                break;
            case 'e':
                out->opts.extension = option.value;
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
            case 206:
                if (!fd_parse_nonnegative_int(out->progname, "--batch-size", option.value, &out->opts.batch_size)) {
                    out->exit_code = 2;
                    return false;
                }
                out->opts.batch_size_set = true;
                break;
            case 207:
                out->opts.path_separator = option.value;
                break;
            case 209:
                out->opts.show_errors = true;
                break;
            case 208:
                if (!fd_parse_strip_cwd_prefix(out->progname, option.value, &out->opts.strip_cwd_prefix)) {
                    out->exit_code = 2;
                    return false;
                }
                break;
            case 213:
                out->opts.output_format = option.value;
                break;
            case 203:
                if (out->opts.num_and_patterns == FD_MAX_AND_PATTERNS) {
                    fprintf(stderr, "%s: too many --and patterns (limit %d)\n", out->progname, FD_MAX_AND_PATTERNS);
                    out->exit_code = 2;
                    return false;
                }
                out->opts.and_patterns[out->opts.num_and_patterns++] = option.value;
                break;
            case 200:
                if (!fd_parse_nonnegative_int(out->progname, "--max-results", option.value, &out->opts.max_results)) {
                    out->exit_code = 2;
                    return false;
                }
                break;
        }
    }

    if (show_help) {
        fd_print_help(out->progname);
        out->exit_code = 0;
        return false;
    }

    if (!out->search_path_count)
        out->search_path_count = 1;

    if (out->opts.unrestrict_level >= 1) {
        out->opts.no_ignore = true;
        out->opts.hidden = true;
        out->opts.no_require_git = true;
    }

    if (!fd_validate_main_args(out))
        return false;

    struct bx_output_policy output_policy;
    bx_output_policy_init_stdout(&output_policy);
    if (out->opts.print0) {
        bx_output_policy_set_nul_terminated(&output_policy, true);
        bx_output_policy_allow_raw_exception(
            &output_policy,
            BX_OUTPUT_POLICY_RAW_MACHINE_DELIMITED_PATHS);
    }
    if (out->opts.output_format) {
        bx_output_policy_allow_raw_exception(&output_policy,
                                             BX_OUTPUT_POLICY_RAW_USER_TEMPLATE);
    }
    out->opts.terminal_quote_paths =
        out->opts.exec_mode == FD_EXEC_NONE &&
        !out->opts.output_format &&
        bx_output_policy_terminal_quote_paths(&output_policy);

    return true;
}

void fd_free_main_args(struct fd_main_args *args) {
    if (!args)
        return;
    while (args->opts.num_exclude_patterns > 0)
        free(args->opts.exclude_patterns[--args->opts.num_exclude_patterns]);
    free(args->search_paths);
    args->search_paths = NULL;
    free(args->exec_argv_storage);
    args->exec_argv_storage = NULL;
    args->opts.exec_argv = NULL;
    args->opts.exec_argc = 0;
}
