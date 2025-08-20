#define _GNU_SOURCE
#include <fnmatch.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "applets.h"
#include "bx/diag.h"
#include "search/walk.h"
#include "search/options.h"

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

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
    pcre2_code *regex;
    bool *stop;
};

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

static void fd_callback(const struct walk_entry *entry, void *user) {
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

    if (entry->is_dir) return;

    if (opts->exact_depth >= 0) {
        if (entry->depth != opts->exact_depth)
            return;
    } else if (entry->depth < opts->min_depth) {
        return;
    }

    if (opts->type_filter) {
        if (opts->show_type && strcmp(opts->type_filter, "file") == 0) {
            printf("f %s\n", entry->path);
        }
    }

    const char *name = entry->path;
    if (!opts->full_path) {
        const char *slash = strrchr(name, '/');
        name = slash ? slash + 1 : name;
    }

    if (!opts->hidden && name[0] == '.')
        return;

    if (opts->extension) {
        const char *dot = strrchr(name, '.');
        if (!dot || strcasecmp(dot + 1, opts->extension) != 0)
            return;
    }

    if (!st->regex) {
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

    int rc = pcre2_match(st->regex, (PCRE2_SPTR)name, strlen(name), 0, 0,
                         pcre2_match_data_create_from_pattern(st->regex, NULL), NULL);
    if (rc >= 0) {
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
        {"max-depth", required_argument, NULL, 'd'},
        {"min-depth", required_argument, NULL, 201},
        {"exact-depth", required_argument, NULL, 202},
        {"type",      required_argument, NULL, 't'},
        {"extension", required_argument, NULL, 'e'},
        {"max-results", required_argument, NULL, 200},
        {"print0",    no_argument,      NULL, '0'},
        {"quiet",     no_argument,      NULL, 'q'},
        {NULL, 0, NULL, 0},
    };

    opterr = 0;
    while ((opt = getopt_long(argc, argv, "hVHIpsSFgd:t:e:0qL1", long_opts, NULL)) != -1) {
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
        case 'd': opts.max_depth = atoi(optarg); break;
        case 201: opts.min_depth = atoi(optarg); break;
        case 202: opts.exact_depth = atoi(optarg); opts.max_depth = opts.exact_depth; break;
        case 't': opts.type_filter = optarg; break;
        case 'e': opts.extension = optarg; break;
        case '0': opts.print0 = true; break;
        case 'q': opts.quiet = true; break;
        case 'L': opts.follow_symlinks = true; break;
        case '1': opts.max_results = 1; break;
        case 200: opts.max_results = atoi(optarg); break;
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
        puts("  -d, --max-depth N   limit recursive depth");
        puts("      --min-depth N   skip matches shallower than N");
        puts("      --exact-depth N match only entries exactly at depth N");
        puts("  -t, --type TYPE     filter by type: f(file), d(dir), l(symlink)");
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

    pcre2_code *re = NULL;
    if (opts.pattern && !opts.glob_match && !opts.fixed_strings) {
        uint32_t flags = PCRE2_CASELESS;
        if (opts.case_sensitive) flags = 0;
        if (opts.smart_case) {
            bool has_upper = false;
            for (const char *ch = opts.pattern; *ch; ch++)
                if (*ch >= 'A' && *ch <= 'Z') { has_upper = true; break; }
            if (!has_upper) flags = PCRE2_CASELESS;
        }
        re = fd_compile_regex(progname, opts.pattern, opts.pattern, flags);
    } else if (opts.pattern && opts.glob_match) {
        char buf[4096];
        char *p = buf;
        for (const char *ch = opts.pattern; *ch; ch++) {
            switch (*ch) {
            case '*': *p++ = '.'; *p++ = '*'; break;
            case '?': *p++ = '.'; break;
            case '.': *p++ = '\\'; *p++ = '.'; break;
            default:  *p++ = *ch; break;
        }
        }
        *p = '\0';
        re = fd_compile_regex(progname, buf, opts.pattern, 0);
    } else if (opts.pattern && opts.fixed_strings) {
        size_t len = strlen(opts.pattern);
        const char *raw = opts.pattern;
        char *buf = malloc(len * 2 + 3);
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
        re = fd_compile_regex(progname, buf, opts.pattern,
                              opts.ignore_case ? PCRE2_CASELESS : 0);
        free(buf);
    }
    if (opts.pattern && !re)
        return 1;

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
    };

    struct fd_state state = {.opts = &opts, .regex = re, .stop = &stop};
    int walk_rc = 0;
    for (int i = 0; i < search_path_count && !stop; i++) {
        if (walk_dir(search_paths[i], &wopts, fd_callback, &state) != 0)
            walk_rc = -1;
    }

    if (re) pcre2_code_free(re);
    if (walk_rc != 0)
        return 1;
    if (opts.quiet)
        return opts.results > 0 ? 0 : 1;
    return 0;
}
