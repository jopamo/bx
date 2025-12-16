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

#include "lib/cli_common.h"
#include "lib/path_ops.h"
#include "search.h"
#include "options.h"
#include "fswalk/walk.h"
#include "filter.h"
#include "pcre2_matcher.h"
#include "literal.h"
#include "record_stream.h"
#include "scanner.h"
#include "traverse.h"
#include "lib/color.h"
#include "bx/diag.h"

static bool progname_uses_os_error_style(const char *progname) {
    if (!progname) return false;
    progname = bx_cli_progname(progname, "grep");
    return strcmp(progname, "rg") == 0 || strcmp(progname, "bxrg") == 0;
}

static bool bx_search_personality_is_rg(enum bx_search_personality personality) {
    return personality == BX_SEARCH_RG;
}

static bool bx_search_use_rg_sort_policy(enum bx_search_personality personality,
                                         const struct search_opts *opts) {
    return !bx_search_personality_is_rg(personality)
        || opts->sort_paths
        || opts->sort_paths_reverse;
}

static int bx_search_cycle_mode(enum bx_search_personality personality,
                                const struct search_opts *opts) {
    if (!opts->follow_symlinks)
        return BX_WALK_CYCLE_NONE;
    return bx_search_personality_is_rg(personality)
        ? BX_WALK_CYCLE_SYMLINK_REPEAT
        : BX_WALK_CYCLE_DIR_REPEAT;
}

static int bx_search_cycle_report(enum bx_search_personality personality,
                                  const struct search_opts *opts) {
    if (!opts->follow_symlinks)
        return BX_WALK_CYCLE_IGNORE;
    return bx_search_personality_is_rg(personality)
        ? BX_WALK_CYCLE_ERROR
        : BX_WALK_CYCLE_WARN;
}

static char *bx_regex_strerror_dup(int rc, const regex_t *regex) {
    size_t needed = regerror(rc, regex, NULL, 0);
    char *buf = malloc(needed > 0 ? needed : 1);
    if (!buf)
        return NULL;
    regerror(rc, regex, buf, needed > 0 ? needed : 1);
    return buf;
}

static void report_path_error(const char *progname, const char *path, int errnum,
                              const struct search_opts *opts) {
    if (opts && opts->suppress_errors)
        return;

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
    if (strip_dot_prefix)
        return bx_path_strip_dot_slash_prefix_ptr(path);
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

struct bx_search_stats {
    int matches;
    int matched_lines;
    int files_with_matches;
    int files_searched;
    size_t bytes_printed;
    size_t bytes_searched;
};

static struct bx_search_stats *current_stats = NULL;

static bool is_binary(const char *path);
static int search_binary_without_match(const char *display_name,
                                       struct search_opts *opts,
                                       int *match_count,
                                       struct bx_search_stats *stats);
static int search_file_streaming_opened(FILE *f,
                                        bool use_stdin,
                                        const char *display_name,
                                        struct bx_matcher *m,
                                        struct search_opts *opts,
                                        int *match_count,
                                        struct bx_record_stream *record_stream,
                                        struct bx_search_stats *stats);
static ssize_t read_record(FILE *f, struct bx_record_stream *stream, struct search_opts *opts);
static char record_delimiter(const struct search_opts *opts);
static size_t record_match_len(const unsigned char *buf, size_t len, const struct search_opts *opts);
static void write_record_terminator(const struct search_opts *opts);
static int matcher_find_with_opts(struct bx_matcher *m, const unsigned char *buf, size_t len,
                                  size_t start, struct search_opts *opts, struct bx_match *out);
static void stats_count_bytes(size_t count);

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

static bool rg_pattern_requires_pcre2(const char *pattern, const struct search_opts *opts) {
    if (!pattern)
        return false;

    if (opts && (opts->multiline || opts->multiline_dotall))
        return true;

    for (const char *p = pattern; *p; ++p) {
        if (*p == '\\') {
            ++p;
            if (!*p)
                break;
            if (*p >= '1' && *p <= '9')
                return true;
            if (*p == 'g' || *p == 'k')
                return true;
            continue;
        }

        if (*p != '(' || p[1] != '?')
            continue;

        if (p[2] == '=' || p[2] == '!' || p[2] == '>')
            return true;
        if (p[2] == '<' && (p[3] == '=' || p[3] == '!'))
            return true;
        if (p[2] == '(' || p[2] == 'R' || p[2] == '&')
            return true;
    }

    return false;
}

static bool pattern_is_plain_literal(const char *pattern) {
    if (!pattern || !*pattern)
        return false;

    for (const unsigned char *p = (const unsigned char *)pattern; *p; ++p) {
        switch (*p) {
        case '\\':
        case '.':
        case '^':
        case '$':
        case '*':
        case '+':
        case '?':
        case '(':
        case ')':
        case '[':
        case ']':
        case '{':
        case '}':
        case '|':
            return false;
        default:
            break;
        }
    }

    return true;
}

static bool matcher_uses_posix(const char *pattern,
                               enum bx_search_personality personality,
                               const struct search_opts *opts) {
    if (opts->fixed_strings)
        return false;

    if (personality != BX_SEARCH_RG)
        return !opts->perl_regexp;

    switch (opts->rg_engine) {
    case BX_RG_ENGINE_DEFAULT:
        return true;
    case BX_RG_ENGINE_AUTO:
        return !rg_pattern_requires_pcre2(pattern, opts);
    case BX_RG_ENGINE_PCRE2:
    case BX_RG_ENGINE_UNSPECIFIED:
    default:
        return false;
    }
}

static int matcher_find(struct bx_matcher *m, const unsigned char *buf, size_t len,
                        size_t start, struct bx_match *out) {
    if (m->kind == MATCHER_LITERAL)
        return bx_literal_find(m->literal, buf, len, start, out);
    if (m->kind == MATCHER_POSIX) {
        if (start > len)
            return -1;

        const unsigned char *remaining = buf + start;
        size_t remaining_len = len - start;
        if (memchr(remaining, '\0', remaining_len) == NULL) {
            regmatch_t match = {0};
            int rc = regexec(&m->posix, (const char *)remaining, 1, &match, 0);
            if (rc != 0)
                return -1;
            if (match.rm_so < 0 || match.rm_eo < 0)
                return -1;
            out->start = start + (size_t)match.rm_so;
            out->end = start + (size_t)match.rm_eo;
            return 0;
        }

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

static bool matcher_verify_literal_candidate_with_opts(struct bx_matcher *m,
                                                       const unsigned char *buf,
                                                       size_t len,
                                                       size_t candidate_start,
                                                       struct search_opts *opts,
                                                       struct bx_match *out) {
    if (!m || m->kind != MATCHER_LITERAL || !opts || opts->line_regexp)
        return false;
    if (!bx_literal_verify_at(m->literal, buf, len, candidate_start, out))
        return false;
    if (opts->word_regexp && !match_has_word_boundaries(buf, len, out))
        return false;
    return true;
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
    bool use_posix = matcher_uses_posix(pattern, personality, opts);

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

    if (opts->multiline)
        flags |= BX_REGEX_MULTILINE;
    if (opts->multiline_dotall)
        flags |= BX_REGEX_DOTALL;

    struct bx_matcher *m = calloc(1, sizeof(*m));
    if (!m) {
        free(wrapped);
        return NULL;
    }

    if (opts->fixed_strings ||
        (!opts->line_regexp && pattern_is_plain_literal(final_pattern))) {
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
        if (bx_search_personality_is_rg(personality) || opts->extended_regex)
            cflags |= REG_EXTENDED;
        if (!bx_search_personality_is_rg(personality))
            cflags |= REG_NEWLINE;
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

static bool matcher_is_scanner_literal_eligible(const struct bx_matcher *m,
                                                const struct search_opts *opts) {
    if (!m || !opts || m->kind != MATCHER_LITERAL)
        return false;
    return !bx_literal_contains_byte(m->literal, (unsigned char)record_delimiter(opts));
}

/* --- match output helpers --- */

static void print_match_colored(const unsigned char *line, size_t len,
                                 size_t match_start, size_t match_end,
                                 struct search_opts *opts) {
    if (!opts->only_matching) {
        fwrite(line, 1, match_start, stdout);
        stats_count_bytes(match_start);
        if (bx_color_enabled()) fputs(bx_color_red(), stdout);
        fwrite(line + match_start, 1, match_end - match_start, stdout);
        stats_count_bytes(match_end - match_start);
        if (bx_color_enabled()) fputs(bx_color_reset(), stdout);
        fwrite(line + match_end, 1, len - match_end, stdout);
        stats_count_bytes(len - match_end);
        if (len == 0 || line[len - 1] != record_delimiter(opts))
            write_record_terminator(opts);
    } else {
        fwrite(line + match_start, 1, match_end - match_start, stdout);
        stats_count_bytes(match_end - match_start);
        write_record_terminator(opts);
    }
}

static void print_replacement_piece(const char *replace, const unsigned char *match, size_t match_len) {
    if (!replace) {
        fwrite(match, 1, match_len, stdout);
        stats_count_bytes(match_len);
        return;
    }

    for (const char *p = replace; *p; ++p) {
        if (p[0] == '$' && p[1] == '0') {
            fwrite(match, 1, match_len, stdout);
            stats_count_bytes(match_len);
            ++p;
            continue;
        }
        putchar((unsigned char)*p);
        stats_count_bytes(1);
    }
}

static void print_replaced_record(const unsigned char *line, size_t len,
                                  struct bx_matcher *m, struct search_opts *opts) {
    size_t match_len = record_match_len(line, len, opts);
    size_t start = 0;
    size_t cursor = 0;

    while (start <= match_len) {
        struct bx_match bm;
        if (matcher_find_with_opts(m, line, match_len, start, opts, &bm) != 0)
            break;
        fwrite(line + cursor, 1, bm.start - cursor, stdout);
        stats_count_bytes(bm.start - cursor);
        print_replacement_piece(opts->replace, line + bm.start, bm.end - bm.start);
        cursor = bm.end;
        start = bm.end > bm.start ? bm.end : bm.start + 1;
    }

    fwrite(line + cursor, 1, match_len - cursor, stdout);
    stats_count_bytes(match_len - cursor);
    write_record_terminator(opts);
}

static bool should_omit_long_match_line(const struct search_opts *opts, size_t record_len) {
    return opts->max_columns > 0 && !opts->only_matching && (int)record_len > opts->max_columns;
}

static void print_omitted_long_line(struct search_opts *opts) {
    fputs("[Omitted long matching line]", stdout);
    stats_count_bytes(strlen("[Omitted long matching line]"));
    write_record_terminator(opts);
}

static const char *match_field_separator(struct search_opts *opts) {
    return opts->field_match_separator ? opts->field_match_separator : ":";
}

static const char *context_field_separator(struct search_opts *opts) {
    return opts->field_context_separator ? opts->field_context_separator : "-";
}

static void print_result_prefix(const char *display_name, struct search_opts *opts,
                                int line_num, size_t column, bool has_column,
                                size_t byte_offset, const char *sep) {
    if (opts->show_filename && display_name) {
        if (bx_color_enabled())
            fputs(bx_color_magenta(), stdout);
        fputs(display_name, stdout);
        stats_count_bytes(strlen(display_name));
        if (bx_color_enabled())
            fputs(bx_color_reset(), stdout);
        if (opts->null_filename)
            putchar('\0');
        else
            fputs(sep, stdout);
        stats_count_bytes(opts->null_filename ? 1 : strlen(sep));
    }
    if (opts->show_line_number) {
        if (bx_color_enabled())
            fputs(bx_color_green(), stdout);
        int n = printf("%d", line_num);
        if (n > 0) stats_count_bytes((size_t)n);
        if (bx_color_enabled())
            fputs(bx_color_reset(), stdout);
        fputs(sep, stdout);
        stats_count_bytes(strlen(sep));
    }
    if (opts->show_column && has_column) {
        if (bx_color_enabled())
            fputs(bx_color_green(), stdout);
        int n = printf("%zu", column);
        if (n > 0) stats_count_bytes((size_t)n);
        if (bx_color_enabled())
            fputs(bx_color_reset(), stdout);
        fputs(sep, stdout);
        stats_count_bytes(strlen(sep));
    }
    if (opts->show_byte_offset) {
        if (bx_color_enabled())
            fputs(bx_color_green(), stdout);
        int n = printf("%zu", byte_offset);
        if (n > 0) stats_count_bytes((size_t)n);
        if (bx_color_enabled())
            fputs(bx_color_reset(), stdout);
        fputs(sep, stdout);
        stats_count_bytes(strlen(sep));
    }
}

static bool use_heading_output(const char *display_name, const struct search_opts *opts) {
    return opts->heading && opts->show_filename && display_name && display_name[0] != '\0';
}

static void maybe_print_heading(const char *display_name, struct search_opts *opts,
                                bool *heading_printed_for_file) {
    if (!use_heading_output(display_name, opts) || *heading_printed_for_file)
        return;
    if (opts->heading_output_started)
        putchar('\n');
    if (bx_color_enabled())
        fputs(bx_color_magenta(), stdout);
    printf("%s", display_name);
    if (bx_color_enabled())
        fputs(bx_color_reset(), stdout);
    putchar('\n');
    *heading_printed_for_file = true;
    opts->heading_output_started = true;
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
        print_result_prefix(display_name, opts, line_num, bm.start + 1, true,
                            byte_offset + bm.start,
                            match_field_separator(opts));
        fwrite(line + bm.start, 1, bm.end - bm.start, stdout);
        stats_count_bytes(bm.end - bm.start);
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
    int    match_count;
};

static void free_lines(struct line_buf *lines, int count) {
    for (int i = 0; i < count; i++) free(lines[i].text);
    free(lines);
}

static bool needs_line_buffering(const struct search_opts *opts) {
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
    stats_count_bytes(1);
}

static void print_count_result(const char *display_name, struct search_opts *opts, int file_matches) {
    if (opts->omit_zero_count_output && file_matches == 0)
        return;
    if (opts->show_filename && display_name)
        printf("%s%c%d\n", display_name, opts->null_filename ? '\0' : ':', file_matches);
    else
        printf("%d\n", file_matches);
}

static void print_stats_summary(struct bx_search_stats *stats) {
    printf("\n%d matches\n", stats->matches);
    printf("%d matched lines\n", stats->matched_lines);
    printf("%d files contained matches\n", stats->files_with_matches);
    printf("%d files searched\n", stats->files_searched);
    printf("%zu bytes printed\n", stats->bytes_printed);
    printf("%zu bytes searched\n", stats->bytes_searched);
    printf("0.000000 seconds spent searching\n");
    printf("0.000000 seconds total\n");
}

static void stats_count_bytes(size_t count) {
    if (current_stats)
        current_stats->bytes_printed += count;
}

static int count_record_matches(struct bx_matcher *m, const unsigned char *buf, size_t len,
                                struct search_opts *opts) {
    size_t start = 0;
    int count = 0;
    while (start <= len) {
        struct bx_match bm;
        if (matcher_find_with_opts(m, buf, len, start, opts, &bm) != 0)
            break;
        count++;
        if (bm.end > bm.start)
            start = bm.end;
        else
            start = bm.start + 1;
    }
    return count;
}

static FILE *open_search_input_stream(const char *filename,
                                      const char *progname,
                                      struct search_opts *opts,
                                      struct bx_record_stream *stream,
                                      bool *use_stdin_out) {
    bool use_stdin = (!filename || strcmp(filename, "-") == 0);
    if (use_stdin) {
        if (use_stdin_out)
            *use_stdin_out = true;
        return stdin;
    }

    FILE *f = fopen(filename, "r");
    if (!f) {
        report_path_error(progname, filename, errno, opts);
        return NULL;
    }

    bx_record_stream_prepare_file(f, stream);
    if (use_stdin_out)
        *use_stdin_out = false;
    return f;
}

static ssize_t read_record(FILE *f, struct bx_record_stream *stream, struct search_opts *opts) {
    return bx_record_stream_read(f, stream, record_delimiter(opts));
}

static unsigned char *read_stream_all(FILE *f, size_t *out_len) {
    size_t cap = 4096;
    size_t len = 0;
    unsigned char *buf = malloc(cap + 1);
    if (!buf)
        return NULL;

    for (;;) {
        if (len == cap) {
            size_t new_cap = cap * 2;
            unsigned char *tmp = realloc(buf, new_cap + 1);
            if (!tmp) {
                free(buf);
                return NULL;
            }
            buf = tmp;
            cap = new_cap;
        }

        size_t nread = fread(buf + len, 1, cap - len, f);
        len += nread;
        if (nread == 0)
            break;
    }

    if (ferror(f)) {
        free(buf);
        return NULL;
    }

    buf[len] = '\0';
    if (out_len)
        *out_len = len;
    return buf;
}

static size_t line_number_for_offset(const unsigned char *buf, size_t offset) {
    size_t line = 1;
    for (size_t i = 0; i < offset; i++) {
        if (buf[i] == '\n')
            line++;
    }
    return line;
}

static size_t line_start_offset(const unsigned char *buf, size_t offset) {
    while (offset > 0 && buf[offset - 1] != '\n')
        offset--;
    return offset;
}

static size_t column_number_for_offset(const unsigned char *buf, size_t offset) {
    return offset - line_start_offset(buf, offset) + 1;
}

static size_t line_end_offset(const unsigned char *buf, size_t len, size_t offset) {
    while (offset < len && buf[offset] != '\n')
        offset++;
    if (offset < len)
        offset++;
    return offset;
}

static int search_file_multiline(const char *filename, const char *display_name,
                                 const char *progname, struct bx_matcher *m,
                                 struct search_opts *opts, int *match_count,
                                 struct bx_search_stats *stats) {
    FILE *f = stdin;
    bool use_stdin = (!filename || strcmp(filename, "-") == 0);
    if (!use_stdin) {
        f = fopen(filename, "r");
        if (!f) {
            report_path_error(progname, filename, errno, opts);
            return 2;
        }
    }

    size_t len = 0;
    unsigned char *buf = read_stream_all(f, &len);
    if (!use_stdin)
        fclose(f);
    if (!buf)
        return 2;
    if (stats) {
        stats->files_searched++;
        stats->bytes_searched += len;
    }

    size_t start = 0;
    int file_matches = 0;
    int status = 1;
    bool heading_printed_for_file = false;

    while (start <= len) {
        struct bx_match bm;
        if (matcher_find_with_opts(m, buf, len, start, opts, &bm) != 0)
            break;

        file_matches++;
        status = 0;
        if (stats) {
            stats->matches++;
            stats->matched_lines++;
        }

        if (opts->quiet)
            break;
        if (opts->count_only) {
            start = bm.end > bm.start ? bm.end : bm.start + 1;
            if (opts->max_count > 0 && file_matches >= opts->max_count)
                break;
            continue;
        }
        if (opts->files_with_matches || opts->files_without_match) {
            if (!opts->stats)
                break;
            start = bm.end > bm.start ? bm.end : bm.start + 1;
            if (opts->max_count > 0 && file_matches >= opts->max_count)
                break;
            continue;
        }

        size_t line_num = line_number_for_offset(buf, bm.start);
        if (opts->only_matching && !opts->invert_match) {
            maybe_print_heading(display_name, opts, &heading_printed_for_file);
            print_result_prefix(heading_printed_for_file ? NULL : display_name,
                                opts, (int)line_num, column_number_for_offset(buf, bm.start), true,
                                bm.start, match_field_separator(opts));
            fwrite(buf + bm.start, 1, bm.end - bm.start, stdout);
            write_record_terminator(opts);
        } else {
            size_t out_start = line_start_offset(buf, bm.start);
            size_t out_end = line_end_offset(buf, len, bm.end);
            maybe_print_heading(display_name, opts, &heading_printed_for_file);
            print_result_prefix(heading_printed_for_file ? NULL : display_name,
                                opts, (int)line_number_for_offset(buf, out_start),
                                column_number_for_offset(buf, bm.start), true,
                                out_start, match_field_separator(opts));
            if (should_omit_long_match_line(opts, out_end - out_start))
                print_omitted_long_line(opts);
            else if (opts->replace) {
                print_replaced_record(buf + out_start, out_end - out_start, m, opts);
            } else {
                fwrite(buf + out_start, 1, out_end - out_start, stdout);
                if (out_end == out_start || buf[out_end - 1] != record_delimiter(opts))
                    write_record_terminator(opts);
            }
        }

        if (opts->max_count > 0 && file_matches >= opts->max_count)
            break;
        start = bm.end > bm.start ? bm.end : bm.start + 1;
    }

        if (opts->count_only)
            print_count_result(display_name, opts, file_matches);
    if (opts->files_with_matches && file_matches > 0 && display_name) {
        if (opts->null_output)
            printf("%s%c", display_name, '\0');
        else
            printf("%s\n", display_name);
    }
    if (opts->files_without_match && file_matches == 0 && display_name) {
        if (opts->null_output)
            printf("%s%c", display_name, '\0');
        else
            printf("%s\n", display_name);
    }

    if (stats && file_matches > 0)
        stats->files_with_matches++;
    if (match_count)
        *match_count += file_matches;
    free(buf);
    return status;
}

static bool search_default_show_filename(int argc, char **argv, int first_file,
                                         enum bx_search_personality personality,
                                         struct search_opts *opts,
                                         bool rg_searches_stdin) {
    int num_files = argc - first_file;
    if (num_files == 0) {
        if (bx_search_personality_is_rg(personality))
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

static bool search_default_heading(enum bx_search_personality personality,
                                   const struct search_opts *opts) {
    if (!bx_search_personality_is_rg(personality) || !isatty(STDOUT_FILENO))
        return false;
    return opts->show_filename;
}

#define BX_SEARCH_SCANNER_MIN_FILE_SIZE 65536u

static bool search_file_scanner_stream_is_eligible(FILE *f) {
    if (!f)
        return false;

    int fd = fileno(f);
    if (fd < 0)
        return false;

    struct stat st;
    if (fstat(fd, &st) != 0)
        return false;
    if (!S_ISREG(st.st_mode))
        return false;
    if (st.st_size < (off_t)BX_SEARCH_SCANNER_MIN_FILE_SIZE)
        return false;
    return true;
}

static bool search_file_can_use_scanner(const struct bx_matcher *m,
                                        const struct search_opts *opts,
                                        bool use_stdin) {
    if (!m || !opts || use_stdin)
        return false;
    if (!opts->recursive || opts->multiline || opts->invert_match)
        return false;
    if (needs_line_buffering(opts) || opts->replace || opts->only_matching || opts->passthru)
        return false;
    if (opts->stop_on_nonmatch)
        return false;
    if (bx_color_enabled())
        return false;
    return matcher_is_scanner_literal_eligible(m, opts);
}

static bool search_file_scanner_can_shortcut_file_presence(const struct search_opts *opts) {
    if (!opts || opts->count_matches)
        return false;
    if (opts->quiet)
        return true;
    if (opts->count_only)
        return false;
    return (opts->files_with_matches || opts->files_without_match) && !opts->stats;
}

static int search_file_buffered_opened(FILE *f,
                                       bool use_stdin,
                                       const char *display_name,
                                       const char *progname,
                                       struct bx_matcher *m,
                                       struct search_opts *opts,
                                       int *match_count,
                                       struct bx_record_stream *record_stream,
                                       struct bx_search_stats *stats) {
    int cap = 256;
    struct line_buf *lines = malloc((size_t)cap * sizeof(*lines));
    int nlines = 0;
    ssize_t len;
    int file_matches = 0;
    int after_left = -1;
    size_t file_offset = 0;
    bool saw_binary = false;
    bool saw_match_record = false;
    bool heading_printed_for_file = false;

    while ((len = read_record(f, record_stream, opts)) != -1) {
        char *raw = record_stream->record;
        if (!opts->null_data && memchr(raw, '\0', (size_t)len) != NULL)
            saw_binary = true;
        if (nlines >= cap) { cap *= 2; lines = realloc(lines, (size_t)cap * sizeof(*lines)); }
        lines[nlines].text = malloc((size_t)len + 1);
        memcpy(lines[nlines].text, raw, (size_t)len + 1);
        lines[nlines].len = (size_t)len;
        lines[nlines].byte_offset = file_offset;
        lines[nlines].print = false;
        if (stats)
            stats->bytes_searched += (size_t)len;
        struct bx_match bm;
        size_t match_len = record_match_len((unsigned char *)raw, (size_t)len, opts);
        bool matched = (matcher_find_with_opts(m, (unsigned char *)raw, match_len, 0, opts, &bm) == 0);
        file_offset += (size_t)len;
        if (opts->invert_match) matched = !matched;
        bool selected = matched;
        int record_match_count = 0;
        if (matched && !opts->invert_match)
            record_match_count = opts->count_matches
                                     ? count_record_matches(m, (unsigned char *)raw, match_len, opts)
                                     : 1;
        if (matched && opts->max_count > 0 && file_matches >= opts->max_count)
            selected = false;
        lines[nlines].match = selected;
        lines[nlines].match_count = record_match_count;
        if (selected) {
            file_matches += opts->count_matches ? record_match_count : 1;
            if (stats) {
                stats->matches += opts->count_matches ? record_match_count : 1;
                stats->matched_lines++;
            }
            saw_match_record = true;
            if (opts->max_count > 0 && file_matches >= opts->max_count) {
                after_left = opts->after_context;
                nlines++;
                if (after_left == 0)
                    break;
                continue;
            }
        } else if (opts->stop_on_nonmatch && saw_match_record) {
            free(lines[nlines].text);
            lines[nlines].text = NULL;
            break;
        } else if (after_left > 0) {
            after_left--;
            nlines++;
            if (after_left == 0)
                break;
            continue;
        }
        nlines++;
    }
    if (!use_stdin) fclose(f);
    if (stats)
        stats->files_searched++;

    if (saw_binary && !opts->binary_as_text) {
        if (opts->binary_without_match) {
            free_lines(lines, nlines);
            return search_binary_without_match(display_name, opts, match_count, stats);
        }

        if (opts->quiet && file_matches > 0) {
            if (stats && file_matches > 0)
                stats->files_with_matches++;
            free_lines(lines, nlines);
            *match_count += file_matches;
            return 0;
        }

        if (opts->count_only || opts->files_with_matches || opts->files_without_match) {
            if (opts->count_only)
                print_count_result(display_name, opts, file_matches);
            if (opts->files_with_matches && file_matches > 0 && display_name) {
                if (opts->null_output) printf("%s%c", display_name, '\0');
                else printf("%s\n", display_name);
            }
            if (opts->files_without_match && file_matches == 0 && display_name) {
                if (opts->null_output) printf("%s%c", display_name, '\0');
                else printf("%s\n", display_name);
            }
            if (stats && file_matches > 0)
                stats->files_with_matches++;
            *match_count += file_matches;
            free_lines(lines, nlines);
            return file_matches > 0 ? 0 : 1;
        }

        if (file_matches > 0) {
            report_binary_match(progname, display_name);
            if (stats)
                stats->files_with_matches++;
            *match_count += file_matches;
            free_lines(lines, nlines);
            return 0;
        }

        free_lines(lines, nlines);
        return 1;
    }

    if (opts->quiet && file_matches > 0) {
        if (stats)
            stats->files_with_matches++;
        free_lines(lines, nlines);
        *match_count += file_matches;
        return 0;
    }

    if (opts->count_only || opts->files_with_matches || opts->files_without_match) {
        if (opts->count_only)
            print_count_result(display_name, opts, file_matches);
        if (opts->files_with_matches && file_matches > 0 && display_name) {
            if (opts->null_output) printf("%s%c", display_name, '\0');
            else printf("%s\n", display_name);
        }
        if (opts->files_without_match && file_matches == 0 && display_name) {
            if (opts->null_output) printf("%s%c", display_name, '\0');
            else printf("%s\n", display_name);
        }
        if (stats && file_matches > 0)
            stats->files_with_matches++;
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
                maybe_print_heading(display_name, opts, &heading_printed_for_file);
                print_only_matches((unsigned char *)lines[i].text, lines[i].len,
                                   heading_printed_for_file ? NULL : display_name,
                                   i + 1, lines[i].byte_offset, m, opts);
            } else {
                struct bx_match bm;
                matcher_find_with_opts(m, (unsigned char *)lines[i].text,
                                       record_match_len((unsigned char *)lines[i].text, lines[i].len, opts),
                                       0, opts, &bm);
                maybe_print_heading(display_name, opts, &heading_printed_for_file);
                print_result_prefix(heading_printed_for_file ? NULL : display_name,
                                    opts, i + 1, bm.start + 1, true, lines[i].byte_offset,
                                    match_field_separator(opts));
                if (opts->only_matching && opts->invert_match) {
                    continue;
                }
                if (opts->invert_match) {
                    fwrite(lines[i].text, 1, lines[i].len, stdout);
                    if (lines[i].len == 0 || lines[i].text[lines[i].len - 1] != record_delimiter(opts))
                        write_record_terminator(opts);
                    continue;
                }
                if (should_omit_long_match_line(opts, lines[i].len))
                    print_omitted_long_line(opts);
                else if (opts->replace)
                    print_replaced_record((unsigned char *)lines[i].text, lines[i].len, m, opts);
                else
                    print_match_colored((unsigned char *)lines[i].text, lines[i].len, bm.start, bm.end, opts);
            }
        } else {
            maybe_print_heading(display_name, opts, &heading_printed_for_file);
            print_result_prefix(heading_printed_for_file ? NULL : display_name,
                                opts, i + 1, 0, false, lines[i].byte_offset,
                                context_field_separator(opts));
            fwrite(lines[i].text, 1, lines[i].len, stdout);
            if (lines[i].len == 0 || lines[i].text[lines[i].len - 1] != record_delimiter(opts))
                write_record_terminator(opts);
        }
        in_group = true; last_printed = i;
    }
    if (stats && file_matches > 0)
        stats->files_with_matches++;
    *match_count += file_matches;
    free_lines(lines, nlines);
    return file_matches > 0 ? 0 : 1;
}

static int search_file_buffered(const char *filename, const char *display_name,
                                const char *progname,
                                struct bx_matcher *m, struct search_opts *opts,
                                int *match_count,
                                struct bx_record_stream *record_stream,
                                struct bx_search_stats *stats) {
    bool use_stdin = false;
    FILE *f = open_search_input_stream(filename, progname, opts, record_stream, &use_stdin);
    if (!f)
        return 2;

    return search_file_buffered_opened(f, use_stdin, display_name, progname, m, opts,
                                       match_count, record_stream, stats);
}

static int search_file_scanner_opened(FILE *f,
                                      bool use_stdin,
                                      const char *display_name,
                                      const char *progname,
                                      struct bx_matcher *m,
                                      struct search_opts *opts,
                                      int *match_count,
                                      struct bx_search_scanner *scanner,
                                      struct bx_search_stats *stats) {
    int file_matches = 0;
    int status = 1;
    bool heading_printed_for_file = false;
    bool stop = false;

    if (stats)
        stats->files_searched++;

    bx_search_scanner_begin_file(scanner, record_delimiter(opts));
    while (!stop && bx_search_scanner_read_chunk(scanner, f)) {
        if (stats)
            stats->bytes_searched += scanner->scan_len;

        size_t cursor = 0u;
        while (!stop) {
            struct bx_search_candidate candidate;
            if (!bx_search_scanner_next_literal_candidate(scanner, m->literal, &cursor, &candidate))
                break;

            struct bx_match bm;
            if (search_file_scanner_can_shortcut_file_presence(opts)) {
                if (!matcher_verify_literal_candidate_with_opts(m, scanner->buf, scanner->scan_len,
                                                                candidate.chunk_off, opts, &bm)) {
                    continue;
                }

                file_matches++;
                if (stats) {
                    stats->matches++;
                    stats->matched_lines++;
                }
                status = 0;
                stop = true;
                break;
            }

            struct bx_search_record_slice record;
            if (!bx_search_scanner_expand_record(scanner, &candidate, &record))
                continue;

            size_t match_len = record_match_len(record.data, record.len, opts);
            size_t candidate_record_off = candidate.chunk_off - record.chunk_off;
            if (!matcher_verify_literal_candidate_with_opts(m, record.data, match_len,
                                                            candidate_record_off, opts, &bm)) {
                if (matcher_find_with_opts(m, record.data, match_len, 0, opts, &bm) != 0)
                    continue;
            }

            cursor = record.chunk_off + record.len;
            size_t line_num = bx_search_scanner_record_number(scanner, &record);
            int record_match_count = opts->count_matches
                                         ? count_record_matches(m, record.data, match_len, opts)
                                         : 1;
            file_matches += record_match_count;
            if (stats) {
                stats->matches += record_match_count;
                stats->matched_lines++;
            }
            status = 0;

            if (opts->quiet) {
                stop = true;
                break;
            }
            if (opts->count_only) {
                if (opts->max_count > 0 && file_matches >= opts->max_count)
                    stop = true;
                continue;
            }
            if (opts->files_with_matches || opts->files_without_match) {
                if (!opts->stats) {
                    stop = true;
                    break;
                }
                if (opts->max_count > 0 && file_matches >= opts->max_count)
                    stop = true;
                continue;
            }

            maybe_print_heading(display_name, opts, &heading_printed_for_file);
            print_result_prefix(heading_printed_for_file ? NULL : display_name,
                                opts, (int)line_num, bm.start + 1u, true,
                                (size_t)record.file_off,
                                match_field_separator(opts));
            if (should_omit_long_match_line(opts, record.len))
                print_omitted_long_line(opts);
            else
                print_match_colored(record.data, record.len, bm.start, bm.end, opts);

            if (opts->max_count > 0 && file_matches >= opts->max_count)
                stop = true;
        }
    }

    if (ferror(f)) {
        report_path_error(progname, display_name, errno ? errno : EIO, opts);
        if (!use_stdin)
            fclose(f);
        return 2;
    }

    if (opts->quiet && file_matches > 0)
        status = 0;
    if (opts->count_only)
        print_count_result(display_name, opts, file_matches);
    if (opts->files_with_matches && file_matches > 0 && display_name) {
        if (opts->null_output)
            printf("%s%c", display_name, '\0');
        else
            printf("%s\n", display_name);
    }
    if (opts->files_without_match && file_matches == 0 && display_name) {
        if (opts->null_output)
            printf("%s%c", display_name, '\0');
        else
            printf("%s\n", display_name);
    }
    if (stats && file_matches > 0)
        stats->files_with_matches++;
    if (match_count)
        *match_count += file_matches;
    if (!use_stdin)
        fclose(f);
    return status;
}

static int search_file_scanner(const char *filename,
                               const char *display_name,
                               const char *progname,
                               struct bx_matcher *m,
                               struct search_opts *opts,
                               int *match_count,
                               struct bx_search_scanner *scanner,
                               struct bx_record_stream *record_stream,
                               struct bx_search_stats *stats) {
    bool use_stdin = false;
    FILE *f = open_search_input_stream(filename, progname, opts, record_stream, &use_stdin);
    if (!f)
        return 2;
    if (!search_file_scanner_stream_is_eligible(f))
        return search_file_streaming_opened(f, use_stdin, display_name, m, opts,
                                            match_count, record_stream, stats);

    return search_file_scanner_opened(f, use_stdin, display_name, progname, m, opts,
                                      match_count, scanner, stats);
}

/* --- streaming search (no context) --- */

static int search_file_streaming_opened(FILE *f,
                                        bool use_stdin,
                                        const char *display_name,
                                        struct bx_matcher *m,
                                        struct search_opts *opts,
                                        int *match_count,
                                        struct bx_record_stream *record_stream,
                                        struct bx_search_stats *stats) {
    ssize_t len;
    int line_num = 0, file_matches = 0, status = 1;
    size_t file_offset = 0;
    bool saw_match_record = false;
    bool heading_printed_for_file = false;
    if (stats)
        stats->files_searched++;

    while ((len = read_record(f, record_stream, opts)) != -1) {
        char *line = record_stream->record;
        size_t line_offset = file_offset;
        file_offset += (size_t)len;
        if (stats)
            stats->bytes_searched += (size_t)len;
        line_num++;
        struct bx_match bm;
        size_t match_len = record_match_len((unsigned char *)line, (size_t)len, opts);
        bool matched = (matcher_find_with_opts(m, (unsigned char *)line, match_len, 0, opts, &bm) == 0);
        if (opts->invert_match) matched = !matched;
        if (matched) {
            int record_match_count = (!opts->invert_match && opts->count_matches)
                                         ? count_record_matches(m, (unsigned char *)line, match_len, opts)
                                         : 1;
            file_matches += record_match_count;
            if (stats) {
                stats->matches += record_match_count;
                stats->matched_lines++;
            }
            status = 0;
            saw_match_record = true;
            if (opts->quiet) break;
            if (opts->count_only) {
                if (opts->max_count > 0 && file_matches >= opts->max_count) break;
                continue;
            }
            if (opts->files_with_matches || opts->files_without_match) {
                if (!opts->stats) break;
                if (opts->max_count > 0 && file_matches >= opts->max_count) break;
                continue;
            }
            if (opts->only_matching && !opts->invert_match) {
                maybe_print_heading(display_name, opts, &heading_printed_for_file);
                print_only_matches((unsigned char *)line, (size_t)len,
                                   heading_printed_for_file ? NULL : display_name, line_num,
                                   line_offset, m, opts);
            } else {
                if (!(opts->only_matching && opts->invert_match)) {
                    maybe_print_heading(display_name, opts, &heading_printed_for_file);
                    print_result_prefix(heading_printed_for_file ? NULL : display_name,
                                        opts, line_num, bm.start + 1, true, line_offset,
                                        match_field_separator(opts));
                    if (opts->invert_match) {
                        fwrite(line, 1, (size_t)len, stdout);
                        if (len == 0 || line[len - 1] != record_delimiter(opts))
                            write_record_terminator(opts);
                    } else if (opts->replace) {
                        print_replaced_record((unsigned char *)line, (size_t)len, m, opts);
                    } else {
                        if (should_omit_long_match_line(opts, (size_t)len))
                            print_omitted_long_line(opts);
                        else
                            print_match_colored((unsigned char *)line, (size_t)len, bm.start, bm.end, opts);
                    }
                }
            }
            if (opts->max_count > 0 && file_matches >= opts->max_count) break;
        } else if (opts->passthru) {
            print_result_prefix(display_name, opts, line_num, 0, false, line_offset,
                                context_field_separator(opts));
            fwrite(line, 1, (size_t)len, stdout);
            if (len == 0 || line[len - 1] != record_delimiter(opts))
                write_record_terminator(opts);
        } else if (opts->stop_on_nonmatch && saw_match_record) {
            break;
        }
    }

    if (opts->quiet && file_matches > 0) status = 0;
    if (opts->count_only)
        print_count_result(display_name, opts, file_matches);
    if (opts->files_with_matches && file_matches > 0 && display_name) {
        if (opts->null_output) printf("%s%c", display_name, '\0');
        else printf("%s\n", display_name);
    }
    if (opts->files_without_match && file_matches == 0 && display_name) {
        if (opts->null_output) printf("%s%c", display_name, '\0');
        else printf("%s\n", display_name);
    }
    if (stats && file_matches > 0)
        stats->files_with_matches++;
    if (match_count) *match_count += file_matches;
    if (!use_stdin) fclose(f);
    return status;
}

static int search_file_streaming(const char *filename, const char *display_name,
                                 const char *progname,
                                 struct bx_matcher *m, struct search_opts *opts,
                                 int *match_count,
                                 struct bx_record_stream *record_stream,
                                 struct bx_search_stats *stats) {
    bool use_stdin = false;
    FILE *f = open_search_input_stream(filename, progname, opts, record_stream, &use_stdin);
    if (!f)
        return 2;

    return search_file_streaming_opened(f, use_stdin, display_name, m, opts,
                                        match_count, record_stream, stats);
}

static int search_binary_without_match(const char *display_name,
                                       struct search_opts *opts,
                                       int *match_count,
                                       struct bx_search_stats *stats) {
    if (stats)
        stats->files_searched++;
    if (opts->count_only)
        print_count_result(display_name, opts, 0);
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

static bool binary_file_matches_opened(FILE *f,
                                       struct bx_matcher *m,
                                       struct search_opts *opts,
                                       struct bx_record_stream *record_stream) {
    ssize_t len;
    bool matched = false;

    while ((len = read_record(f, record_stream, opts)) != -1) {
        char *line = record_stream->record;
        struct bx_match bm;
        size_t match_len = record_match_len((unsigned char *)line, (size_t)len, opts);
        if (!opts->null_data && memchr(line, '\0', match_len) != NULL) {
            size_t chunk_start = 0;
            matched = false;
            while (chunk_start <= match_len) {
                size_t chunk_len = 0;
                while (chunk_start + chunk_len < match_len &&
                       line[chunk_start + chunk_len] != '\0') {
                    chunk_len++;
                }
                matched = matcher_find_with_opts(
                              m, (unsigned char *)line + chunk_start, chunk_len, 0,
                              opts, &bm) == 0;
                if (opts->invert_match)
                    matched = !matched;
                if (matched)
                    break;
                if (chunk_start + chunk_len >= match_len)
                    break;
                chunk_start += chunk_len + 1;
            }
        } else {
            matched = matcher_find_with_opts(m, (unsigned char *)line, match_len,
                                             0, opts, &bm) == 0;
            if (opts->invert_match)
                matched = !matched;
        }
        if (matched)
            break;
    }

    return matched;
}

static bool binary_file_matches(const char *filename,
                                struct bx_matcher *m,
                                struct search_opts *opts,
                                struct bx_record_stream *record_stream) {
    FILE *f = fopen(filename, "r");
    if (!f)
        return false;

    bx_record_stream_prepare_file(f, record_stream);
    bool matched = binary_file_matches_opened(f, m, opts, record_stream);
    fclose(f);
    return matched;
}

static int search_file(const char *filename, const char *display_name_override, const char *progname,
                       struct bx_matcher *m, struct search_opts *opts,
                       int *match_count,
                       struct bx_search_scanner *scanner,
                       struct bx_record_stream *record_stream,
                       struct bx_search_stats *stats) {
    const char *display_name = display_name_for_stream(filename, display_name_override, opts);
    bool use_stdin = (!filename || strcmp(filename, "-") == 0);
    if (opts->multiline)
        return search_file_multiline(filename, display_name, progname, m, opts, match_count, stats);
    if (display_name && !opts->recursive) {
        struct stat st;
        if (filename && strcmp(filename, "-") != 0 && lstat(filename, &st) == 0 && S_ISDIR(st.st_mode)) {
            report_path_error(progname, filename, EISDIR, opts);
            return 2;
        }
    }

    if (use_stdin && !opts->null_data && !opts->binary_as_text &&
        (opts->binary_without_match ||
         (!opts->quiet && !opts->count_only &&
          !opts->files_with_matches && !opts->files_without_match))) {
        return search_file_buffered(filename, display_name, progname, m, opts,
                                    match_count, record_stream, stats);
    }

    if (!use_stdin && !opts->null_data && !opts->binary_as_text) {
        FILE *f = open_search_input_stream(filename, progname, opts, record_stream, NULL);
        if (!f)
            return 2;

        bool is_binary_file = false;
        if (bx_record_stream_probe_binary_prefix(f, &is_binary_file)) {
            if (is_binary_file) {
                if (opts->binary_without_match) {
                    fclose(f);
                    return search_binary_without_match(display_name, opts, match_count, stats);
                }

                if (opts->quiet || opts->files_with_matches || opts->files_without_match || opts->count_only) {
                    if (needs_line_buffering(opts))
                        return search_file_buffered_opened(f, false, display_name, progname, m, opts,
                                                           match_count, record_stream, stats);
                    return search_file_streaming_opened(f, false, display_name, m, opts,
                                                        match_count, record_stream, stats);
                }

                bool matched = binary_file_matches_opened(f, m, opts, record_stream);
                fclose(f);
                if (matched) {
                    report_binary_match(progname, display_name);
                    if (match_count)
                        (*match_count)++;
                    return 0;
                }
                return 1;
            }

            if (needs_line_buffering(opts))
                return search_file_buffered_opened(f, false, display_name, progname, m, opts,
                                                   match_count, record_stream, stats);
            if (search_file_can_use_scanner(m, opts, false)
                && search_file_scanner_stream_is_eligible(f)) {
                return search_file_scanner_opened(f, false, display_name, progname, m, opts,
                                                  match_count, scanner, stats);
            }
            return search_file_streaming_opened(f, false, display_name, m, opts,
                                                match_count, record_stream, stats);
        }

        fclose(f);
    }

    if (!use_stdin && !opts->null_data && !opts->binary_as_text && is_binary(filename)) {
        if (opts->binary_without_match)
            return search_binary_without_match(display_name, opts, match_count, stats);

        if (opts->quiet || opts->files_with_matches || opts->files_without_match || opts->count_only) {
            if (needs_line_buffering(opts))
                return search_file_buffered(filename, display_name, progname, m, opts,
                                            match_count, record_stream, stats);
            return search_file_streaming(filename, display_name, progname, m, opts,
                                         match_count, record_stream, stats);
        }

        if (binary_file_matches(filename, m, opts, record_stream)) {
            report_binary_match(progname, display_name);
            if (match_count)
                (*match_count)++;
            return 0;
        }
        return 1;
    }

    if (needs_line_buffering(opts))
        return search_file_buffered(filename, display_name, progname, m, opts,
                                    match_count, record_stream, stats);
    if (search_file_can_use_scanner(m, opts, use_stdin))
        return search_file_scanner(filename, display_name, progname, m, opts,
                                   match_count, scanner, record_stream, stats);
    return search_file_streaming(filename, display_name, progname, m, opts,
                                 match_count, record_stream, stats);
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

    if (S_ISREG(st.st_mode) || S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode))
        return true;

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
    struct bx_search_scanner *scanner;
    struct bx_record_stream *record_stream;
    struct bx_search_stats *stats;
    int *exit_status;
    bool *match_seen;
    bool *error_seen;
    bool *stop;
    bool strip_dot_prefix;
};

struct files_walk_state {
    struct search_opts *opts;
    const char *progname;
    bool *error_seen;
    bool strip_dot_prefix;
};

static const char *const rg_ignore_filenames[] = {
    ".gitignore",
    ".ignore",
    ".rgignore",
};

static struct bx_walk_opts bx_search_make_walk_opts(const char *progname,
                                                    enum bx_search_personality personality,
                                                    const struct search_opts *opts,
                                                    bool *stop) {
    return (struct bx_walk_opts){
        .sort_entries = bx_search_use_rg_sort_policy(personality, opts),
        .reverse_sort = opts->sort_paths_reverse,
        .follow_symlinks = opts->follow_symlinks,
        .follow_root_symlink = true,
        .post_order = false,
        .stay_on_filesystem = false,
        .stop = stop,
        .suppress_eacces = false,
        .suppress_errors = opts->suppress_errors,
        .report_eacces = false,
        .os_error_style = progname_uses_os_error_style(progname),
        .error_prefix = progname,
        .max_depth = opts->max_depth,
        .cycle_mode = bx_search_cycle_mode(personality, opts),
        .cycle_report = bx_search_cycle_report(personality, opts),
    };
}

static struct bx_walk_filter_opts bx_search_make_filter_opts(const struct search_opts *opts) {
    return (struct bx_walk_filter_opts){
        .hidden = opts->hidden,
        .include_patterns = opts->include_patterns,
        .include_pattern_casefold = opts->include_pattern_casefold,
        .num_include_patterns = opts->num_include,
        .exclude_patterns = opts->exclude_patterns,
        .num_exclude_patterns = opts->num_exclude,
        .exclude_dirs = opts->exclude_dir_patterns,
        .num_exclude_dirs = opts->num_exclude_dir,
    };
}

static struct bx_walk_ignore_opts bx_search_make_ignore_opts(const struct search_opts *opts) {
    return (struct bx_walk_ignore_opts){
        .no_ignore = opts->no_ignore,
        .no_ignore_parent = opts->no_ignore_parent,
        .no_ignore_vcs = opts->no_ignore_vcs,
        .no_ignore_dot = opts->no_ignore_dot,
        .no_require_git = opts->no_require_git,
        .gitignore_enabled = false,
        .ignore_filenames = rg_ignore_filenames,
        .num_ignore_filenames = 3,
    };
}

struct bx_search_operand_ref {
    const char *path;
    int index;
};

static int bx_search_operand_ref_compare(const void *left, const void *right) {
    const struct bx_search_operand_ref *a = left;
    const struct bx_search_operand_ref *b = right;
    int cmp = strcmp(a->path, b->path);
    if (cmp != 0)
        return cmp;
    return (a->index > b->index) - (a->index < b->index);
}

static struct bx_search_operand_ref *bx_search_collect_sorted_operands(
    int argc, char **argv, int first_file, int *out_count
) {
    int count = argc - first_file;
    if (out_count)
        *out_count = count;
    if (count <= 0)
        return NULL;

    struct bx_search_operand_ref *refs = calloc((size_t)count, sizeof(*refs));
    if (!refs)
        return NULL;

    for (int i = 0; i < count; i++) {
        refs[i].path = argv[first_file + i];
        refs[i].index = first_file + i;
    }
    qsort(refs, (size_t)count, sizeof(*refs), bx_search_operand_ref_compare);
    return refs;
}

static enum bx_walk_action fs_cb(struct bx_walk_entry *entry, void *user) {
    struct files_walk_state *st = user;
    if (!entry->is_dir)
        printf("%s%c", display_path_for_output(entry->path, st && st->strip_dot_prefix),
               (st && st->opts && st->opts->null_output) ? '\0' : '\n');
    return BX_WALK_CONTINUE;
}

static enum bx_walk_action grep_walk_error_cb(const char *path, int errnum, void *user) {
    struct grep_walk_state *gs = user;
    report_path_error(gs->progname, path, errnum, gs->opts);
    *gs->exit_status = 2;
    if (gs->error_seen)
        *gs->error_seen = true;
    return BX_WALK_CONTINUE;
}

static enum bx_walk_action files_walk_error_cb(const char *path, int errnum, void *user) {
    struct files_walk_state *st = user;
    report_path_error(st->progname, path, errnum, st->opts);
    if (st->error_seen)
        *st->error_seen = true;
    return BX_WALK_CONTINUE;
}

static bool grep_explicit_entry_selected(const struct grep_walk_state *gs,
                                         const char *path) {
    const char *name = bx_path_basename_ptr(path);

    if (gs->opts->num_include > 0) {
        struct bx_walk_filter_opts filter_opts = {
            .hidden = gs->opts->hidden,
            .include_patterns = gs->opts->include_patterns,
            .include_pattern_casefold = gs->opts->include_pattern_casefold,
            .num_include_patterns = gs->opts->num_include,
        };
        struct bx_walk_filter_state filter_state;
        bx_walk_filter_init(&filter_state, &filter_opts, path);
        if (!bx_walk_filter_matches_include(&filter_state, name, path))
            return false;
    }

    for (int i = 0; i < gs->opts->num_exclude; i++) {
        if (fnmatch(gs->opts->exclude_patterns[i], name, 0) == 0)
            return false;
    }

    return true;
}

static enum bx_walk_action grep_walk_cb(struct bx_walk_entry *entry, void *user) {
    struct grep_walk_state *gs = user;
    if (gs->stop && *gs->stop)
        return BX_WALK_STOP;
    if (entry->is_dir)
        return BX_WALK_CONTINUE;

    const char *display_name = display_path_for_output(entry->path, gs->strip_dot_prefix);

    int r = search_file(entry->path, display_name, gs->progname, gs->m, gs->opts,
                        gs->match_count, gs->scanner, gs->record_stream, gs->stats);
    if (r == 2) {
        *gs->exit_status = 2;
        if (gs->error_seen) *gs->error_seen = true;
        return BX_WALK_CONTINUE;
    }
    if (r == 0) {
        *gs->exit_status = 0;
        if (gs->match_seen) *gs->match_seen = true;
        if (gs->opts->quiet && gs->stop)
            return BX_WALK_STOP;
    }
    return BX_WALK_CONTINUE;
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
        if (rc == 1)
            return 0;
        if (rc == 3)
            return 1;
        return 2;
    }

    if (opts.files_only) {
        bool error_seen = false;
        struct files_walk_state fstate = {
            .opts = &opts,
            .progname = progname,
            .error_seen = &error_seen,
        };
        struct bx_walk_opts walk_opts = bx_search_make_walk_opts(progname, personality, &opts, NULL);
        walk_opts.cycle_mode = opts.follow_symlinks ? BX_WALK_CYCLE_SYMLINK_REPEAT
                                                    : BX_WALK_CYCLE_NONE;
        walk_opts.cycle_report = BX_WALK_CYCLE_ERROR;
        struct bx_walk_filter_opts filter_opts = bx_search_make_filter_opts(&opts);
        struct bx_walk_ignore_opts ignore_opts = bx_search_make_ignore_opts(&opts);
        struct bx_search_walk_config walk_config = {
            .walk_opts = &walk_opts,
            .filter_opts = &filter_opts,
            .ignore_opts = &ignore_opts,
            .visit = fs_cb,
            .error = files_walk_error_cb,
        };
        int num_files = argc - first_file;
        int sorted_operand_count = 0;
        struct bx_search_operand_ref *sorted_operands =
            opts.sort_paths
                ? bx_search_collect_sorted_operands(argc, argv, first_file, &sorted_operand_count)
                : NULL;
        if (num_files == 0) {
            fstate.strip_dot_prefix = true;
            if (bx_search_walk(".", &walk_config, &fstate) != 0)
                error_seen = true;
        } else {
            for (int operand_i = 0; operand_i < num_files; operand_i++) {
                int j = sorted_operands
                            ? sorted_operands[opts.sort_paths_reverse
                                                  ? (sorted_operand_count - 1 - operand_i)
                                                  : operand_i]
                                  .index
                            : (first_file + operand_i);
                struct stat st;
                if (stat(argv[j], &st) != 0) {
                    report_path_error(progname, argv[j], errno, &opts);
                    error_seen = true;
                    continue;
                }
                if (S_ISDIR(st.st_mode))
                    error_seen |= bx_search_walk(argv[j], &walk_config, &fstate) != 0;
                else
                    printf("%s%c", argv[j], opts.null_output ? '\0' : '\n');
            }
        }
        free(sorted_operands);
        bx_search_free_options(&opts);
        return error_seen ? 2 : 0;
    }

    struct bx_matcher *m;
    char *compile_error = NULL;
    struct bx_search_scanner scanner = {0};
    struct bx_record_stream record_stream = {0};

    if (opts.num_extra_patterns > 0) {
        bool use_basic_grouping = !bx_search_personality_is_rg(personality) &&
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
            if (!bx_search_personality_is_rg(personality)) {
                fprintf(stderr, "%s: Invalid regular expression\n",
                        argv[0] ? argv[0] : "grep");
            } else {
                fprintf(stderr, "%s: invalid pattern '%s': %s\n",
                        argv[0] ? argv[0] : "grep", pattern, compile_error);
            }
            free(compile_error);
        } else {
            fprintf(stderr, "%s: invalid pattern: %s\n",
                    argv[0] ? argv[0] : "grep", pattern);
        }
        bx_search_scanner_dispose(&scanner);
        bx_record_stream_dispose(&record_stream);
        bx_search_free_options(&opts);
        return 2;
    }

    int num_files = argc - first_file;
    int sorted_operand_count = 0;
    struct bx_search_operand_ref *sorted_operands =
        opts.sort_paths
            ? bx_search_collect_sorted_operands(argc, argv, first_file, &sorted_operand_count)
            : NULL;
    bool rg_searches_stdin = (bx_search_personality_is_rg(personality)
                              && num_files == 0
                              && rg_should_search_stdin());
    if (!opts.show_filename && !opts.hide_filename)
        opts.show_filename = search_default_show_filename(argc, argv, first_file, personality,
                                                          &opts, rg_searches_stdin);
    if (opts.hide_filename)
        opts.show_filename = false;
    if (bx_search_personality_is_rg(personality)
        && !opts.show_line_number && isatty(STDOUT_FILENO))
        opts.show_line_number = true;
    if (!opts.heading_set)
        opts.heading = search_default_heading(personality, &opts);

    int global_matches = 0;
    int exit_status = 1;
    bool match_seen = false;
    bool error_seen = false;
    struct bx_search_stats stats = {0};
    current_stats = opts.stats ? &stats : NULL;

    if (num_files == 0) {
        if ((bx_search_personality_is_rg(personality) && !rg_searches_stdin) ||
            (!bx_search_personality_is_rg(personality) && opts.recursive)) {
            bool stop = false;
            struct grep_walk_state gs = {.m = m, .opts = &opts,
                                         .progname = progname,
                                         .match_count = &global_matches,
                                         .scanner = &scanner,
                                         .record_stream = &record_stream,
                                         .stats = &stats,
                                         .exit_status = &exit_status,
                                         .match_seen = &match_seen,
                                         .error_seen = &error_seen,
                                         .stop = &stop,
                                         .strip_dot_prefix = true};
            struct bx_walk_opts walk_opts = bx_search_make_walk_opts(progname, personality, &opts, &stop);
            struct bx_walk_filter_opts filter_opts = bx_search_make_filter_opts(&opts);
            struct bx_walk_ignore_opts ignore_opts = bx_search_make_ignore_opts(&opts);
            struct bx_search_walk_config walk_config = {
                .walk_opts = &walk_opts,
                .filter_opts = &filter_opts,
                .ignore_opts = &ignore_opts,
                .visit = grep_walk_cb,
                .error = grep_walk_error_cb,
            };

            if (bx_search_walk(".", &walk_config, &gs) != 0) {
                exit_status = 2;
                error_seen = true;
            }
        } else {
            exit_status = search_file(NULL, NULL, progname, m, &opts, &global_matches,
                                      &scanner, &record_stream, &stats);
            if (exit_status == 0) match_seen = true;
            else if (exit_status == 2) error_seen = true;
        }
    } else if (opts.recursive) {
        bool stop = false;
        struct grep_walk_state gs = {.m = m, .opts = &opts,
                                     .progname = progname,
                                     .match_count = &global_matches,
                                     .scanner = &scanner,
                                     .record_stream = &record_stream,
                                     .stats = &stats,
                                     .exit_status = &exit_status,
                                     .match_seen = &match_seen,
                                     .error_seen = &error_seen,
                                     .stop = &stop,
                                     .strip_dot_prefix = false};
        struct bx_walk_opts walk_opts = bx_search_make_walk_opts(progname, personality, &opts, &stop);
        struct bx_walk_filter_opts filter_opts = bx_search_make_filter_opts(&opts);
        struct bx_walk_ignore_opts ignore_opts = bx_search_make_ignore_opts(&opts);
        struct bx_search_walk_config walk_config = {
            .walk_opts = &walk_opts,
            .filter_opts = &filter_opts,
            .ignore_opts = &ignore_opts,
            .visit = grep_walk_cb,
            .error = grep_walk_error_cb,
        };

        for (int operand_i = 0; operand_i < num_files && !stop; operand_i++) {
            int j = sorted_operands
                        ? sorted_operands[opts.sort_paths_reverse
                                              ? (sorted_operand_count - 1 - operand_i)
                                              : operand_i]
                              .index
                        : (first_file + operand_i);
            struct stat st;
            if (stat(argv[j], &st) != 0) {
                report_path_error(progname, argv[j], errno, &opts);
                exit_status = 2;
                error_seen = true;
                continue;
            }
            if (S_ISDIR(st.st_mode)) {
                if (bx_search_walk(argv[j], &walk_config, &gs) != 0) {
                    exit_status = 2;
                    error_seen = true;
                }
            } else if (S_ISREG(st.st_mode)) {
                if (grep_explicit_entry_selected(&gs, argv[j])) {
                    struct bx_walk_entry entry = {.path = argv[j], .is_dir = false, .mode = st.st_mode};
                    enum bx_walk_action action = grep_walk_cb(&entry, &gs);
                    if (action == BX_WALK_STOP)
                        stop = true;
                }
            }
        }
    } else {
        for (int operand_i = 0; operand_i < num_files; operand_i++) {
            int j = sorted_operands
                        ? sorted_operands[opts.sort_paths_reverse
                                              ? (sorted_operand_count - 1 - operand_i)
                                              : operand_i]
                              .index
                        : (first_file + operand_i);
            if (argv[j] && strcmp(argv[j], "-") != 0) {
                struct stat st;
                if (lstat(argv[j], &st) == 0 && S_ISDIR(st.st_mode)) {
                    if (opts.directory_mode == BX_GREP_DIR_SKIP)
                        continue;
                    report_path_error(progname, argv[j], EISDIR, &opts);
                    exit_status = 2;
                    error_seen = true;
                    continue;
                }
            }
            int r = search_file(argv[j], NULL, progname, m, &opts, &global_matches,
                                &scanner, &record_stream, &stats);
            if (r == 2) {
                exit_status = 2;
                error_seen = true;
            } else if (r == 0) {
                exit_status = 0;
                match_seen = true;
                if (opts.quiet)
                    break;
            }
        }
    }
    matcher_free(m);
    free(sorted_operands);
    bx_search_scanner_dispose(&scanner);
    bx_record_stream_dispose(&record_stream);
    if (opts.stats)
        print_stats_summary(&stats);
    current_stats = NULL;
    bx_search_free_options(&opts);
    if (opts.quiet && match_seen)
        return 0;
    if (error_seen)
        return 2;
    return match_seen ? 0 : 1;
}
