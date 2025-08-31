#define _GNU_SOURCE
#include <fnmatch.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "applets.h"
#include "bx/diag.h"
#include "search/metadata.h"
#include "search/walk.h"
#include "search/options.h"

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#define FD_MAX_AND_PATTERNS 16

struct fd_opts {
    bool hidden;
    bool no_ignore;
    bool follow_symlinks;
    bool full_path;
    bool ignore_case;
    bool smart_case;
    bool case_sensitive;
    bool fixed_strings;
    bool glob_match;
    bool print0;
    bool quiet;
    bool show_type;
    const char *pattern;
    const char *and_patterns[FD_MAX_AND_PATTERNS];
    int num_and_patterns;
    const char *type_filter;
    const char *extension;
    int max_depth;
    int min_depth;
    int exact_depth;
    int max_results;
    int results;
};

struct fd_state {
    struct fd_opts *opts;
    pcre2_code *regexes[FD_MAX_AND_PATTERNS + 1];
    pcre2_match_data *match_data[FD_MAX_AND_PATTERNS + 1];
    int regex_count;
    bool *stop;
};

static bool fd_parse_nonnegative_int(const char *progname, const char *optname,
                                     const char *text, int *out) {
    char *end = NULL;
    long v = strtol(text, &end, 10);
    if (!text || *text == '\0' || (end && *end != '\0') || v < 0 || v > (1 << 20)) {
        fprintf(stderr, "%s: invalid argument for %s: %s\n",
                progname, optname, text ? text : "(null)");
        return false;
    }
    *out = (int)v;
    return true;
}

static uint32_t fd_compile_flags(const struct fd_opts *opts, const char *pattern) {
    if (opts->case_sensitive)
        return 0;
    if (opts->ignore_case)
        return PCRE2_CASELESS;
    if (opts->smart_case && pattern) {
        for (const char *ch = pattern; *ch; ch++) {
            if (*ch >= 'A' && *ch <= 'Z')
                return 0;
        }
        return PCRE2_CASELESS;
    }
    return 0;
}

static const char *fd_basename(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static bool fd_parse_type_filter(const char *progname, const char *text, const char **out) {
    char type_filter = '\0';
    if (!bx_walk_parse_named_type_filter(text, &type_filter)) {
        fprintf(stderr, "%s: invalid argument for --type: %s\n", progname, text);
        return false;
    }

    switch (type_filter) {
    case 'f': *out = "f"; break;
    case 'd': *out = "d"; break;
    case 'l': *out = "l"; break;
    case 'x': *out = "x"; break;
    case 'e': *out = "e"; break;
    case 'p': *out = "p"; break;
    case 's': *out = "s"; break;
    case 'b': *out = "b"; break;
    case 'c': *out = "c"; break;
    default:
        fprintf(stderr, "%s: invalid argument for --type: %s\n", progname, text);
        return false;
    }
    return true;
}

static bool fd_matches_type(struct walk_entry *entry, const struct fd_opts *opts) {
    if (!opts->type_filter)
        return true;
    return bx_walk_entry_matches_type(entry, opts->type_filter[0]);
}

static pcre2_code *fd_compile_regex(const char *progname, const char *pattern,
                                    const char *display_pattern, uint32_t flags) {
    int errcode;
    PCRE2_SIZE erroffset;
    pcre2_code *re = pcre2_compile((PCRE2_SPTR)pattern, PCRE2_ZERO_TERMINATED,
                                   flags, &errcode, &erroffset, NULL);
    if (re)
        return re;

    PCRE2_UCHAR errbuf[256];
    int msg_rc = pcre2_get_error_message(errcode, errbuf, sizeof(errbuf));
    fprintf(stderr, "%s: invalid pattern '%s': regex parse error at offset %zu: %s\n",
            progname,
            display_pattern ? display_pattern : pattern,
            (size_t)erroffset,
            msg_rc >= 0 ? (const char *)errbuf : "regex compile failed");
    return NULL;
}

static pcre2_code *fd_compile_pattern(const char *progname, const struct fd_opts *opts,
                                      const char *pattern) {
    if (!pattern)
        return NULL;

    if (!opts->glob_match && !opts->fixed_strings) {
        uint32_t flags = fd_compile_flags(opts, pattern);
        return fd_compile_regex(progname, pattern, pattern, flags);
    }

    if (opts->glob_match) {
        char buf[4096];
        char *p = buf;
        for (const char *ch = pattern; *ch; ch++) {
            switch (*ch) {
            case '*': *p++ = '.'; *p++ = '*'; break;
            case '?': *p++ = '.'; break;
            case '.': *p++ = '\\'; *p++ = '.'; break;
            default:  *p++ = *ch; break;
            }
        }
        *p = '\0';
        return fd_compile_regex(progname, buf, pattern, fd_compile_flags(opts, pattern));
    }

    size_t len = strlen(pattern);
    const char *raw = pattern;
    char *buf = malloc(len * 2 + 3);
    if (!buf)
        return NULL;
    char *p = buf;
    for (size_t i = 0; i < len; i++) {
        char ch = raw[i];
        if (ch == '.' || ch == '\\' || ch == '+' || ch == '*' || ch == '?' ||
            ch == '[' || ch == ']' || ch == '(' || ch == ')' || ch == '{' ||
            ch == '}' || ch == '^' || ch == '$' || ch == '|')
            *p++ = '\\';
        *p++ = ch;
    }
    *p = '\0';
    pcre2_code *re = fd_compile_regex(progname, buf, pattern, fd_compile_flags(opts, pattern));
    free(buf);
    return re;
}

static bool fd_match_name(const struct fd_state *st, const char *name) {
    if (st->regex_count == 0)
        return true;

    for (int i = 0; i < st->regex_count; i++) {
        int rc = pcre2_match(st->regexes[i], (PCRE2_SPTR)name, strlen(name), 0, 0,
                             st->match_data[i], NULL);
        if (rc < 0)
            return false;
    }
    return true;
}

static void fd_callback(struct walk_entry *entry, void *user) {
    struct fd_state *st = user;
    struct fd_opts *opts = st->opts;

    if (st->stop && *st->stop)
        return;

    if (opts->max_results > 0 && opts->results >= opts->max_results) {
        if (st->stop) *st->stop = true;
        return;
    }

    if (opts->quiet && opts->results > 0) {
        if (st->stop) *st->stop = true;
        return;
    }

    if (entry->depth == 0 && entry->is_dir)
        return;

    if (opts->exact_depth >= 0) {
        if (entry->depth != opts->exact_depth)
            return;
    } else if (entry->depth < opts->min_depth) {
        return;
    }

    if (!fd_matches_type(entry, opts))
        return;

    const char *name = opts->full_path ? entry->path : fd_basename(entry->path);

    if (opts->extension) {
        const char *dot = strrchr(fd_basename(entry->path), '.');
        if (!dot || strcasecmp(dot + 1, opts->extension) != 0)
            return;
    }

    if (st->regex_count == 0) {
        opts->results++;
        if (!opts->quiet) {
            if (opts->print0)
                printf("%s%c", entry->path, '\0');
            else
                printf("%s\n", entry->path);
        }
        if ((opts->max_results > 0 && opts->results >= opts->max_results) ||
            (opts->quiet && opts->results > 0)) {
            if (st->stop) *st->stop = true;
        }
        return;
    }

    if (fd_match_name(st, name)) {
        opts->results++;
        if (!opts->quiet) {
            if (opts->print0)
                printf("%s%c", entry->path, '\0');
            else
                printf("%s\n", entry->path);
        }
        if ((opts->max_results > 0 && opts->results >= opts->max_results) ||
            (opts->quiet && opts->results > 0)) {
            if (st->stop) *st->stop = true;
        }
    }
}

int bx_fd_main(int argc, char **argv) {
    struct fd_opts opts = {0};
    opts.max_depth = -1;
    opts.exact_depth = -1;
    opts.smart_case = true;
    bool show_help = false;
    const char *progname = argv[0] ? argv[0] : "fd";

    int opt;
    static struct option long_opts[] = {
        {"help",     no_argument,       NULL, 'h'},
        {"version",  no_argument,       NULL, 'V'},
        {"hidden",   no_argument,       NULL, 'H'},
        {"no-ignore", no_argument,      NULL, 'I'},
        {"follow",   no_argument,       NULL, 'L'},
        {"full-path", no_argument,      NULL, 'p'},
        {"ignore-case", no_argument,    NULL, 'i'},
        {"case-sensitive", no_argument, NULL, 's'},
        {"fixed-strings", no_argument,  NULL, 'F'},
        {"glob",      no_argument,      NULL, 'g'},
        {"regex",     no_argument,      NULL, 204},
        {"max-depth", required_argument, NULL, 'd'},
        {"min-depth", required_argument, NULL, 201},
        {"exact-depth", required_argument, NULL, 202},
        {"type",      required_argument, NULL, 't'},
        {"extension", required_argument, NULL, 'e'},
        {"max-results", required_argument, NULL, 200},
        {"and",       required_argument, NULL, 203},
        {"print0",    no_argument,      NULL, '0'},
        {"quiet",     no_argument,      NULL, 'q'},
        {NULL, 0, NULL, 0},
    };

    opterr = 0;
    while ((opt = getopt_long(argc, argv, "hVHIpisSFgd:t:e:0qL1", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'h': show_help = true; break;
        case 'V':
            printf("fd (bx) %s\n", BX_VERSION);
            return 0;
        case 'H': opts.hidden = true; break;
        case 'I': opts.no_ignore = true; break;
        case 'p': opts.full_path = true; break;
        case 'i': opts.ignore_case = true; opts.smart_case = false; break;
        case 's': opts.case_sensitive = true; opts.smart_case = false; break;
        case 'F': opts.fixed_strings = true; break;
        case 'g': opts.glob_match = true; break;
        case 204:
            opts.fixed_strings = false;
            opts.glob_match = false;
            break;
        case 'd':
            if (!fd_parse_nonnegative_int(progname, "--max-depth", optarg, &opts.max_depth))
                return 2;
            break;
        case 201:
            if (!fd_parse_nonnegative_int(progname, "--min-depth", optarg, &opts.min_depth))
                return 2;
            break;
        case 202:
            if (!fd_parse_nonnegative_int(progname, "--exact-depth", optarg, &opts.exact_depth))
                return 2;
            opts.max_depth = opts.exact_depth;
            break;
        case 't':
            if (!fd_parse_type_filter(progname, optarg, &opts.type_filter))
                return 2;
            break;
        case 'e': opts.extension = optarg; break;
        case '0': opts.print0 = true; break;
        case 'q': opts.quiet = true; break;
        case 'L': opts.follow_symlinks = true; break;
        case '1': opts.max_results = 1; break;
        case 203:
            if (opts.num_and_patterns < FD_MAX_AND_PATTERNS)
                opts.and_patterns[opts.num_and_patterns++] = optarg;
            break;
        case 200:
            if (!fd_parse_nonnegative_int(progname, "--max-results", optarg, &opts.max_results))
                return 2;
            break;
        case '?':
            if (optind > 0 && optind <= argc)
                fprintf(stderr, "%s: unrecognized option '%s'\n", progname, argv[optind - 1]);
            else
                fprintf(stderr, "%s: unrecognized option\n", progname);
            return 2;
        }
    }

    if (show_help) {
        printf("Usage: %s [OPTIONS] [PATTERN] [PATH]...\n", argv[0]);
        puts("fd - find entries in the filesystem");
        puts("");
        puts("  -H, --hidden        search hidden files and directories");
        puts("  -I, --no-ignore     do not respect ignore files");
        puts("  -p, --full-path     match against full path, not basename");
        puts("  -i, --ignore-case   case-insensitive matching");
        puts("  -s, --case-sensitive  case-sensitive matching");
        puts("  -F, --fixed-strings treat pattern as literal string");
        puts("  -g, --glob          glob-based matching");
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
        puts("      --max-results N limit number of results");
        puts("      --help           display this help and exit");
        puts("      --version        output version information and exit");
        return 0;
    }

    opts.pattern = NULL;
    char *default_paths[] = { "." };
    char **search_paths = default_paths;
    int search_path_count = 1;
    int positional = argc - optind;
    if (positional == 1) {
        opts.pattern = argv[optind++];
    } else if (positional > 1) {
        opts.pattern = argv[optind++];
        search_paths = &argv[optind];
        search_path_count = argc - optind;
    }

    bool stop = false;
    struct walk_opts wopts = {
        .hidden = opts.hidden,
        .no_ignore = opts.no_ignore,
        .follow_symlinks = opts.follow_symlinks,
        .follow_root_symlink = true,
        .stop = &stop,
        .suppress_eacces = true,
        .error_prefix = progname,
        .max_depth = opts.max_depth,
        .cycle_mode = opts.follow_symlinks ? WALK_CYCLE_SYMLINK_REPEAT : WALK_CYCLE_NONE,
        .cycle_report = WALK_CYCLE_IGNORE,
    };

    struct fd_state state = {.opts = &opts, .stop = &stop};
    if (opts.pattern)
        state.regexes[state.regex_count++] = fd_compile_pattern(progname, &opts, opts.pattern);
    for (int i = 0; i < opts.num_and_patterns; i++)
        state.regexes[state.regex_count++] = fd_compile_pattern(progname, &opts, opts.and_patterns[i]);
    if (!opts.pattern && opts.num_and_patterns > 0) {
        for (int i = 0; i < state.regex_count; i++) {
            if (!state.regexes[i])
                return 1;
        }
    } else if (opts.pattern) {
        for (int i = 0; i < state.regex_count; i++) {
            if (!state.regexes[i])
                return 1;
        }
    }
    for (int i = 0; i < state.regex_count; i++)
        state.match_data[i] = pcre2_match_data_create_from_pattern(state.regexes[i], NULL);
    int walk_rc = 0;
    for (int i = 0; i < search_path_count && !stop; i++) {
        if (walk_dir(search_paths[i], &wopts, fd_callback, &state) != 0)
            walk_rc = -1;
    }

    for (int i = 0; i < state.regex_count; i++) {
        if (state.match_data[i])
            pcre2_match_data_free(state.match_data[i]);
        if (state.regexes[i])
            pcre2_code_free(state.regexes[i]);
    }
    if (walk_rc != 0)
        return 1;
    if (opts.quiet)
        return opts.results > 0 ? 0 : 1;
    return 0;
}
