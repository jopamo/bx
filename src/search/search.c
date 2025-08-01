#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "search.h"
#include "options.h"
#include "pcre2_matcher.h"
#include "literal.h"
#include "diag.h"

/* --- unified matcher (regex or literal) --- */

enum matcher_kind { MATCHER_REGEX, MATCHER_LITERAL };

struct bx_matcher {
    enum matcher_kind kind;
    union {
        struct bx_regex *regex;
        struct bx_literal_matcher *literal;
    };
};

static int matcher_find(struct bx_matcher *m, const unsigned char *buf, size_t len,
                        size_t start, struct bx_match *out) {
    if (m->kind == MATCHER_LITERAL)
        return bx_literal_find(m->literal, buf, len, start, out);
    struct bx_match tmp;
    int rc = bx_regex_find(m->regex, buf, len, start, &tmp);
    if (rc == 0) { out->start = tmp.start; out->end = tmp.end; }
    return rc;
}

static void matcher_free(struct bx_matcher *m) {
    if (m->kind == MATCHER_LITERAL)
        bx_literal_free(m->literal);
    else
        bx_regex_free(m->regex);
    free(m);
}

static struct bx_matcher *compile_matcher(const char *pattern, struct search_opts *opts) {
    struct bx_matcher *m = calloc(1, sizeof(*m));

    if (opts->fixed_strings) {
        m->kind = MATCHER_LITERAL;
        if (bx_literal_compile(&m->literal, pattern, opts->ignore_case) != 0) {
            free(m);
            return NULL;
        }
    } else {
        m->kind = MATCHER_REGEX;
        int flags = 0;
        if (opts->ignore_case) flags |= BX_REGEX_ICASE;
        if (bx_regex_compile(&m->regex, pattern, flags) != 0) {
            free(m);
            return NULL;
        }
    }
    return m;
}

/* --- line buffering for context --- */

struct line_buf {
    char  *text;
    size_t len;
    bool   match;
    bool   print;
};

static void free_lines(struct line_buf *lines, int count) {
    for (int i = 0; i < count; i++) free(lines[i].text);
    free(lines);
}

static bool needs_line_buffering(struct search_opts *opts) {
    return opts->after_context > 0 || opts->before_context > 0;
}

static int search_file_buffered(const char *filename, const char *display_name,
                                  struct bx_matcher *m, struct search_opts *opts,
                                int *match_count) {
    FILE *f = stdin;
    if (display_name) {
        f = fopen(filename, "r");
        if (!f) { fprintf(stderr, "grep: %s: %s\n", filename, strerror(errno)); return 2; }
    }

    int cap = 256;
    struct line_buf *lines = malloc((size_t)cap * sizeof(*lines));
    int nlines = 0;
    char *raw = NULL;
    size_t raw_cap = 0;
    ssize_t len;
    int file_matches = 0;

    while ((len = getline(&raw, &raw_cap, f)) != -1) {
        if (nlines >= cap) { cap *= 2; lines = realloc(lines, (size_t)cap * sizeof(*lines)); }
        lines[nlines].text = malloc((size_t)len + 1);
        memcpy(lines[nlines].text, raw, (size_t)len + 1);
        lines[nlines].len = (size_t)len;
        lines[nlines].print = false;
        struct bx_match bm;
        bool matched = (matcher_find(m, (unsigned char *)raw, (size_t)len, 0, &bm) == 0);
        if (opts->invert_match) matched = !matched;
        lines[nlines].match = matched;
        if (matched) file_matches++;
        nlines++;
    }
    free(raw);
    if (f != stdin) fclose(f);

    if (opts->quiet && file_matches > 0) { free_lines(lines, nlines); *match_count += file_matches; return 0; }

    if (opts->count_only || opts->files_with_matches || opts->files_without_match) {
        if (opts->count_only) {
            if (opts->show_filename && display_name) printf("%s:%d\n", display_name, file_matches);
            else printf("%d\n", file_matches);
        }
        if (opts->files_with_matches && file_matches > 0 && display_name) printf("%s\n", display_name);
        if (opts->files_without_match && file_matches == 0 && display_name) printf("%s\n", display_name);
        *match_count += file_matches;
        free_lines(lines, nlines);
        return file_matches > 0 ? 0 : 1;
    }

    for (int i = 0; i < nlines; i++) {
        if (!lines[i].match) continue;
        int start = i - opts->before_context;
        if (start < 0) start = 0;
        int end = i + opts->after_context + 1;
        if (end > nlines) end = nlines;
        for (int j = start; j < end; j++) lines[j].print = true;
    }

    bool in_group = false;
    int last_printed = -1;
    for (int i = 0; i < nlines; i++) {
        if (!lines[i].print) { in_group = false; continue; }
        if (!in_group && last_printed >= 0 && i > last_printed + 1) {
            if (opts->show_filename && display_name) printf("%s--\n", display_name);
            else puts("--");
        }
        if (opts->show_filename && display_name) printf("%s%c", display_name, lines[i].match ? ':' : '-');
        if (opts->show_line_number) printf("%d%c", i + 1, lines[i].match ? ':' : '-');
        if (opts->only_matching && lines[i].match) {
            struct bx_match bm;
            matcher_find(m, (unsigned char *)lines[i].text, lines[i].len, 0, &bm);
            fwrite(lines[i].text + bm.start, 1, bm.end - bm.start, stdout);
            putchar('\n');
        } else {
            fwrite(lines[i].text, 1, lines[i].len, stdout);
            if (lines[i].text[lines[i].len - 1] != '\n') putchar('\n');
        }
        in_group = true; last_printed = i;
    }
    *match_count += file_matches;
    free_lines(lines, nlines);
    return file_matches > 0 ? 0 : 1;
}

/* --- streaming search (no context) --- */

static int search_file_streaming(const char *filename, const char *display_name,
                                struct bx_matcher *m, struct search_opts *opts,
                                  int *match_count) {
    FILE *f = stdin;
    if (display_name) {
        f = fopen(filename, "r");
        if (!f) { fprintf(stderr, "grep: %s: %s\n", filename, strerror(errno)); return 2; }
    }

    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    int line_num = 0, file_matches = 0, status = 1;

    while ((len = getline(&line, &cap, f)) != -1) {
        line_num++;
        struct bx_match bm;
        bool matched = (matcher_find(m, (unsigned char *)line, (size_t)len, 0, &bm) == 0);
        if (opts->invert_match) matched = !matched;
        if (matched) {
            file_matches++; status = 0;
            if (opts->quiet) break;
            if (opts->count_only || opts->files_with_matches || opts->files_without_match) continue;
            if (opts->show_filename && display_name) printf("%s:", display_name);
            if (opts->show_line_number) printf("%d:", line_num);
            if (opts->only_matching) {
                fwrite(line + bm.start, 1, bm.end - bm.start, stdout); putchar('\n');
            } else {
                fwrite(line, 1, (size_t)len, stdout);
                if (line[len - 1] != '\n') putchar('\n');
            }
        }
    }

    if (opts->quiet && file_matches > 0) status = 0;
    if (opts->count_only) {
        if (opts->show_filename && display_name) printf("%s:%d\n", display_name, file_matches);
        else printf("%d\n", file_matches);
    }
    if (opts->files_with_matches && file_matches > 0 && display_name) printf("%s\n", display_name);
    if (opts->files_without_match && file_matches == 0 && display_name) printf("%s\n", display_name);
    if (match_count) *match_count += file_matches;
    free(line);
    if (f != stdin) fclose(f);
    return status;
}

static int search_file(const char *filename, struct bx_matcher *m, struct search_opts *opts,
                       int *match_count) {
    const char *display_name = filename;
    if (!filename || strcmp(filename, "-") == 0) display_name = NULL;
    if (needs_line_buffering(opts))
        return search_file_buffered(filename, display_name, m, opts, match_count);
    return search_file_streaming(filename, display_name, m, opts, match_count);
}

/* --- binary detection --- */

static bool is_binary(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    unsigned char buf[1024];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    if (n == 0) return false;
    for (size_t i = 0; i < n; i++)
        if (buf[i] == 0) return true;
    return false;
}

/* --- pattern matching helpers --- */

static bool match_any_pattern(const char *name, char **patterns, int n) {
    for (int i = 0; i < n; i++)
        if (fnmatch(patterns[i], name, 0) == 0)
            return true;
    return n == 0;
}

static bool excluded_by_patterns(const char *name, char **patterns, int n) {
    if (n == 0) return false;
    return match_any_pattern(name, patterns, n);
}

/* --- directory walker --- */

struct file_entry { char *path; };
struct file_list {
    struct file_entry *entries;
    int count, cap;
};

static void file_list_add(struct file_list *fl, const char *path) {
    if (fl->count >= fl->cap) {
        fl->cap = fl->cap ? fl->cap * 2 : 256;
        fl->entries = realloc(fl->entries, (size_t)fl->cap * sizeof(*fl->entries));
    }
    fl->entries[fl->count].path = strdup(path);
    fl->count++;
}

static bool file_list_contains(struct file_list *fl, const char *path) {
    for (int i = 0; i < fl->count; i++)
        if (strcmp(fl->entries[i].path, path) == 0) return true;
    return false;
}

static void file_list_free(struct file_list *fl) {
    for (int i = 0; i < fl->count; i++) free(fl->entries[i].path);
    free(fl->entries);
}

static int walk_dir(const char *dirpath, struct file_list *fl, struct search_opts *opts,
                    int depth, struct file_list *visited) {
    if (depth > 100) return 0;
    DIR *d = opendir(dirpath);
    if (!d) {
        if (errno != EACCES)
            fprintf(stderr, "grep: %s: %s\n", dirpath, strerror(errno));
        return 0;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        size_t plen = strlen(dirpath) + 1 + strlen(ent->d_name) + 1;
        char *full = malloc(plen);
        snprintf(full, plen, "%s/%s", dirpath, ent->d_name);
        struct stat st;
        int stat_rc = opts->follow_symlinks ? stat(full, &st) : lstat(full, &st);
        if (stat_rc != 0) { free(full); continue; }
        if (S_ISDIR(st.st_mode)) {
            if (excluded_by_patterns(ent->d_name, opts->exclude_dir_patterns, opts->num_exclude_dir)) {
                free(full); continue;
            }
            char *real = realpath(full, NULL);
            if (real) {
                if (file_list_contains(visited, real)) { free(real); free(full); continue; }
                file_list_add(visited, real);
                free(real);
            }
            walk_dir(full, fl, opts, depth + 1, visited);
        } else if (S_ISREG(st.st_mode)) {
            if (!match_any_pattern(ent->d_name, opts->include_patterns, opts->num_include)) {
                free(full); continue;
            }
            if (excluded_by_patterns(ent->d_name, opts->exclude_patterns, opts->num_exclude)) {
                free(full); continue;
            }
            file_list_add(fl, full);
        }
        free(full);
    }
    closedir(d);
    return 0;
}

/* --- main entry point --- */

int bx_search_main(int argc, char **argv, enum bx_search_personality personality) {
    struct search_opts opts;
    const char *pattern;
    int first_file;

    int rc = bx_search_parse_options(argc, argv, &opts, personality, &pattern, &first_file);
    if (rc != 0) {
        bx_search_free_options(&opts);
        return rc == 1 ? 0 : 2;
    }

    struct bx_matcher *m = compile_matcher(pattern, &opts);
    if (!m) {
        fprintf(stderr, "%s: invalid pattern: %s\n",
                argv[0] ? argv[0] : "grep", pattern);
        bx_search_free_options(&opts);
        return 2;
    }

    int num_files = argc - first_file;
    if (!opts.show_filename && !opts.hide_filename)
        opts.show_filename = (num_files > 1 || opts.recursive);
    if (opts.hide_filename)
        opts.show_filename = false;

    int global_matches = 0;
    int exit_status = 1;

    if (num_files == 0) {
        exit_status = search_file(NULL, m, &opts, &global_matches);
    } else if (opts.recursive) {
        struct file_list fl = {0};
        struct file_list visited = {0};
        for (int j = first_file; j < argc; j++) {
            struct stat st;
            if (stat(argv[j], &st) != 0) {
                fprintf(stderr, "grep: %s: %s\n", argv[j], strerror(errno));
                exit_status = 2;
                continue;
            }
            if (S_ISDIR(st.st_mode)) {
                char *real = realpath(argv[j], NULL);
                if (real) { file_list_add(&visited, real); free(real); }
                walk_dir(argv[j], &fl, &opts, 0, &visited);
            } else if (S_ISREG(st.st_mode)) {
                if (match_any_pattern(argv[j], opts.include_patterns, opts.num_include) &&
                    !excluded_by_patterns(argv[j], opts.exclude_patterns, opts.num_exclude))
                    file_list_add(&fl, argv[j]);
            }
        }
        for (int j = 0; j < fl.count; j++) {
            if (!opts.binary_as_text && is_binary(fl.entries[j].path)) {
                if (!opts.binary_without_match && !opts.quiet) {
                    bool matched = false;
                    FILE *f = fopen(fl.entries[j].path, "r");
                    if (f) {
                        char *line = NULL; size_t cap = 0;
                        ssize_t len;
                        while ((len = getline(&line, &cap, f)) != -1) {
                            struct bx_match bm;
                            if (matcher_find(m, (unsigned char *)line, (size_t)len, 0, &bm) == 0)
                                { matched = true; break; }
                        }
                        free(line); fclose(f);
                    }
                    if (matched) {
                        printf("Binary file %s matches\n", fl.entries[j].path);
                        global_matches++; exit_status = 0;
                    }
                }
                continue;
            }
            int r = search_file(fl.entries[j].path, m, &opts, &global_matches);
            if (r == 2) exit_status = 2;
            else if (r == 0) exit_status = 0;
        }
        file_list_free(&fl);
        file_list_free(&visited);
    } else {
        for (int j = first_file; j < argc; j++) {
            int r = search_file(argv[j], m, &opts, &global_matches);
            if (r == 2) exit_status = 2;
            else if (r == 0) exit_status = 0;
        }
    }

    matcher_free(m);
    bx_search_free_options(&opts);
    return exit_status;
}
