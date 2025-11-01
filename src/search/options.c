#include <ctype.h>
#include <getopt.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bx/diag.h"
#include "lib/cli_common.h"
#include "options.h"
#include "pcre2_matcher.h"
#include "search.h"

enum {
    OPT_HELP = 256,
    OPT_VERSION,
    OPT_INCLUDE,
    OPT_EXCLUDE,
    OPT_EXCLUDE_FROM,
    OPT_EXCLUDE_DIR,
    OPT_FILES,
    OPT_TYPE_LIST,
    OPT_COLOR,
    OPT_COLUMN,
    OPT_COUNT_MATCHES,
    OPT_PASSTHRU,
    OPT_REPLACE,
    OPT_STATS,
    OPT_FILES_WITH_MATCHES,
    OPT_FILES_WITHOUT_MATCH,
    OPT_FOLLOW,
    OPT_DIRECTORIES,
    OPT_MAX_DEPTH,
    OPT_MAX_COLUMNS,
    OPT_NO_IGNORE,
    OPT_NO_IGNORE_PARENT,
    OPT_NO_IGNORE_VCS,
    OPT_NO_IGNORE_DOT,
    OPT_NO_REQUIRE_GIT,
    OPT_HIDDEN,
    OPT_IGLOB,
    OPT_LABEL,
    OPT_GROUP_SEPARATOR,
    OPT_NO_GROUP_SEPARATOR,
    OPT_CONTEXT_SEPARATOR,
    OPT_NO_CONTEXT_SEPARATOR,
    OPT_FIELD_CONTEXT_SEPARATOR,
    OPT_FIELD_MATCH_SEPARATOR,
    OPT_HEADING,
    OPT_NO_HEADING,
    OPT_NULL,
    OPT_BINARY_FILES,
    OPT_NULL_DATA,
    OPT_MULTILINE,
    OPT_MULTILINE_DOTALL,
    OPT_STOP_ON_NONMATCH,
    OPT_ENGINE,
    OPT_PCRE2_VERSION,
    OPT_REGEX_SIZE_LIMIT,
    OPT_DFA_SIZE_LIMIT,
    OPT_JSON,
    OPT_DEBUG,
    OPT_SORT,
    OPT_SORTR,
    OPT_TYPE_ADD,
    OPT_TYPE_CLEAR,
};

static bool bx_parse_nonnegative_int(const char *progname, const char *optname,
                                     const char *text, int *out) {
    char *end = NULL;
    long v = strtol(text, &end, 10);
    if (!text || *text == '\0' || (end && *end != '\0') || v < 0 || v > 1<<20) {
        fprintf(stderr, "%s: invalid argument for %s: %s\n",
                progname, optname, text ? text : "(null)");
        return false;
    }
    *out = (int)v;
    return true;
}

static void bx_grep_print_usage_try_help(const char *progname) {
    fprintf(stderr, "Usage: %s [OPTION]... PATTERNS [FILE]...\n", progname);
    bx_cli_print_try_help(progname);
}

static int bx_grep_invalid_directories_mode(const char *progname, const char *value) {
    fprintf(stderr, "%s: invalid argument '%s' for '--directories'\n", progname, value);
    fputs("Valid arguments are:\n", stderr);
    fputs("  - 'read'\n", stderr);
    fputs("  - 'recurse'\n", stderr);
    fputs("  - 'skip'\n", stderr);
    bx_grep_print_usage_try_help(progname);
    return 3;
}

static int bx_grep_missing_option_argument(const char *progname, int missing_opt,
                                           int parse_optind, int argc, char **argv) {
    if (parse_optind > 0 && parse_optind <= argc && argv[parse_optind - 1] != NULL
        && strncmp(argv[parse_optind - 1], "--", 2) == 0) {
        fprintf(stderr, "%s: option '%s' requires an argument\n", progname, argv[parse_optind - 1]);
    } else if (missing_opt != 0) {
        fprintf(stderr, "%s: option requires an argument -- '%c'\n", progname, missing_opt);
    } else {
        fprintf(stderr, "%s: option requires an argument\n", progname);
    }
    bx_grep_print_usage_try_help(progname);
    return -1;
}

static bool bx_grep_unsupported_required_long_option(const char *arg) {
    static const char *const names[] = {
        "--glob",
        "--iglob",
        "--replace",
        "--type",
        "--type-not",
        "--type-add",
        "--type-clear",
        "--max-depth",
        "--max-columns",
        "--context-separator",
        "--field-context-separator",
        "--field-match-separator",
        "--engine",
        "--regex-size-limit",
        "--dfa-size-limit",
        "--sort",
        "--sortr",
    };

    if (!arg)
        return false;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strcmp(arg, names[i]) == 0)
            return true;
    }
    return false;
}

static int bx_grep_unrecognized_option(const char *progname, const char *arg) {
    if (arg)
        fprintf(stderr, "%s: unrecognized option '%s'\n", progname, arg);
    else
        fprintf(stderr, "%s: unrecognized option\n", progname);
    bx_grep_print_usage_try_help(progname);
    return -1;
}

static const char *bx_search_current_option_token(int optind, int argc, char **argv,
                                                  const char *fallback) {
    if (optind > 0 && optind <= argc && argv[optind - 1] != NULL)
        return argv[optind - 1];
    return fallback;
}

static bool bx_search_parse_nonnegative_int(const char *progname,
                                            enum bx_search_personality personality,
                                            const char *optname,
                                            const char *text,
                                            int *out) {
    if (personality == BX_SEARCH_GREP && strcmp(optname, "-m") == 0) {
        char *end = NULL;
        long v = strtol(text, &end, 10);
        if (!text || *text == '\0' || (end && *end != '\0') || v < 0 || v > 1<<20) {
            fprintf(stderr, "%s: invalid max count\n", progname);
            return false;
        }
        *out = (int)v;
        return true;
    }

    return bx_parse_nonnegative_int(progname, optname, text, out);
}

static bool bx_rg_size_limit_parse_failed(const char *progname, const char *optname,
                                          const char *text) {
    fprintf(stderr,
            "%s: error parsing flag %s: invalid size: invalid format for size '%s', which should be a non-empty sequence of digits followed by an optional 'K', 'M' or 'G' suffix\n",
            progname, optname, text ? text : "");
    return false;
}

static bool bx_parse_rg_size_limit(const char *progname, const char *optname,
                                   const char *text, size_t *out) {
    if (!text || !*text)
        return bx_rg_size_limit_parse_failed(progname, optname, text);

    unsigned long long value = 0;
    size_t pos = 0;
    while (text[pos] >= '0' && text[pos] <= '9') {
        unsigned int digit = (unsigned int)(text[pos] - '0');
        if (value > (ULLONG_MAX - digit) / 10ULL)
            return bx_rg_size_limit_parse_failed(progname, optname, text);
        value = value * 10ULL + digit;
        pos++;
    }

    if (pos == 0)
        return bx_rg_size_limit_parse_failed(progname, optname, text);

    unsigned long long multiplier = 1;
    if (text[pos] != '\0') {
        if (text[pos + 1] != '\0')
            return bx_rg_size_limit_parse_failed(progname, optname, text);
        switch (text[pos]) {
        case 'K':
            multiplier = 1024ULL;
            break;
        case 'M':
            multiplier = 1024ULL * 1024ULL;
            break;
        case 'G':
            multiplier = 1024ULL * 1024ULL * 1024ULL;
            break;
        default:
            return bx_rg_size_limit_parse_failed(progname, optname, text);
        }
    }

    if (value > (unsigned long long)SIZE_MAX / multiplier)
        return bx_rg_size_limit_parse_failed(progname, optname, text);

    *out = (size_t)(value * multiplier);
    return true;
}

static bool bx_set_binary_files_mode(const char *progname,
                                     enum bx_search_personality personality,
                                     struct search_opts *opts,
                                     const char *value) {
    if (strcmp(value, "binary") == 0) {
        opts->binary_as_text = false;
        opts->binary_without_match = false;
        return true;
    }
    if (strcmp(value, "text") == 0) {
        opts->binary_as_text = true;
        opts->binary_without_match = false;
        return true;
    }
    if (strcmp(value, "without-match") == 0) {
        opts->binary_as_text = false;
        opts->binary_without_match = true;
        return true;
    }

    if (personality == BX_SEARCH_GREP || personality == BX_SEARCH_EGREP
        || personality == BX_SEARCH_FGREP) {
        fprintf(stderr, "%s: unknown binary-files type\n", progname);
    } else {
        fprintf(stderr, "%s: invalid argument for --binary-files: %s\n",
                progname, value);
    }
    return false;
}

void bx_search_print_help(const char *progname) {
    printf("Usage: %s [OPTION]... PATTERN [FILE]...\n", progname);
    puts("Search for PATTERN in each FILE.");
    puts("");
    puts("  -E            PATTERN is an extended regular expression");
    puts("  -F            PATTERN is a set of fixed strings");
    puts("  -b            print the byte offset with output lines");
    puts("      --column  print the column number with output lines");
    puts("  -H            print the file name for each match");
    puts("  -h            suppress the file name prefix on output");
    puts("  -i            ignore case distinctions");
    puts("  -n            print line number with output lines");
    puts("  -o            show only the part of a line matching PATTERN");
    puts("  -v            select non-matching lines");
    puts("  -c            print only a count of matching lines per FILE");
    puts("      --count-matches  print only a count of individual matches per FILE");
    puts("      --passthru  print both matching and non-matching lines");
    puts("      --replace=TEXT  replace each match with TEXT in printed output");
    puts("      --stats  print a search summary after all results");
    puts("  -l            print only names of FILEs with selected lines");
    puts("  -L            print only names of FILEs with no selected lines");
    puts("  -q, --quiet   suppress all normal output");
    puts("  -r            recursive, do not follow symlinks");
    puts("  -R            recursive, follow symlinks");
    puts("  -a            process binary files as text");
    puts("  -I            skip binary files");
    puts("  -A NUM        print NUM lines of trailing context");
    puts("  -B NUM        print NUM lines of leading context");
    puts("  -C NUM        print NUM lines of output context");
    puts("  -Z, --null    print NUL after file names");
    puts("  -z            use NUL as the record separator");
    puts("      --label=LABEL  use LABEL as the standard input file name");
    puts("      --group-separator=SEP  use SEP between context groups");
    puts("      --no-group-separator   suppress context group separators");
    puts("      --context-separator=SEP  use SEP between ripgrep context groups");
    puts("      --no-context-separator  suppress ripgrep context group separators");
    puts("      --field-context-separator=SEP  use SEP between fields on context lines");
    puts("      --field-match-separator=SEP  use SEP between fields on matching lines");
    puts("      --heading  show file names above matches instead of as prefixes");
    puts("      --max-columns=NUM  omit long matching lines wider than NUM bytes");
    puts("      --no-heading  show file names as prefixes");
    puts("      --include=GLOB   search only files matching GLOB");
    puts("      --exclude=GLOB   skip files matching GLOB");
    puts("      --exclude-from=FILE  skip files matching patterns from FILE");
    puts("      --exclude-dir=GLOB  skip directories matching GLOB");
    puts("      --no-ignore  do not use ignore files");
    puts("      --no-ignore-parent  do not use ignore files from parent directories");
    puts("      --no-ignore-vcs  do not use VCS ignore files");
    puts("      --no-ignore-dot  do not use .ignore or .rgignore files");
    puts("      --no-require-git  use .gitignore outside git repositories");
    puts("      --hidden  search hidden files and directories");
    puts("      --iglob=GLOB  search only files matching GLOB, case-insensitively");
    puts("  -U, --multiline  allow matches to span line terminators");
    puts("      --multiline-dotall  make . match line terminators in multiline mode");
    puts("      --engine=ENGINE  choose regex engine: default, pcre2, or auto");
    puts("      --regex-size-limit=NUM[KMG]  accept ripgrep's regex size limit flag");
    puts("      --dfa-size-limit=NUM[KMG]  accept ripgrep's DFA size limit flag");
    puts("      --pcre2-version  print PCRE2 version information and exit");
    puts("      --stop-on-nonmatch  stop reading a file after a non-matching record follows a match");
    puts("      --sort=TYPE  sort results ascending by TYPE; path is supported");
    puts("      --sortr=TYPE  sort results descending by TYPE; path is supported");
    puts("      --help    display this help and exit");
    puts("      --version output version information and exit");
}

void bx_search_print_version(const char *progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

void bx_search_print_type_list(void) {
    puts("c:    *.c, *.h");
    puts("cpp:  *.cpp, *.cc, *.cxx, *.c++, *.hh, *.hpp, *.hxx, *.h++");
    puts("rs:   *.rs");
    puts("py:   *.py");
    puts("js:   *.js, *.jsx");
    puts("ts:   *.ts, *.tsx");
    puts("go:   *.go");
    puts("java: *.java");
    puts("rb:   *.rb");
    puts("sh:   *.sh, *.bash");
    puts("md:   *.md, *.markdown");
    puts("txt:  *.txt");
    puts("toml: *.toml");
    puts("json: *.json");
    puts("yaml: *.yml, *.yaml");
    puts("xml:  *.xml");
    puts("html: *.html, *.htm");
    puts("css:  *.css");
    puts("lock: *.lock");
}

static const char *bx_get_builtin_type_globs(const char *name) {
    static const struct { const char *name; const char *globs; } types[] = {
        {"c",    "*.c,*.h"},
        {"cpp",  "*.cpp,*.cc,*.cxx,*.c++,*.hh,*.hpp,*.hxx,*.h++"},
        {"rs",   "*.rs"},
        {"py",   "*.py"},
        {"js",   "*.js,*.jsx"},
        {"ts",   "*.ts,*.tsx"},
        {"go",   "*.go"},
        {"java", "*.java"},
        {"rb",   "*.rb"},
        {"sh",   "*.sh,*.bash"},
        {"md",   "*.md,*.markdown"},
        {"txt",  "*.txt"},
        {"toml", "*.toml"},
        {"json", "*.json"},
        {"yaml", "*.yml,*.yaml"},
        {"xml",  "*.xml"},
        {"html", "*.html,*.htm"},
        {"css",  "*.css"},
        {"lock", "*.lock"},
        {NULL, NULL},
    };
    for (int i = 0; types[i].name; i++)
        if (strcmp(types[i].name, name) == 0)
            return types[i].globs;
    return NULL;
}

static const char *bx_get_type_globs(const struct search_opts *opts, const char *name) {
    if (opts && name) {
        for (int i = 0; i < opts->num_cleared_types; i++) {
            if (opts->cleared_type_names[i] && strcmp(opts->cleared_type_names[i], name) == 0)
                return NULL;
        }
    }
    if (opts && name) {
        for (int i = 0; i < opts->num_custom_types; i++) {
            if (opts->custom_type_names[i] && strcmp(opts->custom_type_names[i], name) == 0)
                return opts->custom_type_globs[i];
        }
    }
    return bx_get_builtin_type_globs(name);
}

static bool bx_add_custom_type(struct search_opts *opts, const char *text) {
    if (!opts || !text)
        return false;

    const char *colon = strchr(text, ':');
    if (!colon || colon == text || colon[1] == '\0')
        return false;

    size_t name_len = (size_t)(colon - text);
    char *name = strndup(text, name_len);
    char *globs = strdup(colon + 1);
    if (!name || !globs) {
        free(name);
        free(globs);
        return false;
    }

    for (int i = 0; i < opts->num_custom_types; i++) {
        if (strcmp(opts->custom_type_names[i], name) == 0) {
            size_t merged_len = strlen(opts->custom_type_globs[i]) + 1 + strlen(globs) + 1;
            char *merged = malloc(merged_len);
            if (!merged) {
                free(name);
                free(globs);
                return false;
            }
            snprintf(merged, merged_len, "%s,%s", opts->custom_type_globs[i], globs);
            free(opts->custom_type_globs[i]);
            opts->custom_type_globs[i] = merged;
            free(name);
            free(globs);
            return true;
        }
    }

    if (opts->num_custom_types >= MAX_CUSTOM_TYPES) {
        free(name);
        free(globs);
        return false;
    }

    opts->custom_type_names[opts->num_custom_types] = name;
    opts->custom_type_globs[opts->num_custom_types] = globs;
    opts->num_custom_types++;
    return true;
}

static bool bx_clear_type(struct search_opts *opts, const char *name) {
    if (!opts || !name || *name == '\0')
        return false;

    for (int i = 0; i < opts->num_custom_types; i++) {
        if (opts->custom_type_names[i] && strcmp(opts->custom_type_names[i], name) == 0) {
            free(opts->custom_type_names[i]);
            free(opts->custom_type_globs[i]);
            for (int j = i + 1; j < opts->num_custom_types; j++) {
                opts->custom_type_names[j - 1] = opts->custom_type_names[j];
                opts->custom_type_globs[j - 1] = opts->custom_type_globs[j];
            }
            opts->num_custom_types--;
            opts->custom_type_names[opts->num_custom_types] = NULL;
            opts->custom_type_globs[opts->num_custom_types] = NULL;
            break;
        }
    }

    for (int i = 0; i < opts->num_cleared_types; i++) {
        if (opts->cleared_type_names[i] && strcmp(opts->cleared_type_names[i], name) == 0)
            return true;
    }

    if (opts->num_cleared_types >= MAX_CLEARED_TYPES)
        return false;

    opts->cleared_type_names[opts->num_cleared_types] = strdup(name);
    if (!opts->cleared_type_names[opts->num_cleared_types])
        return false;
    opts->num_cleared_types++;
    return true;
}

void bx_search_free_options(struct search_opts *opts) {
    for (int i = 0; i < opts->num_include; i++) free(opts->include_patterns[i]);
    for (int i = 0; i < opts->num_exclude; i++) free(opts->exclude_patterns[i]);
    for (int i = 0; i < opts->num_exclude_dir; i++) free(opts->exclude_dir_patterns[i]);
    for (int i = 0; i < opts->num_extra_patterns; i++) free(opts->extra_patterns[i]);
    for (int i = 0; i < opts->num_custom_types; i++) {
        free(opts->custom_type_names[i]);
        free(opts->custom_type_globs[i]);
    }
    for (int i = 0; i < opts->num_cleared_types; i++)
        free(opts->cleared_type_names[i]);
    free(opts->label);
    free(opts->group_separator);
    free(opts->replace);
    free(opts->field_match_separator);
    free(opts->field_context_separator);
}

int bx_search_parse_options(int argc, char **argv, struct search_opts *opts,
                             enum bx_search_personality personality,
                             const char **pattern, int *first_file) {
    memset(opts, 0, sizeof(*opts));
    const char *progname = bx_cli_progname(argv[0], "grep");

    if (personality == BX_SEARCH_EGREP) opts->extended_regex = true;
    if (personality == BX_SEARCH_FGREP) opts->fixed_strings = true;
    opts->max_depth = -1;
    if (personality == BX_SEARCH_RG) {
        opts->recursive = true;
        opts->follow_symlinks = false;
        opts->binary_without_match = true;
        opts->hidden = false;
        opts->no_ignore = false;
        opts->smart_case = false;
    } else {
        opts->hidden = true;
        opts->no_ignore = true;
    }

    static struct option long_opts[] = {
        {"help",         no_argument,       NULL, OPT_HELP},
        {"version",      no_argument,       NULL, OPT_VERSION},
        {"quiet",        no_argument,       NULL, 'q'},
        {"regexp",       required_argument, NULL, 'e'},
        {"include",      required_argument, NULL, OPT_INCLUDE},
        {"exclude",      required_argument, NULL, OPT_EXCLUDE},
        {"exclude-from", required_argument, NULL, OPT_EXCLUDE_FROM},
        {"exclude-dir",  required_argument, NULL, OPT_EXCLUDE_DIR},
        {"files",        no_argument,       NULL, OPT_FILES},
        {"column",       no_argument,       NULL, OPT_COLUMN},
        {"count-matches", no_argument,      NULL, OPT_COUNT_MATCHES},
        {"passthru",     no_argument,       NULL, OPT_PASSTHRU},
        {"replace",      required_argument, NULL, OPT_REPLACE},
        {"stats",        no_argument,       NULL, OPT_STATS},
        {"files-with-matches", no_argument, NULL, OPT_FILES_WITH_MATCHES},
        {"files-without-match", no_argument, NULL, OPT_FILES_WITHOUT_MATCH},
        {"directories", required_argument, NULL, OPT_DIRECTORIES},
        {"follow",       no_argument,       NULL, OPT_FOLLOW},
        {"after-context", required_argument, NULL, 'A'},
        {"before-context", required_argument, NULL, 'B'},
        {"context",      required_argument, NULL, 'C'},
        {"no-ignore", no_argument,          NULL, OPT_NO_IGNORE},
        {"no-ignore-parent", no_argument,   NULL, OPT_NO_IGNORE_PARENT},
        {"no-ignore-vcs", no_argument,      NULL, OPT_NO_IGNORE_VCS},
        {"no-ignore-dot", no_argument,      NULL, OPT_NO_IGNORE_DOT},
        {"no-require-git", no_argument,     NULL, OPT_NO_REQUIRE_GIT},
        {"hidden",       no_argument,       NULL, OPT_HIDDEN},
        {"byte-offset",  no_argument,       NULL, 'b'},
        {"glob",         required_argument, NULL, 'g'},
        {"iglob",        required_argument, NULL, OPT_IGLOB},
        {"ignore-case",  no_argument,       NULL, 'i'},
        {"case-sensitive", no_argument,     NULL, 's'},
        {"smart-case",   no_argument,       NULL, 'S'},
        {"type",         required_argument, NULL, 't'},
        {"type-not",     required_argument, NULL, 'T'},
        {"type-add",     required_argument, NULL, OPT_TYPE_ADD},
        {"type-clear",   required_argument, NULL, OPT_TYPE_CLEAR},
        {"type-list",    no_argument,       NULL, OPT_TYPE_LIST},
        {"file",         required_argument, NULL, 'f'},
        {"max-count",    required_argument, NULL, 'm'},
        {"max-depth",    required_argument, NULL, OPT_MAX_DEPTH},
        {"max-columns",  required_argument, NULL, OPT_MAX_COLUMNS},
        {"color",        optional_argument, NULL, OPT_COLOR},
        {"colour",       optional_argument, NULL, OPT_COLOR},
        {"label",        required_argument, NULL, OPT_LABEL},
        {"group-separator", required_argument, NULL, OPT_GROUP_SEPARATOR},
        {"no-group-separator", no_argument, NULL, OPT_NO_GROUP_SEPARATOR},
        {"context-separator", required_argument, NULL, OPT_CONTEXT_SEPARATOR},
        {"no-context-separator", no_argument, NULL, OPT_NO_CONTEXT_SEPARATOR},
        {"field-context-separator", required_argument, NULL, OPT_FIELD_CONTEXT_SEPARATOR},
        {"field-match-separator", required_argument, NULL, OPT_FIELD_MATCH_SEPARATOR},
        {"heading",      no_argument,       NULL, OPT_HEADING},
        {"no-heading",   no_argument,       NULL, OPT_NO_HEADING},
        {"null",         no_argument,       NULL, OPT_NULL},
        {"binary-files", required_argument, NULL, OPT_BINARY_FILES},
        {"null-data",    no_argument,       NULL, OPT_NULL_DATA},
        {"multiline",    no_argument,       NULL, OPT_MULTILINE},
        {"multiline-dotall", no_argument,   NULL, OPT_MULTILINE_DOTALL},
        {"stop-on-nonmatch", no_argument,   NULL, OPT_STOP_ON_NONMATCH},
        {"json",         no_argument,       NULL, OPT_JSON},
        {"debug",        no_argument,       NULL, OPT_DEBUG},
        {"sort",         required_argument, NULL, OPT_SORT},
        {"sortr",        required_argument, NULL, OPT_SORTR},
        {"engine",       required_argument, NULL, OPT_ENGINE},
        {"regex-size-limit", required_argument, NULL, OPT_REGEX_SIZE_LIMIT},
        {"dfa-size-limit", required_argument, NULL, OPT_DFA_SIZE_LIMIT},
        {"pcre2-version", no_argument,      NULL, OPT_PCRE2_VERSION},
        {NULL, 0, NULL, 0},
    };

    opterr = 0;
    optind = 1;

    int c;
    while ((c = getopt_long(argc, argv, ":0EFHbhinovclLqrRIszZd:aA:B:C:e:f:g:j:t:T:uwPxSm:U", long_opts, NULL)) != -1) {
        switch (c) {
        case ':':
            if (personality == BX_SEARCH_GREP || personality == BX_SEARCH_EGREP
                || personality == BX_SEARCH_FGREP) {
                if (optind > 0 && optind <= argc
                    && bx_grep_unsupported_required_long_option(argv[optind - 1])) {
                    return bx_grep_unrecognized_option(progname, argv[optind - 1]);
                }
                return bx_grep_missing_option_argument(progname, optopt, optind, argc, argv);
            }
            if (optopt != 0) {
                fprintf(stderr, "%s: option requires an argument -- '%c'\n", progname, optopt);
            } else if (optind > 0 && optind <= argc && argv[optind - 1] != NULL) {
                fprintf(stderr, "%s: option requires an argument -- '%s'\n", progname, argv[optind - 1]);
            } else {
                fprintf(stderr, "%s: option requires an argument\n", progname);
            }
            return -1;
        case '0':
            if (personality == BX_SEARCH_RG) {
                opts->null_output = true;
            } else {
                fprintf(stderr, "%s: unrecognized option '-0'\n", progname);
                return -1;
            }
            break;
        case 'E': opts->extended_regex = true; break;
        case 'F': opts->fixed_strings = true; break;
        case 'b': opts->show_byte_offset = true; break;
        case OPT_COLUMN:
            if (personality != BX_SEARCH_RG) {
                fprintf(stderr, "%s: unrecognized option '--column'\n", progname);
                return -1;
            }
            opts->show_column = true;
            break;
        case 'H': opts->show_filename = true; break;
        case 'h': opts->hide_filename = true; break;
        case 'i':
            opts->ignore_case = true;
            if (personality == BX_SEARCH_RG)
                opts->smart_case = false;
            break;
        case 'n': opts->show_line_number = true; break;
        case 'o': opts->only_matching = true; break;
        case 'v': opts->invert_match = true; break;
        case 'c': opts->count_only = true; break;
        case OPT_COUNT_MATCHES:
            if (personality != BX_SEARCH_RG) {
                return bx_grep_unrecognized_option(progname, "--count-matches");
            }
            opts->count_matches = true;
            opts->count_only = true;
            break;
        case OPT_PASSTHRU:
            if (personality != BX_SEARCH_RG) {
                return bx_grep_unrecognized_option(progname, "--passthru");
            }
            opts->passthru = true;
            break;
        case OPT_REPLACE:
            if (personality != BX_SEARCH_RG) {
                return bx_grep_unrecognized_option(
                    progname,
                    bx_search_current_option_token(optind, argc, argv, "--replace"));
            }
            free(opts->replace);
            opts->replace = strdup(optarg);
            if (!opts->replace)
                return -1;
            break;
        case OPT_STATS:
            if (personality != BX_SEARCH_RG) {
                return bx_grep_unrecognized_option(progname, "--stats");
            }
            opts->stats = true;
            break;
        case 'l': opts->files_with_matches = true; break;
        case 'L':
            if (personality == BX_SEARCH_RG)
                opts->follow_symlinks = true;
            else
                opts->files_without_match = true;
            break;
        case 'q': opts->quiet = true; break;
        case 'r': opts->recursive = true; opts->follow_symlinks = false; break;
        case 'R': opts->recursive = true; opts->follow_symlinks = true; break;
        case 'd':
        case OPT_DIRECTORIES:
            if (personality == BX_SEARCH_RG) {
                if (!bx_parse_nonnegative_int(progname, "-d", optarg, &opts->max_depth))
                    return -1;
            } else {
                if (strcmp(optarg, "read") == 0) {
                    opts->directory_mode = BX_GREP_DIR_READ;
                } else if (strcmp(optarg, "recurse") == 0) {
                    opts->directory_mode = BX_GREP_DIR_RECURSE;
                    opts->recursive = true;
                    opts->follow_symlinks = false;
                } else if (strcmp(optarg, "skip") == 0) {
                    opts->directory_mode = BX_GREP_DIR_SKIP;
                } else {
                    if (personality == BX_SEARCH_GREP || personality == BX_SEARCH_EGREP
                        || personality == BX_SEARCH_FGREP) {
                        return bx_grep_invalid_directories_mode(progname, optarg);
                    }
                    fprintf(stderr, "%s: invalid argument for -d: %s\n", progname, optarg);
                    return -1;
                }
            }
            break;
        case OPT_MAX_COLUMNS:
            if (personality != BX_SEARCH_RG) {
                fprintf(stderr, "%s: unrecognized option '--max-columns'\n", progname);
                return -1;
            }
            if (!bx_parse_nonnegative_int(progname, "--max-columns", optarg, &opts->max_columns))
                return -1;
            break;
        case 'I':
            opts->binary_without_match = true;
            opts->binary_as_text = false;
            break;
        case 'a':
            opts->binary_as_text = true;
            opts->binary_without_match = false;
            break;
        case 'w': opts->word_regexp = true; break;
        case 'x': opts->line_regexp = true; break;
        case 'P': opts->perl_regexp = true; break;
        case 's':
            if (personality == BX_SEARCH_RG) {
                opts->smart_case = false;
                opts->ignore_case = false;
            } else {
                fprintf(stderr, "%s: invalid option -- 's'\n", progname);
                return -1;
            }
            break;
        case 'S':
            if (personality == BX_SEARCH_RG) {
                opts->smart_case = true;
                opts->ignore_case = false;
            } else {
                fprintf(stderr, "%s: invalid option -- 'S'\n", progname);
                return -1;
            }
            break;
        case 'm':
            if (!bx_search_parse_nonnegative_int(progname, personality, "-m", optarg, &opts->max_count))
                return -1;
            break;
        case 'Z':
            if (personality == BX_SEARCH_RG) {
                fprintf(stderr, "%s: invalid option -- 'Z'\n", progname);
                return -1;
            }
            opts->null_output = true;
            opts->null_filename = true;
            break;
        case 'z':
            if (personality == BX_SEARCH_RG) {
                fprintf(stderr, "%s: invalid option -- 'z'\n", progname);
                return -1;
            }
            opts->null_data = true;
            break;
        case 'g':
            if (personality == BX_SEARCH_RG && optarg && optarg[0] == '!') {
                if (opts->num_exclude < MAX_EXCLUDE_PATTERNS)
                    opts->exclude_patterns[opts->num_exclude++] = strdup(optarg + 1);
            } else if (opts->num_include < MAX_INCLUDE_PATTERNS) {
                opts->include_patterns[opts->num_include++] = strdup(optarg);
            }
            break;
        case OPT_IGLOB:
            if (personality != BX_SEARCH_RG) {
                return bx_grep_unrecognized_option(
                    progname,
                    bx_search_current_option_token(optind, argc, argv, "--iglob"));
            }
            if (optarg && optarg[0] == '!') {
                fprintf(stderr, "%s: unsupported option argument for --iglob: %s\n", progname, optarg);
                return -1;
            }
            if (opts->num_include < MAX_INCLUDE_PATTERNS) {
                opts->include_patterns[opts->num_include] = strdup(optarg);
                opts->include_pattern_casefold[opts->num_include] = true;
                opts->num_include++;
            }
            break;
        case 'u':
            if (opts->unrestrict_level < 3) opts->unrestrict_level++;
            break;
        case 'U':
            if (personality != BX_SEARCH_RG) {
                fprintf(stderr, "%s: invalid option -- 'U'\n", progname);
                return -1;
            }
            opts->multiline = true;
            break;
        case OPT_HIDDEN:
            if (personality == BX_SEARCH_RG) {
                opts->hidden = true;
            } else {
                return bx_grep_unrecognized_option(progname, "--hidden");
            }
            break;
        case 'j':
            break;  /* thread count accepted, single-threaded for now */
        case 't':
        case 'T': {
            if (personality != BX_SEARCH_RG) {
                const char *arg = bx_search_current_option_token(optind, argc, argv, NULL);
                if (arg && (strcmp(arg, "--type") == 0 || strcmp(arg, "--type-not") == 0
                            || strncmp(arg, "--type=", 7) == 0
                            || strncmp(arg, "--type-not=", 11) == 0)) {
                    return bx_grep_unrecognized_option(progname, arg);
                }
                if (c == 't') {
                    fprintf(stderr, "%s: invalid option -- 't'\n", progname);
                    bx_grep_print_usage_try_help(progname);
                    return -1;
                }
            }
            const char *globs = bx_get_type_globs(opts, optarg);
            if (!globs) {
                if (personality == BX_SEARCH_RG) {
                    fprintf(stderr, "%s: unrecognized file type: %s\n", progname, optarg);
                    return -1;
                }
                break;
            }
            char *copy = strdup(globs);
            char *tok = strtok(copy, ",");
            while (tok) {
                while (*tok == ' ') tok++;
                char *end = tok + strlen(tok) - 1;
                while (end > tok && *end == ' ') *end-- = '\0';
                if (c == 't' && opts->num_include < MAX_INCLUDE_PATTERNS)
                    opts->include_patterns[opts->num_include++] = strdup(tok);
                else if (c == 'T' && opts->num_exclude < MAX_EXCLUDE_PATTERNS)
                    opts->exclude_patterns[opts->num_exclude++] = strdup(tok);
                tok = strtok(NULL, ",");
            }
            free(copy);
            break;
        }
        case OPT_TYPE_ADD:
            if (personality != BX_SEARCH_RG) {
                return bx_grep_unrecognized_option(
                    progname,
                    bx_search_current_option_token(optind, argc, argv, "--type-add"));
            }
            if (!bx_add_custom_type(opts, optarg)) {
                fprintf(stderr, "%s: invalid argument for --type-add: %s\n", progname, optarg);
                return -1;
            }
            break;
        case OPT_TYPE_CLEAR:
            if (personality != BX_SEARCH_RG) {
                return bx_grep_unrecognized_option(
                    progname,
                    bx_search_current_option_token(optind, argc, argv, "--type-clear"));
            }
            if (!bx_clear_type(opts, optarg)) {
                fprintf(stderr, "%s: invalid argument for --type-clear: %s\n", progname, optarg);
                return -1;
            }
            break;
        case 'e':
            if (opts->num_extra_patterns < 16)
                opts->extra_patterns[opts->num_extra_patterns++] = strdup(optarg);
            break;
        case 'f': {
            FILE *pf = NULL;
            bool close_pf = false;
            if (strcmp(optarg, "-") == 0) {
                pf = stdin;
            } else {
                pf = fopen(optarg, "r");
                close_pf = true;
            }
            if (!pf) {
                fprintf(stderr, "%s: %s: %s\n", progname, optarg, strerror(errno));
                return -1;
            }
            char *line = NULL; size_t cap = 0;
            while (getline(&line, &cap, pf) != -1) {
                size_t llen = strlen(line);
                while (llen > 0 && (line[llen-1] == '\n' || line[llen-1] == '\r'))
                    line[--llen] = '\0';
                if (opts->num_extra_patterns < 16)
                    opts->extra_patterns[opts->num_extra_patterns++] = strdup(line);
            }
            free(line);
            if (close_pf)
                fclose(pf);
            break;
        }
        case 'A':
            if (!bx_parse_nonnegative_int(progname, "-A", optarg, &opts->after_context))
                return -1;
            break;
        case 'B':
            if (!bx_parse_nonnegative_int(progname, "-B", optarg, &opts->before_context))
                return -1;
            break;
        case 'C': {
            int n;
            if (!bx_parse_nonnegative_int(progname, "-C", optarg, &n))
                return -1;
            opts->after_context = n;
            opts->before_context = n;
            break;
        }
        case OPT_INCLUDE:
            if (opts->num_include < MAX_INCLUDE_PATTERNS)
                opts->include_patterns[opts->num_include++] = strdup(optarg);
            break;
        case OPT_EXCLUDE:
            if (opts->num_exclude < MAX_EXCLUDE_PATTERNS)
                opts->exclude_patterns[opts->num_exclude++] = strdup(optarg);
            break;
        case OPT_EXCLUDE_FROM: {
            FILE *ef = fopen(optarg, "r");
            if (!ef) {
                fprintf(stderr, "%s: %s: %s\n", progname, optarg, strerror(errno));
                return -1;
            }
            char *line = NULL;
            size_t cap = 0;
            while (getline(&line, &cap, ef) != -1) {
                size_t len = strlen(line);
                while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
                    line[--len] = '\0';
                if (len == 0)
                    continue;
                if (opts->num_exclude < MAX_EXCLUDE_PATTERNS)
                    opts->exclude_patterns[opts->num_exclude++] = strdup(line);
            }
            free(line);
            fclose(ef);
            break;
        }
        case OPT_EXCLUDE_DIR:
            if (opts->num_exclude_dir < MAX_EXCLUDE_DIR_PATTERNS)
                opts->exclude_dir_patterns[opts->num_exclude_dir++] = strdup(optarg);
            break;
        case OPT_FILES:
            opts->files_only = true;
            break;
        case OPT_FILES_WITH_MATCHES:
            opts->files_with_matches = true;
            break;
        case OPT_FILES_WITHOUT_MATCH:
            opts->files_without_match = true;
            break;
        case OPT_FOLLOW:
            if (personality == BX_SEARCH_RG) {
                opts->follow_symlinks = true;
            } else {
                return bx_grep_unrecognized_option(progname, "--follow");
            }
            break;
        case OPT_NO_IGNORE:
            if (personality == BX_SEARCH_RG) {
                opts->no_ignore = true;
            } else {
                bx_grep_print_usage_try_help(progname);
                return -1;
            }
            break;
        case OPT_NO_IGNORE_PARENT:
            if (personality == BX_SEARCH_RG) {
                opts->no_ignore_parent = true;
            } else {
                return bx_grep_unrecognized_option(progname, "--no-ignore-parent");
            }
            break;
        case OPT_NO_IGNORE_VCS:
            if (personality == BX_SEARCH_RG) {
                opts->no_ignore_vcs = true;
            } else {
                return bx_grep_unrecognized_option(progname, "--no-ignore-vcs");
            }
            break;
        case OPT_NO_IGNORE_DOT:
            if (personality == BX_SEARCH_RG) {
                opts->no_ignore_dot = true;
            } else {
                return bx_grep_unrecognized_option(progname, "--no-ignore-dot");
            }
            break;
        case OPT_NO_REQUIRE_GIT:
            if (personality == BX_SEARCH_RG) {
                opts->no_require_git = true;
            } else {
                return bx_grep_unrecognized_option(progname, "--no-require-git");
            }
            break;
        case OPT_TYPE_LIST:
            bx_search_print_type_list();
            return 1;
        case OPT_MAX_DEPTH:
            if (personality == BX_SEARCH_RG) {
                if (!bx_parse_nonnegative_int(progname, "--max-depth", optarg, &opts->max_depth))
                    return -1;
            } else {
                return bx_grep_unrecognized_option(
                    progname,
                    bx_search_current_option_token(optind, argc, argv, "--max-depth"));
            }
            break;
        case OPT_LABEL:
            if (personality == BX_SEARCH_RG) {
                fprintf(stderr, "%s: unrecognized option '--label'\n", progname);
                return -1;
            }
            free(opts->label);
            opts->label = strdup(optarg);
            break;
        case OPT_GROUP_SEPARATOR:
            if (personality == BX_SEARCH_RG) {
                fprintf(stderr, "%s: unrecognized option '--group-separator'\n", progname);
                return -1;
            }
            free(opts->group_separator);
            opts->group_separator = strdup(optarg);
            opts->suppress_group_separator = false;
            break;
        case OPT_NO_GROUP_SEPARATOR:
            if (personality == BX_SEARCH_RG) {
                fprintf(stderr, "%s: unrecognized option '--no-group-separator'\n", progname);
                return -1;
            }
            opts->suppress_group_separator = true;
            break;
        case OPT_CONTEXT_SEPARATOR:
            if (personality != BX_SEARCH_RG) {
                return bx_grep_unrecognized_option(
                    progname,
                    bx_search_current_option_token(optind, argc, argv, "--context-separator"));
            }
            free(opts->group_separator);
            opts->group_separator = strdup(optarg);
            opts->suppress_group_separator = false;
            break;
        case OPT_NO_CONTEXT_SEPARATOR:
            if (personality != BX_SEARCH_RG) {
                fprintf(stderr, "%s: unrecognized option '--no-context-separator'\n", progname);
                return -1;
            }
            opts->suppress_group_separator = true;
            break;
        case OPT_FIELD_CONTEXT_SEPARATOR:
            if (personality != BX_SEARCH_RG) {
                return bx_grep_unrecognized_option(
                    progname,
                    bx_search_current_option_token(optind, argc, argv, "--field-context-separator"));
            }
            free(opts->field_context_separator);
            opts->field_context_separator = strdup(optarg);
            break;
        case OPT_FIELD_MATCH_SEPARATOR:
            if (personality != BX_SEARCH_RG) {
                return bx_grep_unrecognized_option(
                    progname,
                    bx_search_current_option_token(optind, argc, argv, "--field-match-separator"));
            }
            free(opts->field_match_separator);
            opts->field_match_separator = strdup(optarg);
            break;
        case OPT_HEADING:
            if (personality != BX_SEARCH_RG) {
                return bx_grep_unrecognized_option(progname, "--heading");
            }
            opts->heading = true;
            opts->heading_set = true;
            break;
        case OPT_NO_HEADING:
            if (personality != BX_SEARCH_RG) {
                return bx_grep_unrecognized_option(progname, "--no-heading");
            }
            opts->heading = false;
            opts->heading_set = true;
            break;
        case OPT_NULL:
            if (personality == BX_SEARCH_RG) {
                opts->null_output = true;
                opts->null_filename = true;
                break;
            }
            opts->null_output = true;
            opts->null_filename = true;
            break;
        case OPT_BINARY_FILES:
            if (personality == BX_SEARCH_RG) {
                fprintf(stderr, "%s: unrecognized option '--binary-files'\n", progname);
                return -1;
            }
            if (!bx_set_binary_files_mode(progname, personality, opts, optarg))
                return -1;
            break;
        case OPT_NULL_DATA:
            opts->null_data = true;
            break;
        case OPT_MULTILINE:
            if (personality != BX_SEARCH_RG) {
                return bx_grep_unrecognized_option(progname, "--multiline");
            }
            opts->multiline = true;
            break;
        case OPT_MULTILINE_DOTALL:
            if (personality != BX_SEARCH_RG) {
                return bx_grep_unrecognized_option(progname, "--multiline-dotall");
            }
            opts->multiline = true;
            opts->multiline_dotall = true;
            break;
        case OPT_STOP_ON_NONMATCH:
            if (personality != BX_SEARCH_RG) {
                return bx_grep_unrecognized_option(progname, "--stop-on-nonmatch");
            }
            opts->stop_on_nonmatch = true;
            break;
        case OPT_JSON:
            if (personality != BX_SEARCH_RG) {
                return bx_grep_unrecognized_option(progname, "--json");
            }
            fprintf(stderr, "%s: unsupported option '--json'\n", progname);
            return -1;
        case OPT_DEBUG:
            if (personality != BX_SEARCH_RG) {
                return bx_grep_unrecognized_option(progname, "--debug");
            }
            fprintf(stderr, "%s: unsupported option '--debug'\n", progname);
            return -1;
        case OPT_SORT:
        case OPT_SORTR:
            if (personality != BX_SEARCH_RG) {
                return bx_grep_unrecognized_option(
                    progname,
                    bx_search_current_option_token(optind, argc, argv,
                                                   c == OPT_SORT ? "--sort" : "--sortr"));
            }
            if (strcmp(optarg, "path") != 0) {
                fprintf(stderr,
                        "%s: unsupported argument for %s: %s\n",
                        progname,
                        c == OPT_SORT ? "--sort" : "--sortr",
                        optarg);
                return -1;
            }
            opts->sort_paths = true;
            opts->sort_paths_reverse = (c == OPT_SORTR);
            break;
        case OPT_ENGINE:
            if (personality != BX_SEARCH_RG) {
                return bx_grep_unrecognized_option(
                    progname,
                    bx_search_current_option_token(optind, argc, argv, "--engine"));
            }
            if (strcmp(optarg, "default") == 0)
                opts->rg_engine = BX_RG_ENGINE_DEFAULT;
            else if (strcmp(optarg, "pcre2") == 0)
                opts->rg_engine = BX_RG_ENGINE_PCRE2;
            else if (strcmp(optarg, "auto") == 0)
                opts->rg_engine = BX_RG_ENGINE_AUTO;
            else {
                fprintf(stderr,
                        "%s: error parsing flag --engine: unrecognized regex engine '%s'\n",
                        progname, optarg);
                return -1;
            }
            break;
        case OPT_PCRE2_VERSION:
            if (personality != BX_SEARCH_RG) {
                return bx_grep_unrecognized_option(progname, "--pcre2-version");
            }
            opts->pcre2_version = true;
            bx_regex_print_version();
            return 1;
        case OPT_REGEX_SIZE_LIMIT:
            if (personality != BX_SEARCH_RG) {
                fprintf(stderr, "%s: unrecognized option '--regex-size-limit'\n", progname);
                return -1;
            }
            if (!bx_parse_rg_size_limit(progname, "--regex-size-limit", optarg,
                                        &opts->regex_size_limit))
                return -1;
            opts->regex_size_limit_set = true;
            break;
        case OPT_DFA_SIZE_LIMIT:
            if (personality != BX_SEARCH_RG) {
                fprintf(stderr, "%s: unrecognized option '--dfa-size-limit'\n", progname);
                return -1;
            }
            if (!bx_parse_rg_size_limit(progname, "--dfa-size-limit", optarg,
                                        &opts->dfa_size_limit))
                return -1;
            opts->dfa_size_limit_set = true;
            break;
        case OPT_COLOR:
            opts->color_mode = bx_color_parse(optarg ? optarg : "auto");
            bx_color_set_mode(opts->color_mode);
            break;
        case OPT_HELP:
            bx_search_print_help(progname);
            return 1;
        case OPT_VERSION:
            bx_search_print_version(progname);
            return 1;
        case '?':
            if (personality != BX_SEARCH_RG && optind > 0 && optind <= argc) {
                const char *arg = argv[optind - 1];
                if (arg && arg[0] == '-' && isdigit((unsigned char)arg[1])) {
                    bool all_digits = true;
                    for (const char *p = arg + 1; *p; p++) {
                        if (!isdigit((unsigned char)*p)) {
                            all_digits = false;
                            break;
                        }
                    }
                    if (all_digits) {
                        int n;
                        if (!bx_parse_nonnegative_int(progname, arg, arg + 1, &n))
                            return -1;
                        opts->after_context = n;
                        opts->before_context = n;
                        break;
                    }
                }
            }
            if (optopt) {
                fprintf(stderr, "%s: invalid option -- '%c'\n", progname, optopt);
            } else if (optind > 0 && optind <= argc) {
                fprintf(stderr, "%s: unrecognized option '%s'\n", progname, argv[optind - 1]);
            } else {
                fprintf(stderr, "%s: unrecognized option\n", progname);
            }
            if (personality == BX_SEARCH_GREP) {
                bx_grep_print_usage_try_help(progname);
            }
            return -1;
        default:
            return -1;
        }
    }

    if (opts->unrestrict_level >= 1) opts->no_ignore = true;
    if (opts->unrestrict_level >= 2) opts->hidden = true;
    if (opts->unrestrict_level >= 3) {
        opts->binary_as_text = true;
        opts->binary_without_match = false;
    }

    if (personality == BX_SEARCH_RG) {
        if (opts->files_with_matches || opts->files_without_match)
            opts->count_only = false;
        if (opts->count_only)
            opts->omit_zero_count_output = true;
        if (opts->show_column)
            opts->show_line_number = true;
        if (opts->perl_regexp)
            opts->rg_engine = BX_RG_ENGINE_PCRE2;
        if (opts->rg_engine == BX_RG_ENGINE_PCRE2)
            opts->perl_regexp = true;
    }

    if (opts->files_only) {
        *pattern = "";
        *first_file = optind;
        return 0;
    }

    if (optind >= argc) {
        if (!opts->files_only && opts->num_extra_patterns == 0) {
            if (personality == BX_SEARCH_GREP) {
                bx_grep_print_usage_try_help(progname);
            } else {
                fprintf(stderr, "%s: missing pattern\n", progname);
            }
            return -1;
        }
        *pattern = opts->num_extra_patterns > 0 ? opts->extra_patterns[--opts->num_extra_patterns] : "";
    } else if (opts->num_extra_patterns > 0) {
        *pattern = opts->extra_patterns[--opts->num_extra_patterns];
    } else {
        *pattern = argv[optind++];
    }
    *first_file = optind;
    return 0;
}
