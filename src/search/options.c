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
    OPT_EXCLUDE_DIR,
    OPT_FILES,
    OPT_TYPE_LIST,
    OPT_COLOR,
};

void bx_search_print_help(const char *progname) {
    printf("Usage: %s [OPTION]... PATTERN [FILE]...\n", progname);
    puts("Search for PATTERN in each FILE.");
    puts("");
    puts("  -E            PATTERN is an extended regular expression");
    puts("  -F            PATTERN is a set of fixed strings");
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
    puts("      --include=GLOB   search only files matching GLOB");
    puts("      --exclude=GLOB   skip files matching GLOB");
    puts("      --exclude-dir=GLOB  skip directories matching GLOB");
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

static const char *bx_get_type_globs(const char *name) {
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

void bx_search_free_options(struct search_opts *opts) {
    for (int i = 0; i < opts->num_include; i++) free(opts->include_patterns[i]);
    for (int i = 0; i < opts->num_exclude; i++) free(opts->exclude_patterns[i]);
    for (int i = 0; i < opts->num_exclude_dir; i++) free(opts->exclude_dir_patterns[i]);
    for (int i = 0; i < opts->num_extra_patterns; i++) free(opts->extra_patterns[i]);
}

int bx_search_parse_options(int argc, char **argv, struct search_opts *opts,
                             enum bx_search_personality personality,
                             const char **pattern, int *first_file) {
    memset(opts, 0, sizeof(*opts));

    if (personality == BX_SEARCH_EGREP) opts->extended_regex = true;
    if (personality == BX_SEARCH_FGREP) opts->fixed_strings = true;
    if (personality == BX_SEARCH_RG) {
        opts->recursive = true;
        opts->follow_symlinks = false;
        opts->binary_without_match = true;
        opts->hidden = false;
        opts->no_ignore = false;
        opts->smart_case = true;
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
        {"exclude-dir",  required_argument, NULL, OPT_EXCLUDE_DIR},
        {"files",        no_argument,       NULL, OPT_FILES},
        {"glob",         required_argument, NULL, 'g'},
        {"type",         required_argument, NULL, 't'},
        {"type-not",     required_argument, NULL, 'T'},
        {"type-list",    no_argument,       NULL, OPT_TYPE_LIST},
        {"file",         required_argument, NULL, 'f'},
        {"color",        optional_argument, NULL, OPT_COLOR},
        {"colour",       optional_argument, NULL, OPT_COLOR},
        {NULL, 0, NULL, 0},
    };

    opterr = 0;
    optind = 1;

    int c;
    while ((c = getopt_long(argc, argv, "EFHhinovclLqrRIaA:B:C:e:f:g:j:t:T:uwPxSm:", long_opts, NULL)) != -1) {
        switch (c) {
        case 'E': opts->extended_regex = true; break;
        case 'F': opts->fixed_strings = true; break;
        case 'H': opts->show_filename = true; break;
        case 'h': opts->hide_filename = true; break;
        case 'i': opts->ignore_case = true; break;
        case 'n': opts->show_line_number = true; break;
        case 'o': opts->only_matching = true; break;
        case 'v': opts->invert_match = true; break;
        case 'c': opts->count_only = true; break;
        case 'l': opts->files_with_matches = true; break;
        case 'L': opts->files_without_match = true; break;
        case 'q': opts->quiet = true; break;
        case 'r': opts->recursive = true; opts->follow_symlinks = false; break;
        case 'R': opts->recursive = true; opts->follow_symlinks = true; break;
        case 'I': opts->binary_without_match = true; break;
        case 'a': opts->binary_as_text = true; break;
        case 'w': opts->word_regexp = true; break;
        case 'x': opts->line_regexp = true; break;
        case 'P': opts->perl_regexp = true; break;
        case 'S': opts->smart_case = true; opts->ignore_case = false; break;
        case 'm': opts->max_count = atoi(optarg); break;
        case 'g':
            if (opts->num_include < MAX_INCLUDE_PATTERNS)
                opts->include_patterns[opts->num_include++] = strdup(optarg);
            break;
        case 'u':
            if (opts->unrestrict_level < 3) opts->unrestrict_level++;
            break;
        case 'j':
            break;  /* thread count accepted, single-threaded for now */
        case 't':
        case 'T': {
            const char *globs = bx_get_type_globs(optarg);
            if (globs) {
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
            }
            break;
        }
        case 'e':
            if (opts->num_extra_patterns < 16)
                opts->extra_patterns[opts->num_extra_patterns++] = strdup(optarg);
            break;
        case 'f': {
            FILE *pf = fopen(optarg, "r");
            if (!pf) {
                fprintf(stderr, "%s: %s: %s\n", argv[0], optarg, strerror(errno));
                return -1;
            }
            char *line = NULL; size_t cap = 0;
            while (getline(&line, &cap, pf) != -1) {
                size_t llen = strlen(line);
                while (llen > 0 && (line[llen-1] == '\n' || line[llen-1] == '\r'))
                    line[--llen] = '\0';
                if (llen > 0 && opts->num_extra_patterns < 16)
                    opts->extra_patterns[opts->num_extra_patterns++] = strdup(line);
            }
            free(line); fclose(pf);
            break;
        }
        case 'A': opts->after_context = atoi(optarg); break;
        case 'B': opts->before_context = atoi(optarg); break;
        case 'C': {
            int n = atoi(optarg);
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
        case OPT_EXCLUDE_DIR:
            if (opts->num_exclude_dir < MAX_EXCLUDE_DIR_PATTERNS)
                opts->exclude_dir_patterns[opts->num_exclude_dir++] = strdup(optarg);
            break;
        case OPT_FILES:
            opts->files_only = true;
            break;
        case OPT_TYPE_LIST:
            bx_search_print_type_list();
            return 1;
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
            if (optopt)
                fprintf(stderr, "%s: invalid option -- '%c'\n", argv[0], optopt);
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
