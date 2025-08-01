#define _GNU_SOURCE
#include <fnmatch.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "applets.h"
#include "diag.h"
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
    int max_results;
    int results;
};

struct fd_state {
    struct fd_opts *opts;
    pcre2_code *regex;
};

static void fd_callback(const struct walk_entry *entry, void *user) {
    struct fd_state *st = user;
    struct fd_opts *opts = st->opts;

    if (opts->max_results > 0 && opts->results >= opts->max_results)
        return;

    if (opts->quiet && opts->results > 0)
        return;

    if (entry->is_dir) return;

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
        if (opts->print0)
            printf("%s%c", entry->path, '\0');
        else
            printf("%s\n", entry->path);
        return;
    }

    int rc = pcre2_match(st->regex, (PCRE2_SPTR)name, strlen(name), 0, 0,
                         pcre2_match_data_create_from_pattern(st->regex, NULL), NULL);
    if (rc >= 0) {
        opts->results++;
        if (opts->print0)
            printf("%s%c", entry->path, '\0');
        else
            printf("%s\n", entry->path);
    }
}

int bx_fd_main(int argc, char **argv) {
    struct fd_opts opts = {0};
    opts.max_depth = -1;
    bool show_help = false;

    int opt;
    static struct option long_opts[] = {
        {"help",     no_argument,       NULL, 'h'},
        {"version",  no_argument,       NULL, 'V'},
        {"hidden",   no_argument,       NULL, 'H'},
        {"no-ignore", no_argument,      NULL, 'I'},
        {"full-path", no_argument,      NULL, 'p'},
        {"ignore-case", no_argument,    NULL, 'i'},
        {"case-sensitive", no_argument, NULL, 's'},
        {"fixed-strings", no_argument,  NULL, 'F'},
        {"glob",      no_argument,      NULL, 'g'},
        {"max-depth", required_argument, NULL, 'd'},
        {"type",      required_argument, NULL, 't'},
        {"extension", required_argument, NULL, 'e'},
        {"max-results", required_argument, NULL, 200},
        {"print0",    no_argument,      NULL, '0'},
        {"quiet",     no_argument,      NULL, 'q'},
        {NULL, 0, NULL, 0},
    };

    opterr = 0;
    while ((opt = getopt_long(argc, argv, "hVHIpsSFgd:t:e:0qL", long_opts, NULL)) != -1) {
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
        case 't': opts.type_filter = optarg; break;
        case 'e': opts.extension = optarg; break;
        case '0': opts.print0 = true; break;
        case 'q': opts.quiet = true; break;
        case 'L': opts.follow_symlinks = true; break;
        case 200: opts.max_results = atoi(optarg); break;
        case '?': return 1;
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
        puts("  -t, --type TYPE     filter by type: f(file), d(dir), l(symlink)");
        puts("  -e, --extension EXT filter by file extension");
        puts("  -0, --print0        separate results by NUL byte");
        puts("  -q, --quiet         suppress normal output");
        puts("  -L, --follow        follow symlinks");
        puts("      --max-results N limit number of results");
        puts("      --help           display this help and exit");
        puts("      --version        output version information and exit");
        return 0;
    }

    opts.pattern = NULL;
    const char *search_path = ".";
    if (optind < argc && argv[optind][0] != '.' && argv[optind][0] != '/') {
        opts.pattern = argv[optind++];
    }
    if (optind < argc) {
        search_path = argv[optind];
    }

    pcre2_code *re = NULL;
    if (opts.pattern && !opts.glob_match && !opts.fixed_strings) {
        int errcode;
        PCRE2_SIZE erroffset;
        uint32_t flags = PCRE2_CASELESS;
        if (opts.case_sensitive) flags = 0;
        if (opts.smart_case) {
            bool has_upper = false;
            for (const char *ch = opts.pattern; *ch; ch++)
                if (*ch >= 'A' && *ch <= 'Z') { has_upper = true; break; }
            if (!has_upper) flags = PCRE2_CASELESS;
        }
        re = pcre2_compile((PCRE2_SPTR)opts.pattern, PCRE2_ZERO_TERMINATED,
                           flags, &errcode, &erroffset, NULL);
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
        int errcode; PCRE2_SIZE erroffset;
        re = pcre2_compile((PCRE2_SPTR)buf, PCRE2_ZERO_TERMINATED, 0, &errcode, &erroffset, NULL);
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
        int errcode; PCRE2_SIZE erroffset;
        re = pcre2_compile((PCRE2_SPTR)buf, PCRE2_ZERO_TERMINATED,
                           opts.ignore_case ? PCRE2_CASELESS : 0, &errcode, &erroffset, NULL);
        free(buf);
    }

    struct walk_opts wopts = {
        .hidden = opts.hidden,
        .no_ignore = opts.no_ignore,
        .follow_symlinks = opts.follow_symlinks,
        .max_depth = opts.max_depth,
    };

    struct fd_state state = {.opts = &opts, .regex = re};
    walk_dir(search_path, &wopts, fd_callback, &state);

    if (re) pcre2_code_free(re);
    return opts.results > 0 || !opts.pattern ? 0 : 1;
}
