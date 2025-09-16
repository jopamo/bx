#include <ctype.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "options.h"
#include "search.h"
#include "bx/diag.h"

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
    OPT_FILES_WITH_MATCHES,
    OPT_FILES_WITHOUT_MATCH,
    OPT_FOLLOW,
    OPT_MAX_DEPTH,
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
    OPT_NULL,
    OPT_BINARY_FILES,
    OPT_NULL_DATA,
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

static bool bx_set_binary_files_mode(const char *progname, struct search_opts *opts,
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

    fprintf(stderr, "%s: invalid argument for --binary-files: %s\n",
            progname, value);
    return false;
}

void bx_search_print_help(const char *progname) {
    printf("Usage: %s [OPTION]... PATTERN [FILE]...\n", progname);
    puts("Search for PATTERN in each FILE.");
    puts("");
    puts("  -E            PATTERN is an extended regular expression");
    puts("  -F            PATTERN is a set of fixed strings");
    puts("  -b            print the byte offset with output lines");
    puts("  -H            print the file name for each match");
    puts("  -h            suppress the file name prefix on output");
    puts("  -i            ignore case distinctions");
    puts("  -n            print line number with output lines");
    puts("  -o            show only the part of a line matching PATTERN");
    puts("  -v            select non-matching lines");
    puts("  -c            print only a count of matching lines per FILE");
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
}

int bx_search_parse_options(int argc, char **argv, struct search_opts *opts,
                             enum bx_search_personality personality,
                             const char **pattern, int *first_file) {
    memset(opts, 0, sizeof(*opts));

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
        {"include",      required_argument, NULL, OPT_INCLUDE},
        {"exclude",      required_argument, NULL, OPT_EXCLUDE},
        {"exclude-from", required_argument, NULL, OPT_EXCLUDE_FROM},
        {"exclude-dir",  required_argument, NULL, OPT_EXCLUDE_DIR},
        {"files",        no_argument,       NULL, OPT_FILES},
        {"files-with-matches", no_argument, NULL, OPT_FILES_WITH_MATCHES},
        {"files-without-match", no_argument, NULL, OPT_FILES_WITHOUT_MATCH},
        {"follow",       no_argument,       NULL, OPT_FOLLOW},
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
        {"max-depth",    required_argument, NULL, OPT_MAX_DEPTH},
        {"color",        optional_argument, NULL, OPT_COLOR},
        {"colour",       optional_argument, NULL, OPT_COLOR},
        {"label",        required_argument, NULL, OPT_LABEL},
        {"group-separator", required_argument, NULL, OPT_GROUP_SEPARATOR},
        {"no-group-separator", no_argument, NULL, OPT_NO_GROUP_SEPARATOR},
        {"null",         no_argument,       NULL, OPT_NULL},
        {"binary-files", required_argument, NULL, OPT_BINARY_FILES},
        {"null-data",    no_argument,       NULL, OPT_NULL_DATA},
        {NULL, 0, NULL, 0},
    };

    opterr = 0;
    optind = 1;

    int c;
    while ((c = getopt_long(argc, argv, "0EFHbhinovclLqrRIszZd:aA:B:C:e:f:g:j:t:T:uwPxSm:", long_opts, NULL)) != -1) {
        switch (c) {
        case '0':
            if (personality == BX_SEARCH_RG) {
                opts->null_output = true;
            } else {
                fprintf(stderr, "%s: unrecognized option '-0'\n", argv[0]);
                return -1;
            }
            break;
        case 'E': opts->extended_regex = true; break;
        case 'F': opts->fixed_strings = true; break;
        case 'b': opts->show_byte_offset = true; break;
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
            if (personality == BX_SEARCH_RG) {
                if (!bx_parse_nonnegative_int(argv[0], "-d", optarg, &opts->max_depth))
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
                    fprintf(stderr, "%s: invalid argument for -d: %s\n", argv[0], optarg);
                    return -1;
                }
            }
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
                fprintf(stderr, "%s: invalid option -- 's'\n", argv[0]);
                return -1;
            }
            break;
        case 'S':
            if (personality == BX_SEARCH_RG) {
                opts->smart_case = true;
                opts->ignore_case = false;
            } else {
                fprintf(stderr, "%s: invalid option -- 'S'\n", argv[0]);
                return -1;
            }
            break;
        case 'm':
            if (!bx_parse_nonnegative_int(argv[0], "-m", optarg, &opts->max_count))
                return -1;
            break;
        case 'Z':
            if (personality == BX_SEARCH_RG) {
                fprintf(stderr, "%s: invalid option -- 'Z'\n", argv[0]);
                return -1;
            }
            opts->null_output = true;
            opts->null_filename = true;
            break;
        case 'z':
            if (personality == BX_SEARCH_RG) {
                fprintf(stderr, "%s: invalid option -- 'z'\n", argv[0]);
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
                fprintf(stderr, "%s: unrecognized option '--iglob'\n", argv[0]);
                return -1;
            }
            if (optarg && optarg[0] == '!') {
                fprintf(stderr, "%s: unsupported option argument for --iglob: %s\n", argv[0], optarg);
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
        case OPT_HIDDEN:
            if (personality == BX_SEARCH_RG) {
                opts->hidden = true;
            } else {
                fprintf(stderr, "%s: unrecognized option '--hidden'\n", argv[0]);
                return -1;
            }
            break;
        case 'j':
            break;  /* thread count accepted, single-threaded for now */
        case 't':
        case 'T': {
            const char *globs = bx_get_type_globs(opts, optarg);
            if (!globs) {
                if (personality == BX_SEARCH_RG) {
                    fprintf(stderr, "%s: unrecognized file type: %s\n", argv[0], optarg);
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
                fprintf(stderr, "%s: unrecognized option '--type-add'\n", argv[0]);
                return -1;
            }
            if (!bx_add_custom_type(opts, optarg)) {
                fprintf(stderr, "%s: invalid argument for --type-add: %s\n", argv[0], optarg);
                return -1;
            }
            break;
        case OPT_TYPE_CLEAR:
            if (personality != BX_SEARCH_RG) {
                fprintf(stderr, "%s: unrecognized option '--type-clear'\n", argv[0]);
                return -1;
            }
            if (!bx_clear_type(opts, optarg)) {
                fprintf(stderr, "%s: invalid argument for --type-clear: %s\n", argv[0], optarg);
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
                fprintf(stderr, "%s: %s: %s\n", argv[0], optarg, strerror(errno));
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
            if (!bx_parse_nonnegative_int(argv[0], "-A", optarg, &opts->after_context))
                return -1;
            break;
        case 'B':
            if (!bx_parse_nonnegative_int(argv[0], "-B", optarg, &opts->before_context))
                return -1;
            break;
        case 'C': {
            int n;
            if (!bx_parse_nonnegative_int(argv[0], "-C", optarg, &n))
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
                fprintf(stderr, "%s: %s: %s\n", argv[0], optarg, strerror(errno));
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
                fprintf(stderr, "%s: unrecognized option '--follow'\n", argv[0]);
                return -1;
            }
            break;
        case OPT_NO_IGNORE:
            if (personality == BX_SEARCH_RG) {
                opts->no_ignore = true;
            } else {
                fprintf(stderr, "%s: unrecognized option '--no-ignore'\n", argv[0]);
                return -1;
            }
            break;
        case OPT_NO_IGNORE_PARENT:
            if (personality == BX_SEARCH_RG) {
                opts->no_ignore_parent = true;
            } else {
                fprintf(stderr, "%s: unrecognized option '--no-ignore-parent'\n", argv[0]);
                return -1;
            }
            break;
        case OPT_NO_IGNORE_VCS:
            if (personality == BX_SEARCH_RG) {
                opts->no_ignore_vcs = true;
            } else {
                fprintf(stderr, "%s: unrecognized option '--no-ignore-vcs'\n", argv[0]);
                return -1;
            }
            break;
        case OPT_NO_IGNORE_DOT:
            if (personality == BX_SEARCH_RG) {
                opts->no_ignore_dot = true;
            } else {
                fprintf(stderr, "%s: unrecognized option '--no-ignore-dot'\n", argv[0]);
                return -1;
            }
            break;
        case OPT_NO_REQUIRE_GIT:
            if (personality == BX_SEARCH_RG) {
                opts->no_require_git = true;
            } else {
                fprintf(stderr, "%s: unrecognized option '--no-require-git'\n", argv[0]);
                return -1;
            }
            break;
        case OPT_TYPE_LIST:
            bx_search_print_type_list();
            return 1;
        case OPT_MAX_DEPTH:
            if (personality == BX_SEARCH_RG) {
                if (!bx_parse_nonnegative_int(argv[0], "--max-depth", optarg, &opts->max_depth))
                    return -1;
            } else {
                fprintf(stderr, "%s: unrecognized option '--max-depth'\n", argv[0]);
                return -1;
            }
            break;
        case OPT_LABEL:
            if (personality == BX_SEARCH_RG) {
                fprintf(stderr, "%s: unrecognized option '--label'\n", argv[0]);
                return -1;
            }
            free(opts->label);
            opts->label = strdup(optarg);
            break;
        case OPT_GROUP_SEPARATOR:
            if (personality == BX_SEARCH_RG) {
                fprintf(stderr, "%s: unrecognized option '--group-separator'\n", argv[0]);
                return -1;
            }
            free(opts->group_separator);
            opts->group_separator = strdup(optarg);
            opts->suppress_group_separator = false;
            break;
        case OPT_NO_GROUP_SEPARATOR:
            if (personality == BX_SEARCH_RG) {
                fprintf(stderr, "%s: unrecognized option '--no-group-separator'\n", argv[0]);
                return -1;
            }
            opts->suppress_group_separator = true;
            break;
        case OPT_NULL:
            if (personality == BX_SEARCH_RG) {
                fprintf(stderr, "%s: unrecognized option '--null'\n", argv[0]);
                return -1;
            }
            opts->null_output = true;
            opts->null_filename = true;
            break;
        case OPT_BINARY_FILES:
            if (personality == BX_SEARCH_RG) {
                fprintf(stderr, "%s: unrecognized option '--binary-files'\n", argv[0]);
                return -1;
            }
            if (!bx_set_binary_files_mode(argv[0], opts, optarg))
                return -1;
            break;
        case OPT_NULL_DATA:
            if (personality == BX_SEARCH_RG) {
                fprintf(stderr, "%s: unrecognized option '--null-data'\n", argv[0]);
                return -1;
            }
            opts->null_data = true;
            break;
        case OPT_COLOR:
            opts->color_mode = bx_color_parse(optarg ? optarg : "auto");
            bx_color_set_mode(opts->color_mode);
            break;
        case OPT_HELP:
            bx_search_print_help(argv[0]);
            return 1;
        case OPT_VERSION:
            bx_search_print_version(argv[0]);
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
                        if (!bx_parse_nonnegative_int(argv[0], arg, arg + 1, &n))
                            return -1;
                        opts->after_context = n;
                        opts->before_context = n;
                        break;
                    }
                }
            }
            if (optopt) {
                fprintf(stderr, "%s: invalid option -- '%c'\n", argv[0], optopt);
            } else if (optind > 0 && optind <= argc) {
                fprintf(stderr, "%s: unrecognized option '%s'\n", argv[0], argv[optind - 1]);
            } else {
                fprintf(stderr, "%s: unrecognized option\n", argv[0]);
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

    if (opts->files_only) {
        *pattern = "";
        *first_file = optind;
        return 0;
    }

    if (optind >= argc) {
        if (!opts->files_only && opts->num_extra_patterns == 0) {
            fprintf(stderr, "%s: missing pattern\n", argv[0]);
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
