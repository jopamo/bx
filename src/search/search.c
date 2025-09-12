#define _GNU_SOURCE
#include <errno.h>
#include <fnmatch.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <poll.h>
#include <regex.h>
#include <unistd.h>
#include "search.h"
#include "options.h"
#include "walk.h"
#include "pcre2_matcher.h"
#include "literal.h"
#include "lib/color.h"
#include "bx/diag.h"

static bool progname_uses_os_error_style(const char *progname) {
    if (!progname) return false;
    const char *base = strrchr(progname, '/');
    if (base) progname = base + 1;
    return strcmp(progname, "rg") == 0 || strcmp(progname, "bxrg") == 0;
}

static char *bx_regex_strerror_dup(int rc, const regex_t *regex) {
    size_t needed = regerror(rc, regex, NULL, 0);
    char *buf = malloc(needed > 0 ? needed : 1);
    if (!buf)
        return NULL;
    regerror(rc, regex, buf, needed > 0 ? needed : 1);
    return buf;
}

static void report_path_error(const char *progname, const char *path, int errnum) {
    if (progname_uses_os_error_style(progname))
        fprintf(stderr, "%s: %s: %s (os error %d)\n",
                progname, path, strerror(errnum), errnum);
    else
        fprintf(stderr, "%s: %s: %s\n", progname, path, strerror(errnum));
}

static void report_binary_match(const char *progname, const char *path) {
    fprintf(stderr, "%s: %s: binary file matches\n", progname, path);
}

static const char *display_path_for_output(const char *path, bool strip_dot_prefix) {
    if (strip_dot_prefix && path && path[0] == '.' && path[1] == '/')
        return path + 2;
    return path;
}

static const char *display_name_for_stream(const char *filename, const char *display_name_override,
                                           struct search_opts *opts) {
    if (display_name_override)
        return display_name_override;
    if (!filename || strcmp(filename, "-") == 0)
        return opts->label ? opts->label : "(standard input)";
    return filename;
}

struct bx_matcher;

static bool is_binary(const char *path);
static int search_binary_without_match(const char *display_name,
                                       struct search_opts *opts,
                                       int *match_count);
static ssize_t read_record(FILE *f, char **buf, size_t *cap, struct search_opts *opts);
static char record_delimiter(const struct search_opts *opts);
static size_t record_match_len(const unsigned char *buf, size_t len, const struct search_opts *opts);
static void write_record_terminator(const struct search_opts *opts);
static int matcher_find_with_opts(struct bx_matcher *m, const unsigned char *buf, size_t len,
                                  size_t start, struct search_opts *opts, struct bx_match *out);

/* --- unified matcher (regex or literal) --- */

enum matcher_kind {
    MATCHER_REGEX,
    MATCHER_POSIX,
    MATCHER_LITERAL,
};

struct bx_matcher {
    enum matcher_kind kind;
    union {
        struct bx_regex *regex;
        struct bx_literal_matcher *literal;
        regex_t posix;
    };
};

static int matcher_find(struct bx_matcher *m, const unsigned char *buf, size_t len,
                        size_t start, struct bx_match *out) {
    if (m->kind == MATCHER_LITERAL)
        return bx_literal_find(m->literal, buf, len, start, out);
    if (m->kind == MATCHER_POSIX) {
        if (start > len)
            return -1;

        size_t chunk_start = start;
        while (chunk_start <= len) {
            const unsigned char *chunk_end = memchr(buf + chunk_start, '\0', len - chunk_start);
            size_t chunk_len = chunk_end ? (size_t)(chunk_end - (buf + chunk_start))
                                         : (len - chunk_start);
            char *chunk = malloc(chunk_len + 1);
            if (!chunk)
                return -1;
            memcpy(chunk, buf + chunk_start, chunk_len);
            chunk[chunk_len] = '\0';

            regmatch_t match = {0};
            int rc = regexec(&m->posix, chunk, 1, &match, 0);
            free(chunk);
            if (rc == 0) {
                if (match.rm_so < 0 || match.rm_eo < 0)
                    return -1;
                out->start = chunk_start + (size_t)match.rm_so;
                out->end = chunk_start + (size_t)match.rm_eo;
                return 0;
            }

            if (!chunk_end)
                break;
            chunk_start += chunk_len + 1;
        }
        return -1;
    }

    return bx_regex_find(m->regex, buf, len, start, out);
}

static bool is_word_byte(unsigned char c) {
    return (c >= '0' && c <= '9') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') ||
           c == '_';
}

static bool match_has_word_boundaries(const unsigned char *buf, size_t len,
                                      const struct bx_match *match) {
    if (match->start > 0 && is_word_byte(buf[match->start - 1]))
        return false;
    if (match->end < len && is_word_byte(buf[match->end]))
        return false;
    return true;
}

static int matcher_find_with_opts(struct bx_matcher *m, const unsigned char *buf, size_t len,
                                  size_t start, struct search_opts *opts, struct bx_match *out) {
    size_t pos = start;
    while (pos <= len) {
        if (matcher_find(m, buf, len, pos, out) != 0)
            return -1;
        if (!opts->word_regexp || match_has_word_boundaries(buf, len, out))
            return 0;
        pos = out->end > out->start ? out->start + 1 : out->start + 1;
    }
    return -1;
}

static void matcher_free(struct bx_matcher *m) {
    if (m->kind == MATCHER_LITERAL)
        bx_literal_free(m->literal);
    else if (m->kind == MATCHER_POSIX)
        regfree(&m->posix);
    else
        bx_regex_free(m->regex);
    free(m);
}

static struct bx_matcher *compile_matcher(const char *pattern,
                                          enum bx_search_personality personality,
                                          struct search_opts *opts,
                                          char **errmsg) {
    char *wrapped = NULL;
    const char *base_pattern = pattern;
    const char *final_pattern = pattern;
    int flags = 0;
    bool use_posix = personality != BX_SEARCH_RG && !opts->perl_regexp;

    if (opts->line_regexp) {
        size_t plen = strlen(base_pattern);
        size_t extra = 1;
        if (opts->line_regexp) extra += 2;
        wrapped = malloc(plen + extra);
        if (!wrapped)
            return NULL;

        char *p = wrapped;
        if (opts->line_regexp) *p++ = '^';
        memcpy(p, base_pattern, plen);
        p += plen;
        if (opts->line_regexp) *p++ = '$';
        *p = '\0';
        final_pattern = wrapped;
    }

    if (opts->ignore_case)
        flags |= BX_REGEX_ICASE;

    if (opts->smart_case && !opts->ignore_case) {
        bool has_upper = false;
        for (const char *c = final_pattern; *c; c++) {
            if (*c >= 'A' && *c <= 'Z') {
                has_upper = true;
                break;
            }
        }
        if (!has_upper)
            flags |= BX_REGEX_ICASE;
    }

    struct bx_matcher *m = calloc(1, sizeof(*m));
    if (!m) {
        free(wrapped);
        return NULL;
    }

    if (opts->fixed_strings) {
        if (bx_literal_compile(&m->literal, final_pattern, (flags & BX_REGEX_ICASE) != 0) != 0) {
            if (errmsg && !*errmsg)
                *errmsg = strdup("empty fixed-string pattern is not supported");
            free(wrapped);
            free(m);
            return NULL;
        }
        m->kind = MATCHER_LITERAL;
    } else if (use_posix) {
        int cflags = 0;
        if (opts->extended_regex)
            cflags |= REG_EXTENDED;
        if (flags & BX_REGEX_ICASE)
            cflags |= REG_ICASE;

        int rc = regcomp(&m->posix, final_pattern, cflags);
        if (rc != 0) {
            if (errmsg && !*errmsg)
                *errmsg = bx_regex_strerror_dup(rc, &m->posix);
            free(wrapped);
            free(m);
            return NULL;
        }
        m->kind = MATCHER_POSIX;
    } else {
        if (bx_regex_compile(&m->regex, final_pattern, flags, errmsg) != 0) {
            free(wrapped);
            free(m);
            return NULL;
        }
        m->kind = MATCHER_REGEX;
    }

    free(wrapped);
    return m;
}

/* --- match output helpers --- */

static void print_match_colored(const unsigned char *line, size_t len,
                                 size_t match_start, size_t match_end,
                                 struct search_opts *opts) {
    if (!opts->only_matching) {
        fwrite(line, 1, match_start, stdout);
        if (bx_color_enabled()) fputs(bx_color_red(), stdout);
        fwrite(line + match_start, 1, match_end - match_start, stdout);
        if (bx_color_enabled()) fputs(bx_color_reset(), stdout);
        fwrite(line + match_end, 1, len - match_end, stdout);
        if (len == 0 || line[len - 1] != record_delimiter(opts))
            write_record_terminator(opts);
    } else {
        fwrite(line + match_start, 1, match_end - match_start, stdout);
        write_record_terminator(opts);
    }
}

static void print_result_prefix(const char *display_name, struct search_opts *opts,
                                int line_num, size_t byte_offset, char sep) {
    if (opts->show_filename && display_name)
        printf("%s%c", display_name, opts->null_filename ? '\0' : sep);
    if (opts->show_line_number)
        printf("%d%c", line_num, sep);
    if (opts->show_byte_offset)
        printf("%zu%c", byte_offset, sep);
}

static void print_only_matches(const unsigned char *line, size_t len,
                               const char *display_name, int line_num,
                               size_t byte_offset,
                               struct bx_matcher *m, struct search_opts *opts) {
    size_t match_len = record_match_len(line, len, opts);
    size_t start = 0;
    while (start <= match_len) {
        struct bx_match bm;
        if (matcher_find_with_opts(m, line, match_len, start, opts, &bm) != 0)
            break;
        print_result_prefix(display_name, opts, line_num, byte_offset + bm.start, ':');
        fwrite(line + bm.start, 1, bm.end - bm.start, stdout);
        write_record_terminator(opts);
        if (bm.end > bm.start)
            start = bm.end;
        else
            start = bm.start + 1;
    }
}

/* --- line buffering for context --- */

struct line_buf {
    char  *text;
    size_t len;
    size_t byte_offset;
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

static char record_delimiter(const struct search_opts *opts) {
    return opts->null_data ? '\0' : '\n';
}

static size_t record_match_len(const unsigned char *buf, size_t len, const struct search_opts *opts) {
    if (len > 0 && buf[len - 1] == (unsigned char)record_delimiter(opts))
        return len - 1;
    return len;
}

static void write_record_terminator(const struct search_opts *opts) {
    putchar((unsigned char)record_delimiter(opts));
}

static ssize_t read_record(FILE *f, char **buf, size_t *cap, struct search_opts *opts) {
    return getdelim(buf, cap, record_delimiter(opts), f);
}

static bool search_default_show_filename(int argc, char **argv, int first_file,
                                         enum bx_search_personality personality,
                                         struct search_opts *opts,
                                         bool rg_searches_stdin) {
    int num_files = argc - first_file;
    if (num_files == 0) {
        if (personality == BX_SEARCH_RG)
            return !rg_searches_stdin;
        return opts->recursive;
    }
    if (num_files > 1)
        return true;
    if (!argv[first_file] || strcmp(argv[first_file], "-") == 0)
        return false;

    struct stat st;
    if (stat(argv[first_file], &st) == 0)
        return S_ISDIR(st.st_mode);
    return false;
}

static int search_file_buffered(const char *filename, const char *display_name,
                                  const char *progname,
                                  struct bx_matcher *m, struct search_opts *opts,
                                int *match_count) {
    FILE *f = stdin;
    bool use_stdin = (!filename || strcmp(filename, "-") == 0);
    if (!use_stdin) {
        f = fopen(filename, "r");
        if (!f) { report_path_error(progname, filename, errno); return 2; }
    }

    int cap = 256;
    struct line_buf *lines = malloc((size_t)cap * sizeof(*lines));
    int nlines = 0;
    char *raw = NULL;
    size_t raw_cap = 0;
    ssize_t len;
    int file_matches = 0;
    int after_left = -1;
    size_t file_offset = 0;
    bool saw_binary = false;

    while ((len = read_record(f, &raw, &raw_cap, opts)) != -1) {
        if (!opts->null_data && memchr(raw, '\0', (size_t)len) != NULL)
            saw_binary = true;
        if (nlines >= cap) { cap *= 2; lines = realloc(lines, (size_t)cap * sizeof(*lines)); }
        lines[nlines].text = malloc((size_t)len + 1);
        memcpy(lines[nlines].text, raw, (size_t)len + 1);
        lines[nlines].len = (size_t)len;
        lines[nlines].byte_offset = file_offset;
        lines[nlines].print = false;
        struct bx_match bm;
        size_t match_len = record_match_len((unsigned char *)raw, (size_t)len, opts);
        bool matched = (matcher_find_with_opts(m, (unsigned char *)raw, match_len, 0, opts, &bm) == 0);
        file_offset += (size_t)len;
        if (opts->invert_match) matched = !matched;
        bool selected = matched;
        if (matched && opts->max_count > 0 && file_matches >= opts->max_count)
            selected = false;
        lines[nlines].match = selected;
        if (selected) {
            file_matches++;
            if (opts->max_count > 0 && file_matches >= opts->max_count) {
                after_left = opts->after_context;
                nlines++;
                if (after_left == 0)
                    break;
                continue;
            }
        } else if (after_left > 0) {
            after_left--;
            nlines++;
            if (after_left == 0)
                break;
            continue;
        }
        nlines++;
    }
    free(raw);
    if (!use_stdin) fclose(f);

    if (saw_binary && !opts->binary_as_text) {
        if (opts->binary_without_match) {
            free_lines(lines, nlines);
            return search_binary_without_match(display_name, opts, match_count);
        }

        if (opts->quiet && file_matches > 0) {
            free_lines(lines, nlines);
            *match_count += file_matches;
            return 0;
        }

        if (opts->count_only || opts->files_with_matches || opts->files_without_match) {
            if (opts->count_only) {
                if (opts->show_filename && display_name)
                    printf("%s%c%d\n", display_name, opts->null_filename ? '\0' : ':', file_matches);
                else printf("%d\n", file_matches);
            }
            if (opts->files_with_matches && file_matches > 0 && display_name) {
                if (opts->null_output) printf("%s%c", display_name, '\0');
                else printf("%s\n", display_name);
            }
            if (opts->files_without_match && file_matches == 0 && display_name) {
                if (opts->null_output) printf("%s%c", display_name, '\0');
                else printf("%s\n", display_name);
            }
            *match_count += file_matches;
            free_lines(lines, nlines);
            return file_matches > 0 ? 0 : 1;
        }

        if (file_matches > 0) {
            report_binary_match(progname, display_name);
            *match_count += file_matches;
            free_lines(lines, nlines);
            return 0;
        }

        free_lines(lines, nlines);
        return 1;
    }

    if (opts->quiet && file_matches > 0) { free_lines(lines, nlines); *match_count += file_matches; return 0; }

    if (opts->count_only || opts->files_with_matches || opts->files_without_match) {
        if (opts->count_only) {
            if (opts->show_filename && display_name)
                printf("%s%c%d\n", display_name, opts->null_filename ? '\0' : ':', file_matches);
            else printf("%d\n", file_matches);
        }
        if (opts->files_with_matches && file_matches > 0 && display_name) {
            if (opts->null_output) printf("%s%c", display_name, '\0');
            else printf("%s\n", display_name);
        }
        if (opts->files_without_match && file_matches == 0 && display_name) {
            if (opts->null_output) printf("%s%c", display_name, '\0');
            else printf("%s\n", display_name);
        }
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
            if (!opts->suppress_group_separator)
                printf("%s\n", opts->group_separator ? opts->group_separator : "--");
        }
        if (lines[i].match) {
            if (opts->only_matching && !opts->invert_match) {
                print_only_matches((unsigned char *)lines[i].text, lines[i].len,
                                   display_name, i + 1, lines[i].byte_offset, m, opts);
            } else {
                print_result_prefix(display_name, opts, i + 1, lines[i].byte_offset, ':');
                if (opts->only_matching && opts->invert_match) {
                    continue;
                }
                if (opts->invert_match) {
                    fwrite(lines[i].text, 1, lines[i].len, stdout);
                    if (lines[i].len == 0 || lines[i].text[lines[i].len - 1] != record_delimiter(opts))
                        write_record_terminator(opts);
                    continue;
                }
                struct bx_match bm;
                matcher_find_with_opts(m, (unsigned char *)lines[i].text,
                                       record_match_len((unsigned char *)lines[i].text, lines[i].len, opts),
                                       0, opts, &bm);
                print_match_colored((unsigned char *)lines[i].text, lines[i].len, bm.start, bm.end, opts);
            }
        } else {
            print_result_prefix(display_name, opts, i + 1, lines[i].byte_offset, '-');
            fwrite(lines[i].text, 1, lines[i].len, stdout);
            if (lines[i].len == 0 || lines[i].text[lines[i].len - 1] != record_delimiter(opts))
                write_record_terminator(opts);
        }
        in_group = true; last_printed = i;
    }
    *match_count += file_matches;
    free_lines(lines, nlines);
    return file_matches > 0 ? 0 : 1;
}

/* --- streaming search (no context) --- */

static int search_file_streaming(const char *filename, const char *display_name,
                                const char *progname,
                                struct bx_matcher *m, struct search_opts *opts,
                                  int *match_count) {
    FILE *f = stdin;
    bool use_stdin = (!filename || strcmp(filename, "-") == 0);
    if (!use_stdin) {
        f = fopen(filename, "r");
        if (!f) { report_path_error(progname, filename, errno); return 2; }
    }

    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    int line_num = 0, file_matches = 0, status = 1;
    size_t file_offset = 0;

    while ((len = read_record(f, &line, &cap, opts)) != -1) {
        size_t line_offset = file_offset;
        file_offset += (size_t)len;
        line_num++;
        struct bx_match bm;
        size_t match_len = record_match_len((unsigned char *)line, (size_t)len, opts);
        bool matched = (matcher_find_with_opts(m, (unsigned char *)line, match_len, 0, opts, &bm) == 0);
        if (opts->invert_match) matched = !matched;
        if (matched) {
            file_matches++; status = 0;
            if (opts->quiet) break;
            if (opts->count_only) {
                if (opts->max_count > 0 && file_matches >= opts->max_count) break;
                continue;
            }
            if (opts->files_with_matches || opts->files_without_match) break;
            if (opts->only_matching && !opts->invert_match) {
                print_only_matches((unsigned char *)line, (size_t)len, display_name, line_num,
                                   line_offset, m, opts);
            } else {
                if (!(opts->only_matching && opts->invert_match)) {
                    print_result_prefix(display_name, opts, line_num, line_offset, ':');
                    if (opts->invert_match) {
                        fwrite(line, 1, (size_t)len, stdout);
                        if (len == 0 || line[len - 1] != record_delimiter(opts))
                            write_record_terminator(opts);
                    } else {
                        print_match_colored((unsigned char *)line, (size_t)len, bm.start, bm.end, opts);
                    }
                }
            }
            if (opts->max_count > 0 && file_matches >= opts->max_count) break;
        }
    }

    if (opts->quiet && file_matches > 0) status = 0;
    if (opts->count_only) {
        if (opts->show_filename && display_name)
            printf("%s%c%d\n", display_name, opts->null_filename ? '\0' : ':', file_matches);
        else printf("%d\n", file_matches);
    }
    if (opts->files_with_matches && file_matches > 0 && display_name) {
        if (opts->null_output) printf("%s%c", display_name, '\0');
        else printf("%s\n", display_name);
    }
    if (opts->files_without_match && file_matches == 0 && display_name) {
        if (opts->null_output) printf("%s%c", display_name, '\0');
        else printf("%s\n", display_name);
    }
    if (match_count) *match_count += file_matches;
    free(line);
    if (!use_stdin) fclose(f);
    return status;
}

static int search_binary_without_match(const char *display_name,
                                       struct search_opts *opts,
                                       int *match_count) {
    if (opts->count_only) {
        if (opts->show_filename && display_name)
            printf("%s%c0\n", display_name, opts->null_filename ? '\0' : ':');
        else
            printf("0\n");
    }
    if (opts->files_without_match && display_name) {
        if (opts->null_output)
            printf("%s%c", display_name, '\0');
        else
            printf("%s\n", display_name);
    }
    if (match_count)
        *match_count += 0;
    return 1;
}

static bool binary_file_matches(const char *filename, struct bx_matcher *m,
                                struct search_opts *opts) {
    FILE *f = fopen(filename, "r");
    if (!f)
        return false;

    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    bool matched = false;

    while ((len = read_record(f, &line, &cap, opts)) != -1) {
        struct bx_match bm;
        matched = matcher_find_with_opts(m, (unsigned char *)line,
                                         record_match_len((unsigned char *)line, (size_t)len, opts),
                                         0, opts, &bm) == 0;
        if (opts->invert_match)
            matched = !matched;
        if (matched)
            break;
    }

    free(line);
    fclose(f);
    return matched;
}

static int search_file(const char *filename, const char *display_name_override, const char *progname,
                       struct bx_matcher *m, struct search_opts *opts,
                       int *match_count) {
    const char *display_name = display_name_for_stream(filename, display_name_override, opts);
    bool use_stdin = (!filename || strcmp(filename, "-") == 0);
    if (display_name && !opts->recursive) {
        struct stat st;
        if (filename && strcmp(filename, "-") != 0 && lstat(filename, &st) == 0 && S_ISDIR(st.st_mode)) {
            report_path_error(progname, filename, EISDIR);
            return 2;
        }
    }

    if (use_stdin && !opts->null_data && !opts->binary_as_text &&
        (opts->binary_without_match ||
         (!opts->quiet && !opts->count_only &&
          !opts->files_with_matches && !opts->files_without_match))) {
        return search_file_buffered(filename, display_name, progname, m, opts, match_count);
    }

    if (!use_stdin && !opts->null_data && !opts->binary_as_text && is_binary(filename)) {
        if (opts->binary_without_match)
            return search_binary_without_match(display_name, opts, match_count);

        if (opts->quiet || opts->files_with_matches || opts->files_without_match || opts->count_only) {
            if (needs_line_buffering(opts))
                return search_file_buffered(filename, display_name, progname, m, opts, match_count);
            return search_file_streaming(filename, display_name, progname, m, opts, match_count);
        }

        if (binary_file_matches(filename, m, opts)) {
            report_binary_match(progname, display_name);
            if (match_count)
                (*match_count)++;
            return 0;
        }
        return 1;
    }

    if (needs_line_buffering(opts))
        return search_file_buffered(filename, display_name, progname, m, opts, match_count);
    return search_file_streaming(filename, display_name, progname, m, opts, match_count);
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

static bool rg_should_search_stdin(void) {
    if (isatty(STDIN_FILENO))
        return false;

    struct stat st;
    if (fstat(STDIN_FILENO, &st) != 0)
        return false;

    if (S_ISCHR(st.st_mode))
        return false;

    struct pollfd pfd = {
        .fd = STDIN_FILENO,
        .events = POLLIN,
    };

    int rc = poll(&pfd, 1, 0);
    if (rc <= 0)
        return false;

    return (pfd.revents & POLLIN) != 0;
}

/* --- recursive search using shared walker --- */

struct grep_walk_state {
    struct bx_matcher *m;
    struct search_opts *opts;
    const char *progname;
    int *match_count;
    int *exit_status;
    bool *match_seen;
    bool *error_seen;
    bool *stop;
    bool strip_dot_prefix;
};

struct files_walk_state {
    struct search_opts *opts;
    bool strip_dot_prefix;
};

static const char *const rg_ignore_filenames[] = {
    ".gitignore",
    ".ignore",
    ".rgignore",
};

static void fs_cb(struct walk_entry *entry, void *user) {
    struct files_walk_state *st = user;
    if (!entry->is_dir)
        printf("%s%c", display_path_for_output(entry->path, st && st->strip_dot_prefix),
               (st && st->opts && st->opts->null_output) ? '\0' : '\n');
}

static void grep_walk_cb(struct walk_entry *entry, void *user) {
    struct grep_walk_state *gs = user;
    if (gs->stop && *gs->stop) return;
    if (entry->is_dir) return;

    const char *name = strrchr(entry->path, '/');
    name = name ? name + 1 : entry->path;

    if (gs->opts->num_include > 0) {
        bool ok = false;
        for (int i = 0; i < gs->opts->num_include; i++)
            if (fnmatch(gs->opts->include_patterns[i], name, 0) == 0) ok = true;
        if (!ok) return;
    }
    if (gs->opts->num_exclude > 0) {
        for (int i = 0; i < gs->opts->num_exclude; i++)
            if (fnmatch(gs->opts->exclude_patterns[i], name, 0) == 0) return;
    }

    const char *display_name = display_path_for_output(entry->path, gs->strip_dot_prefix);

    int r = search_file(entry->path, display_name, gs->progname, gs->m, gs->opts, gs->match_count);
    if (r == 2) {
        *gs->exit_status = 2;
        if (gs->error_seen) *gs->error_seen = true;
    }
    else if (r == 0) {
        *gs->exit_status = 0;
        if (gs->match_seen) *gs->match_seen = true;
        if (gs->opts->quiet && gs->stop) *gs->stop = true;
    }
}

/* --- main entry point --- */

int bx_search_main(int argc, char **argv, enum bx_search_personality personality) {
    struct search_opts opts;
    const char *pattern;
    int first_file;
    const char *progname = argv[0] ? argv[0] : "grep";

    int rc = bx_search_parse_options(argc, argv, &opts, personality, &pattern, &first_file);
    if (rc != 0) {
        bx_search_free_options(&opts);
        return rc == 1 ? 0 : 2;
    }

    if (opts.files_only) {
        bool error_seen = false;
        struct files_walk_state fstate = { .opts = &opts };
        struct walk_opts wopts = {
            .hidden = opts.hidden,
            .no_ignore = opts.no_ignore,
            .no_ignore_parent = opts.no_ignore_parent,
            .no_ignore_vcs = opts.no_ignore_vcs,
            .no_ignore_dot = opts.no_ignore_dot,
            .no_require_git = opts.no_require_git,
            .follow_symlinks = opts.follow_symlinks,
            .follow_root_symlink = true,
            .os_error_style = progname_uses_os_error_style(progname),
            .error_prefix = progname,
            .max_depth = opts.max_depth,
            .ignore_filenames = rg_ignore_filenames,
            .num_ignore_filenames = 3,
            .include_patterns = opts.include_patterns,
            .num_include_patterns = opts.num_include,
            .exclude_dirs = opts.exclude_dir_patterns,
            .num_exclude_dirs = opts.num_exclude_dir,
            .cycle_mode = opts.follow_symlinks ? WALK_CYCLE_SYMLINK_REPEAT : WALK_CYCLE_NONE,
            .cycle_report = WALK_CYCLE_ERROR,
        };
        int num_files = argc - first_file;
        if (num_files == 0) {
            fstate.strip_dot_prefix = true;
            if (walk_dir(".", &wopts, fs_cb, &fstate) != 0)
                error_seen = true;
        } else {
            for (int j = first_file; j < argc; j++) {
                struct stat st;
                if (stat(argv[j], &st) != 0) {
                    report_path_error(progname, argv[j], errno);
                    error_seen = true;
                    continue;
                }
                if (S_ISDIR(st.st_mode))
                    error_seen |= walk_dir(argv[j], &wopts, fs_cb, &fstate) != 0;
                else
                    printf("%s%c", argv[j], opts.null_output ? '\0' : '\n');
            }
        }
        bx_search_free_options(&opts);
        return error_seen ? 2 : 0;
    }

    struct bx_matcher *m;
    char *compile_error = NULL;

    if (opts.num_extra_patterns > 0) {
        bool use_basic_grouping = personality != BX_SEARCH_RG &&
                                  !opts.perl_regexp &&
                                  !opts.extended_regex &&
                                  !opts.fixed_strings;
        const char *group_open = use_basic_grouping ? "\\(" : "(";
        const char *group_close = use_basic_grouping ? "\\)" : ")";
        const char *group_sep = use_basic_grouping ? "\\|" : "|";
        size_t total = strlen(pattern) + strlen(group_open) + strlen(group_close) + 1;
        for (int k = 0; k < opts.num_extra_patterns; k++)
            total += strlen(opts.extra_patterns[k]) + strlen(group_sep);
        char *combined = malloc(total);
        char *p = combined;
        memcpy(p, group_open, strlen(group_open));
        p += strlen(group_open);
        memcpy(p, pattern, strlen(pattern)); p += strlen(pattern);
        for (int k = 0; k < opts.num_extra_patterns; k++) {
            memcpy(p, group_sep, strlen(group_sep));
            p += strlen(group_sep);
            const char *ep = opts.extra_patterns[k];
            size_t elen = strlen(ep);
            memcpy(p, ep, elen); p += elen;
        }
        memcpy(p, group_close, strlen(group_close));
        p += strlen(group_close);
        *p = '\0';
        m = compile_matcher(combined, personality, &opts, &compile_error);
        free(combined);
    } else {
        m = compile_matcher(pattern, personality, &opts, &compile_error);
    }

    if (!m) {
        if (compile_error) {
            fprintf(stderr, "%s: invalid pattern '%s': %s\n",
                    argv[0] ? argv[0] : "grep", pattern, compile_error);
            free(compile_error);
        } else {
            fprintf(stderr, "%s: invalid pattern: %s\n",
                    argv[0] ? argv[0] : "grep", pattern);
        }
        bx_search_free_options(&opts);
        return 2;
    }

    int num_files = argc - first_file;
    bool rg_searches_stdin = (personality == BX_SEARCH_RG && num_files == 0 && rg_should_search_stdin());
    if (!opts.show_filename && !opts.hide_filename)
        opts.show_filename = search_default_show_filename(argc, argv, first_file, personality,
                                                          &opts, rg_searches_stdin);
    if (opts.hide_filename)
        opts.show_filename = false;

    int global_matches = 0;
    int exit_status = 1;
    bool match_seen = false;
    bool error_seen = false;

    if (num_files == 0) {
        if ((personality == BX_SEARCH_RG && !rg_searches_stdin) ||
            (personality != BX_SEARCH_RG && opts.recursive)) {
            bool stop = false;
            struct grep_walk_state gs = {.m = m, .opts = &opts,
                                         .progname = progname,
                                         .match_count = &global_matches,
                                         .exit_status = &exit_status,
                                         .match_seen = &match_seen,
                                         .error_seen = &error_seen,
                                         .stop = &stop,
                                         .strip_dot_prefix = true};
            struct walk_opts wopts = {
                .hidden = opts.hidden,
                .no_ignore = opts.no_ignore,
                .no_ignore_parent = opts.no_ignore_parent,
                .no_ignore_vcs = opts.no_ignore_vcs,
                .no_ignore_dot = opts.no_ignore_dot,
                .no_require_git = opts.no_require_git,
                .follow_symlinks = opts.follow_symlinks,
                .follow_root_symlink = true,
                .stop = &stop,
                .os_error_style = progname_uses_os_error_style(progname),
                .error_prefix = progname,
                .max_depth = opts.max_depth,
                .ignore_filenames = rg_ignore_filenames,
                .num_ignore_filenames = 3,
                .include_patterns = opts.include_patterns,
                .num_include_patterns = opts.num_include,
                .exclude_dirs = opts.exclude_dir_patterns,
                .num_exclude_dirs = opts.num_exclude_dir,
                .cycle_mode = opts.follow_symlinks
                                  ? ((personality == BX_SEARCH_RG)
                                         ? WALK_CYCLE_SYMLINK_REPEAT
                                         : WALK_CYCLE_DIR_REPEAT)
                                  : WALK_CYCLE_NONE,
                .cycle_report = opts.follow_symlinks
                                    ? ((personality == BX_SEARCH_RG)
                                           ? WALK_CYCLE_ERROR
                                           : WALK_CYCLE_WARN)
                                    : WALK_CYCLE_IGNORE,
            };

            if (walk_dir(".", &wopts, grep_walk_cb, &gs) != 0) {
                exit_status = 2;
                error_seen = true;
            }
        } else {
            exit_status = search_file(NULL, NULL, progname, m, &opts, &global_matches);
            if (exit_status == 0) match_seen = true;
            else if (exit_status == 2) error_seen = true;
        }
    } else if (opts.recursive) {
        bool stop = false;
        struct grep_walk_state gs = {.m = m, .opts = &opts,
                                     .progname = progname,
                                     .match_count = &global_matches,
                                     .exit_status = &exit_status,
                                     .match_seen = &match_seen,
                                     .error_seen = &error_seen,
                                     .stop = &stop,
                                     .strip_dot_prefix = false};
        struct walk_opts wopts = {
            .hidden = opts.hidden,
            .no_ignore = opts.no_ignore,
            .no_ignore_parent = opts.no_ignore_parent,
            .no_ignore_vcs = opts.no_ignore_vcs,
            .no_ignore_dot = opts.no_ignore_dot,
            .no_require_git = opts.no_require_git,
            .follow_symlinks = opts.follow_symlinks,
            .follow_root_symlink = true,
            .stop = &stop,
            .os_error_style = progname_uses_os_error_style(progname),
            .error_prefix = progname,
            .max_depth = opts.max_depth,
            .ignore_filenames = rg_ignore_filenames,
            .num_ignore_filenames = 3,
            .include_patterns = opts.include_patterns,
            .num_include_patterns = opts.num_include,
            .exclude_dirs = opts.exclude_dir_patterns,
            .num_exclude_dirs = opts.num_exclude_dir,
            .cycle_mode = opts.follow_symlinks
                              ? ((personality == BX_SEARCH_RG)
                                     ? WALK_CYCLE_SYMLINK_REPEAT
                                     : WALK_CYCLE_DIR_REPEAT)
                              : WALK_CYCLE_NONE,
            .cycle_report = opts.follow_symlinks
                                ? ((personality == BX_SEARCH_RG)
                                       ? WALK_CYCLE_ERROR
                                       : WALK_CYCLE_WARN)
                                : WALK_CYCLE_IGNORE,
        };

        for (int j = first_file; j < argc && !stop; j++) {
            struct stat st;
            if (stat(argv[j], &st) != 0) {
                report_path_error(progname, argv[j], errno);
                exit_status = 2;
                error_seen = true;
                continue;
            }
            if (S_ISDIR(st.st_mode)) {
                if (walk_dir(argv[j], &wopts, grep_walk_cb, &gs) != 0) {
                    exit_status = 2;
                    error_seen = true;
                }
            } else if (S_ISREG(st.st_mode)) {
                grep_walk_cb(&(struct walk_entry){.path = argv[j], .is_dir = false, .mode = st.st_mode}, &gs);
            }
        }
    } else {
        for (int j = first_file; j < argc; j++) {
            if (argv[j] && strcmp(argv[j], "-") != 0) {
                struct stat st;
                if (lstat(argv[j], &st) == 0 && S_ISDIR(st.st_mode)) {
                    if (opts.directory_mode == BX_GREP_DIR_SKIP)
                        continue;
                    report_path_error(progname, argv[j], EISDIR);
                    exit_status = 2;
                    error_seen = true;
                    continue;
                }
            }
            int r = search_file(argv[j], NULL, progname, m, &opts, &global_matches);
            if (r == 2) {
                exit_status = 2;
                error_seen = true;
            } else if (r == 0) {
                exit_status = 0;
                match_seen = true;
            }
        }
    }

    matcher_free(m);
    bx_search_free_options(&opts);
    if (opts.quiet && match_seen)
        return 0;
    if (error_seen)
        return 2;
    return match_seen ? 0 : 1;
}
