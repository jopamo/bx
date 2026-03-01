#define _GNU_SOURCE
#include <errno.h>
#include <fnmatch.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <poll.h>
#include <pthread.h>
#include <regex.h>
#include <unistd.h>

#include "lib/cli_common.h"
#include "lib/cancel_state.h"
#include "lib/output_sink.h"
#include "lib/path_ops.h"
#include "lib/thread_count.h"
#include "lib/work_pool.h"
#include "search.h"
#include "options.h"
#include "fswalk/walk.h"
#include "filter.h"
#include "ignore.h"
#include "pcre2_matcher.h"
#include "literal.h"
#include "dev_counters.h"
#include "record_stream.h"
#include "rg_output.h"
#include "rg_text.h"
#include "rg_transform.h"
#include "scanner.h"
#include "traverse.h"
#include "lib/color.h"
#include "bx/diag.h"

static bool progname_uses_os_error_style(const char *progname) {
    if (!progname) return false;
    progname = bx_cli_progname(progname, "grep");
    return strcmp(progname, "rg") == 0;
}

static FILE *bx_search_output_stream(void);
static FILE *bx_search_error_stream(void);

static bool bx_search_path_exceeds_max_filesize(const char *path,
                                                const struct search_opts *opts) {
    if (!path || !opts || !opts->max_filesize_set)
        return false;

    struct stat st;
    if (stat(path, &st) != 0)
        return false;
    return S_ISREG(st.st_mode) && st.st_size > (off_t)opts->max_filesize;
}

static bool bx_search_entry_exceeds_max_filesize(struct bx_walk_entry *entry,
                                                 const struct search_opts *opts) {
    if (!entry || !opts || !opts->max_filesize_set || entry->is_dir)
        return false;
    if (!entry->metadata_loaded && !bx_walk_entry_load_metadata(entry))
        return false;
    return entry->metadata_loaded && S_ISREG(entry->mode)
        && entry->size > (off_t)opts->max_filesize;
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
        fprintf(bx_search_error_stream(), "%s: %s: %s (os error %d)\n",
                progname, path, strerror(errnum), errnum);
    else
        fprintf(bx_search_error_stream(), "%s: %s: %s\n", progname, path, strerror(errnum));
}

static void report_binary_match(const char *progname, const char *path) {
    fprintf(bx_search_error_stream(), "%s: %s: binary file matches\n", progname, path);
}

static bool bx_search_mode_is_special_input(mode_t mode) {
    return S_ISCHR(mode) || S_ISBLK(mode) || S_ISFIFO(mode) || S_ISSOCK(mode);
}

static bool bx_search_should_skip_special_input_mode(mode_t mode,
                                                     const struct search_opts *opts) {
    return opts && opts->device_mode == BX_GREP_DEVICE_SKIP
        && bx_search_mode_is_special_input(mode);
}

static bool bx_search_entry_should_skip_special_input(struct bx_walk_entry *entry,
                                                      const struct search_opts *opts) {
    if (!entry || !opts || opts->device_mode != BX_GREP_DEVICE_SKIP)
        return false;

    if (!entry->metadata_loaded && !bx_walk_entry_load_metadata(entry))
        return false;

    return bx_search_mode_is_special_input(entry->mode);
}

static char *display_path_for_output(const char *path, bool strip_dot_prefix,
                                     const struct search_opts *opts) {
    return bx_rg_display_path_dup(path, strip_dot_prefix,
                                  opts ? opts->path_separator : '/');
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

struct bx_search_output_ctx {
    FILE *out;
    FILE *err;
    struct bx_search_stats *stats;
    bool heading_output_started;
    bool used_heading;
    bool emitted_stdout;
};

static _Thread_local struct bx_search_output_ctx *current_output_ctx = NULL;

static struct bx_search_output_ctx *bx_search_output_ctx_push(struct bx_search_output_ctx *ctx) {
    struct bx_search_output_ctx *previous = current_output_ctx;
    current_output_ctx = ctx;
    return previous;
}

static void bx_search_output_ctx_pop(struct bx_search_output_ctx *previous) {
    current_output_ctx = previous;
}

static FILE *bx_search_output_stream(void) {
    return current_output_ctx && current_output_ctx->out ? current_output_ctx->out : stdout;
}

static FILE *bx_search_error_stream(void) {
    return current_output_ctx && current_output_ctx->err ? current_output_ctx->err : stderr;
}

static void bx_search_note_stdout_output(void) {
    if (current_output_ctx)
        current_output_ctx->emitted_stdout = true;
}

static size_t bx_search_fwrite_out(const void *buf, size_t len) {
    size_t written;

    if (len == 0u)
        return 0u;
    written = fwrite(buf, 1u, len, bx_search_output_stream());
    if (written > 0u)
        bx_search_note_stdout_output();
    return written;
}

static int bx_search_fputs_out(const char *text) {
    int rc = fputs(text, bx_search_output_stream());
    if (rc >= 0 && text && *text)
        bx_search_note_stdout_output();
    return rc;
}

static int bx_search_putc_out(int ch) {
    int rc = fputc(ch, bx_search_output_stream());
    if (rc != EOF)
        bx_search_note_stdout_output();
    return rc;
}

static int bx_search_printf_out(const char *fmt, ...) {
    va_list ap;
    int rc;

    va_start(ap, fmt);
    rc = vfprintf(bx_search_output_stream(), fmt, ap);
    va_end(ap);
    if (rc > 0)
        bx_search_note_stdout_output();
    return rc;
}

static bool is_binary(const char *path);
static int search_binary_without_match(const char *display_name,
                                       struct search_opts *opts,
                                       int *match_count,
                                       struct bx_search_stats *stats);
static int search_file_streaming_opened(FILE *f,
                                        bool use_stdin,
                                        const char *display_name,
                                        const char *progname,
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

static int matcher_find_posix_portable(regex_t *regex,
                                       const unsigned char *buf,
                                       size_t len,
                                       size_t start,
                                       struct bx_match *out) {
    if (!regex || !buf || !out || start > len)
        return -1;

#ifdef REG_STARTEND
    regmatch_t match = {
        .rm_so = (regoff_t)start,
        .rm_eo = (regoff_t)len,
    };
    int rc = regexec(regex, (const char *)buf, 1, &match, REG_STARTEND);
    if (rc != 0)
        return -1;
    if (match.rm_so < 0 || match.rm_eo < 0)
        return -1;
    out->start = (size_t)match.rm_so;
    out->end = (size_t)match.rm_eo;
    return 0;
#else
    size_t chunk_start = start;

    while (chunk_start <= len) {
        const unsigned char *chunk_end = memchr(buf + chunk_start, '\0', len - chunk_start);
        size_t chunk_len = chunk_end ? (size_t)(chunk_end - (buf + chunk_start))
                                     : (len - chunk_start);
        char *chunk = malloc(chunk_len + 1u);
        if (!chunk)
            return -1;
        memcpy(chunk, buf + chunk_start, chunk_len);
        chunk[chunk_len] = '\0';

        regmatch_t match = {0};
        int eflags = 0;
        if (chunk_start > 0u)
            eflags |= REG_NOTBOL;
        if (chunk_end)
            eflags |= REG_NOTEOL;

        int rc = regexec(regex, chunk, 1, &match, eflags);
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
        chunk_start += chunk_len + 1u;
    }

    return -1;
#endif
}

static int matcher_find(struct bx_matcher *m, const unsigned char *buf, size_t len,
                        size_t start, struct bx_match *out) {
    if (m->kind == MATCHER_LITERAL)
        return bx_literal_find(m->literal, buf, len, start, out);
    if (m->kind == MATCHER_POSIX) {
        if (start > len)
            return -1;

        /*
         * Search only the logical record slice [start, len). Grep trims the
         * record delimiter before matching, so POSIX regexec must not see the
         * trailing newline or any carried-over bytes from a reused buffer.
         */
        return matcher_find_posix_portable(&m->posix, buf, len, start, out);
    }

    return bx_regex_find(m->regex, buf, len, start, out);
}

static bool match_has_word_boundaries(const unsigned char *buf, size_t len,
                                      const struct bx_match *match,
                                      const struct search_opts *opts) {
    return bx_rg_match_has_word_boundaries(buf, len, match->start, match->end,
                                           opts && opts->unicode);
}

static int matcher_find_with_opts(struct bx_matcher *m, const unsigned char *buf, size_t len,
                                  size_t start, struct search_opts *opts, struct bx_match *out) {
    bx_search_dev_counters_note_matcher_invocation();
    if (opts->line_regexp && m->kind == MATCHER_LITERAL) {
        if (start != 0u)
            return -1;
        if (!bx_literal_verify_at(m->literal, buf, len, 0u, out))
            return -1;
        return out->start == 0u && out->end == len ? 0 : -1;
    }

    size_t pos = start;
    while (pos <= len) {
        if (matcher_find(m, buf, len, pos, out) != 0)
            return -1;
        if (!opts->word_regexp || match_has_word_boundaries(buf, len, out, opts))
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
    if (!m || m->kind != MATCHER_LITERAL || !opts)
        return false;
    bx_search_dev_counters_note_matcher_invocation();
    if (opts->line_regexp) {
        if (candidate_start != 0u)
            return false;
        if (!bx_literal_verify_at(m->literal, buf, len, 0u, out))
            return false;
        return out->start == 0u && out->end == len;
    }

    if (!bx_literal_verify_at(m->literal, buf, len, candidate_start, out))
        return false;
    if (opts->word_regexp && !match_has_word_boundaries(buf, len, out, opts))
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

    if (opts->line_regexp && !opts->fixed_strings) {
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

static char *build_search_pattern(const char *pattern,
                                  enum bx_search_personality personality,
                                  const struct search_opts *opts) {
    if (!pattern || !opts || opts->num_extra_patterns == 0)
        return pattern ? strdup(pattern) : NULL;

    bool use_basic_grouping = !bx_search_personality_is_rg(personality) &&
                              !opts->perl_regexp &&
                              !opts->extended_regex &&
                              !opts->fixed_strings;
    const char *group_open = use_basic_grouping ? "\\(" : "(";
    const char *group_close = use_basic_grouping ? "\\)" : ")";
    const char *group_sep = use_basic_grouping ? "\\|" : "|";
    size_t total = strlen(pattern) + strlen(group_open) + strlen(group_close) + 1u;

    for (int k = 0; k < opts->num_extra_patterns; k++)
        total += strlen(opts->extra_patterns[k]) + strlen(group_sep);

    char *combined = malloc(total);
    if (!combined)
        return NULL;

    char *p = combined;
    memcpy(p, group_open, strlen(group_open));
    p += strlen(group_open);
    memcpy(p, pattern, strlen(pattern));
    p += strlen(pattern);
    for (int k = 0; k < opts->num_extra_patterns; k++) {
        memcpy(p, group_sep, strlen(group_sep));
        p += strlen(group_sep);
        const char *extra_pattern = opts->extra_patterns[k];
        size_t extra_len = strlen(extra_pattern);
        memcpy(p, extra_pattern, extra_len);
        p += extra_len;
    }
    memcpy(p, group_close, strlen(group_close));
    p += strlen(group_close);
    *p = '\0';
    return combined;
}

static bool matcher_is_scanner_literal_eligible(const struct bx_matcher *m,
                                                const struct search_opts *opts) {
    if (!m || !opts || m->kind != MATCHER_LITERAL)
        return false;
    return !bx_literal_contains_byte(m->literal, (unsigned char)record_delimiter(opts));
}

/* --- match output helpers --- */

static size_t printable_trim_prefix(const unsigned char *line, size_t len,
                                    const struct search_opts *opts) {
    size_t match_len = record_match_len(line, len, opts);
    if (!opts || !opts->trim)
        return 0u;
    return bx_rg_trim_leading_ascii_space(line, match_len);
}

static void print_plain_record_contents(const unsigned char *line, size_t len,
                                        struct search_opts *opts) {
    size_t trim_prefix = printable_trim_prefix(line, len, opts);
    bx_search_fwrite_out(line + trim_prefix, len - trim_prefix);
    stats_count_bytes(len - trim_prefix);
}

static void print_match_colored(const unsigned char *line, size_t len,
                                 size_t match_start, size_t match_end,
                                 struct search_opts *opts) {
    size_t trim_prefix = printable_trim_prefix(line, len, opts);
    if (trim_prefix > match_start)
        trim_prefix = match_start;
    if (!opts->only_matching) {
        bx_search_fwrite_out(line + trim_prefix, match_start - trim_prefix);
        stats_count_bytes(match_start - trim_prefix);
        bx_rg_emit_color_style_start_file(bx_search_output_stream(), &opts->rg_colors.match);
        bx_search_fwrite_out(line + match_start, match_end - match_start);
        stats_count_bytes(match_end - match_start);
        bx_rg_emit_color_reset_file(bx_search_output_stream());
        bx_search_fwrite_out(line + match_end, len - match_end);
        stats_count_bytes(len - match_end);
        if (len == 0 || line[len - 1] != record_delimiter(opts))
            write_record_terminator(opts);
    } else {
        bx_search_fwrite_out(line + match_start, match_end - match_start);
        stats_count_bytes(match_end - match_start);
        write_record_terminator(opts);
    }
    bx_search_dev_counters_note_output_line_emitted();
}

static void print_replacement_piece(const char *replace, const unsigned char *match, size_t match_len) {
    if (!replace) {
        bx_search_fwrite_out(match, match_len);
        stats_count_bytes(match_len);
        return;
    }

    for (const char *p = replace; *p; ++p) {
        if (p[0] == '$' && p[1] == '0') {
            bx_search_fwrite_out(match, match_len);
            stats_count_bytes(match_len);
            ++p;
            continue;
        }
        bx_search_putc_out((unsigned char)*p);
        stats_count_bytes(1);
    }
}

static void print_replaced_record(const unsigned char *line, size_t len,
                                  struct bx_matcher *m, struct search_opts *opts) {
    size_t match_len = record_match_len(line, len, opts);
    size_t trim_prefix = printable_trim_prefix(line, len, opts);
    size_t start = trim_prefix;
    size_t cursor = trim_prefix;

    while (start <= match_len) {
        struct bx_match bm;
        if (matcher_find_with_opts(m, line, match_len, start, opts, &bm) != 0)
            break;
        bx_search_fwrite_out(line + cursor, bm.start - cursor);
        stats_count_bytes(bm.start - cursor);
        print_replacement_piece(opts->replace, line + bm.start, bm.end - bm.start);
        cursor = bm.end;
        start = bm.end > bm.start ? bm.end : bm.start + 1;
    }

    bx_search_fwrite_out(line + cursor, match_len - cursor);
    stats_count_bytes(match_len - cursor);
    write_record_terminator(opts);
    bx_search_dev_counters_note_output_line_emitted();
}

static bool should_omit_long_match_line(const struct search_opts *opts, size_t record_len) {
    return opts->max_columns > 0 && !opts->only_matching && (int)record_len > opts->max_columns;
}

static void print_omitted_long_line(struct search_opts *opts) {
    bx_search_fputs_out("[Omitted long matching line]");
    stats_count_bytes(strlen("[Omitted long matching line]"));
    write_record_terminator(opts);
    bx_search_dev_counters_note_output_line_emitted();
}

static const char *match_field_separator(struct search_opts *opts) {
    return opts->field_match_separator ? opts->field_match_separator : ":";
}

static const char *context_field_separator(struct search_opts *opts) {
    return opts->field_context_separator ? opts->field_context_separator : "-";
}

static bool print_result_prefix(const char *display_name, struct search_opts *opts,
                                int line_num, size_t column, bool has_column,
                                size_t byte_offset, const char *sep) {
    bool printed = false;
    if (opts->show_filename && display_name) {
        char *hyperlink = bx_rg_hyperlink_open_dup(opts->hyperlink_format,
                                                   opts->hostname_bin,
                                                   display_name,
                                                   (size_t)line_num,
                                                   column,
                                                   opts->show_line_number,
                                                   has_column && opts->show_column);
        if (hyperlink)
            bx_search_fputs_out(hyperlink);
        bx_rg_emit_color_style_start_file(bx_search_output_stream(), &opts->rg_colors.path);
        bx_search_fputs_out(display_name);
        stats_count_bytes(strlen(display_name));
        bx_rg_emit_color_reset_file(bx_search_output_stream());
        if (hyperlink) {
            bx_search_fputs_out(bx_rg_hyperlink_close());
            free(hyperlink);
        }
        if (opts->null_filename)
            bx_search_putc_out('\0');
        else
            bx_search_fputs_out(sep);
        stats_count_bytes(opts->null_filename ? 1 : strlen(sep));
        printed = true;
    }
    if (opts->show_line_number) {
        bx_rg_emit_color_style_start_file(bx_search_output_stream(), &opts->rg_colors.line);
        int n = bx_search_printf_out(opts->initial_tab ? "%2d" : "%d", line_num);
        if (n > 0) stats_count_bytes((size_t)n);
        bx_rg_emit_color_reset_file(bx_search_output_stream());
        bx_search_fputs_out(sep);
        stats_count_bytes(strlen(sep));
        printed = true;
    }
    if (opts->show_column && has_column) {
        bx_rg_emit_color_style_start_file(bx_search_output_stream(), &opts->rg_colors.column);
        int n = bx_search_printf_out(opts->initial_tab ? "%2zu" : "%zu", column);
        if (n > 0) stats_count_bytes((size_t)n);
        bx_rg_emit_color_reset_file(bx_search_output_stream());
        bx_search_fputs_out(sep);
        stats_count_bytes(strlen(sep));
        printed = true;
    }
    if (opts->show_byte_offset) {
        bx_rg_emit_color_style_start_file(bx_search_output_stream(), &opts->rg_colors.line);
        int n = bx_search_printf_out(opts->initial_tab ? "%2zu" : "%zu", byte_offset);
        if (n > 0) stats_count_bytes((size_t)n);
        bx_rg_emit_color_reset_file(bx_search_output_stream());
        bx_search_fputs_out(sep);
        stats_count_bytes(strlen(sep));
        printed = true;
    }
    return printed;
}

static void maybe_emit_initial_tab(const struct search_opts *opts, bool prefix_printed) {
    if (!opts || !opts->initial_tab || !prefix_printed)
        return;
    bx_search_putc_out('\t');
    stats_count_bytes(1);
}

static bool use_heading_output(const char *display_name, const struct search_opts *opts) {
    return opts->heading && opts->show_filename && display_name && display_name[0] != '\0';
}

static void maybe_print_heading(const char *display_name, struct search_opts *opts,
                                bool *heading_printed_for_file) {
    struct bx_search_output_ctx *ctx = current_output_ctx;

    if (!use_heading_output(display_name, opts) || *heading_printed_for_file)
        return;
    if (ctx && ctx->heading_output_started)
        bx_search_putc_out('\n');
    char *hyperlink = bx_rg_hyperlink_open_dup(opts->hyperlink_format,
                                               opts->hostname_bin,
                                               display_name, 1u, 1u, false, false);
    if (hyperlink)
        bx_search_fputs_out(hyperlink);
    bx_rg_emit_color_style_start_file(bx_search_output_stream(), &opts->rg_colors.path);
    bx_search_printf_out("%s", display_name);
    bx_rg_emit_color_reset_file(bx_search_output_stream());
    if (hyperlink) {
        bx_search_fputs_out(bx_rg_hyperlink_close());
        free(hyperlink);
    }
    bx_search_putc_out('\n');
    bx_search_dev_counters_note_output_line_emitted();
    *heading_printed_for_file = true;
    if (ctx) {
        ctx->heading_output_started = true;
        ctx->used_heading = true;
    }
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
        bool prefix_printed = print_result_prefix(display_name, opts, line_num,
                                                  bm.start + 1, true,
                                                  byte_offset + bm.start,
                                                  match_field_separator(opts));
        maybe_emit_initial_tab(opts, prefix_printed);
        bx_search_fwrite_out(line + bm.start, bm.end - bm.start);
        stats_count_bytes(bm.end - bm.start);
        write_record_terminator(opts);
        bx_search_dev_counters_note_output_line_emitted();
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
    return bx_rg_record_match_len(buf, len, record_delimiter(opts), opts->crlf);
}

static void write_record_terminator(const struct search_opts *opts) {
    bx_search_putc_out((unsigned char)record_delimiter(opts));
    stats_count_bytes(1);
}

static void print_count_result(const char *display_name, struct search_opts *opts, int file_matches) {
    if (opts->omit_zero_count_output && file_matches == 0)
        return;
    if (opts->show_filename && display_name)
        bx_search_printf_out("%s%c%d\n", display_name, opts->null_filename ? '\0' : ':', file_matches);
    else
        bx_search_printf_out("%d\n", file_matches);
    bx_search_dev_counters_note_output_line_emitted();
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
    for (int i = 0; i < 8; ++i)
        bx_search_dev_counters_note_output_line_emitted();
}

static void stats_count_bytes(size_t count) {
    if (current_output_ctx && current_output_ctx->stats)
        current_output_ctx->stats->bytes_printed += count;
}

static int finish_search_main(int status) {
    bx_search_dev_counters_report(stderr);
    bx_search_dev_counters_reset();
    return status;
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
    bx_search_dev_counters_note_file_opened();

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
        bx_search_dev_counters_note_bytes_read(nread);
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

static int search_buffer_multiline(unsigned char *buf, size_t len,
                                   const char *display_name,
                                   struct bx_matcher *m,
                                   struct search_opts *opts, int *match_count,
                                   struct bx_search_stats *stats) {
    if (stats) {
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
            bool prefix_printed = print_result_prefix(
                heading_printed_for_file ? NULL : display_name,
                opts, (int)line_num, column_number_for_offset(buf, bm.start), true,
                bm.start, match_field_separator(opts));
            maybe_emit_initial_tab(opts, prefix_printed);
            bx_search_fwrite_out(buf + bm.start, bm.end - bm.start);
            write_record_terminator(opts);
            bx_search_dev_counters_note_output_line_emitted();
        } else {
            size_t out_start = line_start_offset(buf, bm.start);
            size_t out_end = line_end_offset(buf, len, bm.end);
            maybe_print_heading(display_name, opts, &heading_printed_for_file);
            bool prefix_printed = print_result_prefix(
                heading_printed_for_file ? NULL : display_name,
                opts, (int)line_number_for_offset(buf, out_start),
                column_number_for_offset(buf, bm.start), true,
                out_start, match_field_separator(opts));
            maybe_emit_initial_tab(opts, prefix_printed);
            if (should_omit_long_match_line(opts, out_end - out_start))
                print_omitted_long_line(opts);
            else if (opts->replace) {
                print_replaced_record(buf + out_start, out_end - out_start, m, opts);
            } else {
                print_plain_record_contents(buf + out_start, out_end - out_start, opts);
                if (out_end == out_start || buf[out_end - 1] != record_delimiter(opts))
                    write_record_terminator(opts);
                bx_search_dev_counters_note_output_line_emitted();
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
            bx_search_printf_out("%s%c", display_name, '\0');
        else
            bx_search_printf_out("%s\n", display_name);
        bx_search_dev_counters_note_output_line_emitted();
    }
    if (opts->files_without_match && file_matches == 0 && display_name) {
        if (opts->null_output)
            bx_search_printf_out("%s%c", display_name, '\0');
        else
            bx_search_printf_out("%s\n", display_name);
        bx_search_dev_counters_note_output_line_emitted();
    }

    if (stats && file_matches > 0)
        stats->files_with_matches++;
    if (match_count)
        *match_count += file_matches;
    free(buf);
    return status;
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
        bx_search_dev_counters_note_file_opened();
    }

    size_t len = 0;
    unsigned char *buf = read_stream_all(f, &len);
    if (!use_stdin)
        fclose(f);
    if (!buf)
        return 2;
    if (stats)
        stats->files_searched++;
    return search_buffer_multiline(buf, len, display_name, m, opts, match_count, stats);
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
                if (opts->null_output) bx_search_printf_out("%s%c", display_name, '\0');
                else bx_search_printf_out("%s\n", display_name);
                bx_search_dev_counters_note_output_line_emitted();
            }
            if (opts->files_without_match && file_matches == 0 && display_name) {
                if (opts->null_output) bx_search_printf_out("%s%c", display_name, '\0');
                else bx_search_printf_out("%s\n", display_name);
                bx_search_dev_counters_note_output_line_emitted();
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
            if (opts->null_output) bx_search_printf_out("%s%c", display_name, '\0');
            else bx_search_printf_out("%s\n", display_name);
            bx_search_dev_counters_note_output_line_emitted();
        }
        if (opts->files_without_match && file_matches == 0 && display_name) {
            if (opts->null_output) bx_search_printf_out("%s%c", display_name, '\0');
            else bx_search_printf_out("%s\n", display_name);
            bx_search_dev_counters_note_output_line_emitted();
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

    bool want_group_separator = needs_line_buffering(opts);
    bool in_group = false;
    int last_printed = -1;
    for (int i = 0; i < nlines; i++) {
        if (!lines[i].print) { in_group = false; continue; }
        if (!in_group && last_printed >= 0 && i > last_printed + 1) {
            if (want_group_separator && !opts->suppress_group_separator) {
                bx_search_printf_out("%s\n", opts->group_separator ? opts->group_separator : "--");
                bx_search_dev_counters_note_output_line_emitted();
            }
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
                bool prefix_printed = print_result_prefix(
                    heading_printed_for_file ? NULL : display_name,
                    opts, i + 1, bm.start + 1, true, lines[i].byte_offset,
                    match_field_separator(opts));
                if (opts->only_matching && opts->invert_match) {
                    continue;
                }
                maybe_emit_initial_tab(opts, prefix_printed);
                if (opts->invert_match) {
                    print_plain_record_contents((unsigned char *)lines[i].text, lines[i].len, opts);
                    if (lines[i].len == 0 || lines[i].text[lines[i].len - 1] != record_delimiter(opts))
                        write_record_terminator(opts);
                    bx_search_dev_counters_note_output_line_emitted();
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
            bool prefix_printed = print_result_prefix(
                heading_printed_for_file ? NULL : display_name,
                opts, i + 1, 0, false, lines[i].byte_offset,
                context_field_separator(opts));
            maybe_emit_initial_tab(opts, prefix_printed);
            print_plain_record_contents((unsigned char *)lines[i].text, lines[i].len, opts);
            if (lines[i].len == 0 || lines[i].text[lines[i].len - 1] != record_delimiter(opts))
                write_record_terminator(opts);
            bx_search_dev_counters_note_output_line_emitted();
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
            bool prefix_printed = print_result_prefix(
                heading_printed_for_file ? NULL : display_name,
                opts, (int)line_num, bm.start + 1u, true,
                (size_t)record.file_off,
                match_field_separator(opts));
            maybe_emit_initial_tab(opts, prefix_printed);
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
            bx_search_printf_out("%s%c", display_name, '\0');
        else
            bx_search_printf_out("%s\n", display_name);
        bx_search_dev_counters_note_output_line_emitted();
    }
    if (opts->files_without_match && file_matches == 0 && display_name) {
        if (opts->null_output)
            bx_search_printf_out("%s%c", display_name, '\0');
        else
            bx_search_printf_out("%s\n", display_name);
        bx_search_dev_counters_note_output_line_emitted();
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
        return search_file_streaming_opened(f, use_stdin, display_name, progname, m, opts,
                                            match_count, record_stream, stats);

    return search_file_scanner_opened(f, use_stdin, display_name, progname, m, opts,
                                      match_count, scanner, stats);
}

/* --- streaming search (no context) --- */

static int search_file_streaming_opened(FILE *f,
                                        bool use_stdin,
                                        const char *display_name,
                                        const char *progname,
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
    bool stdout_emitted = false;
    bool binary_seen_before_output = false;
    const bool line_buffered_stdin_binary_watch =
        use_stdin && opts->line_buffered && !opts->null_data && !opts->binary_as_text &&
        !opts->quiet && !opts->count_only &&
        !opts->files_with_matches && !opts->files_without_match;
    if (stats)
        stats->files_searched++;

    while ((len = read_record(f, record_stream, opts)) != -1) {
        char *line = record_stream->record;
        size_t line_offset = file_offset;
        file_offset += (size_t)len;
        if (stats)
            stats->bytes_searched += (size_t)len;
        line_num++;
        if (line_buffered_stdin_binary_watch &&
            !stdout_emitted && memchr(line, '\0', (size_t)len) != NULL) {
            binary_seen_before_output = true;
        }
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
            if (binary_seen_before_output) {
                report_binary_match(progname, display_name);
                break;
            }
            if (opts->only_matching && !opts->invert_match) {
                maybe_print_heading(display_name, opts, &heading_printed_for_file);
                print_only_matches((unsigned char *)line, (size_t)len,
                                   heading_printed_for_file ? NULL : display_name, line_num,
                                   line_offset, m, opts);
                stdout_emitted = true;
            } else {
                if (!(opts->only_matching && opts->invert_match)) {
                    maybe_print_heading(display_name, opts, &heading_printed_for_file);
                    bool prefix_printed = print_result_prefix(
                        heading_printed_for_file ? NULL : display_name,
                        opts, line_num, bm.start + 1, true, line_offset,
                        match_field_separator(opts));
                    maybe_emit_initial_tab(opts, prefix_printed);
                    if (opts->invert_match) {
                        print_plain_record_contents((unsigned char *)line, (size_t)len, opts);
                        if (len == 0 || line[len - 1] != record_delimiter(opts))
                            write_record_terminator(opts);
                        bx_search_dev_counters_note_output_line_emitted();
                        stdout_emitted = true;
                    } else if (opts->replace) {
                        print_replaced_record((unsigned char *)line, (size_t)len, m, opts);
                        stdout_emitted = true;
                    } else {
                        if (should_omit_long_match_line(opts, (size_t)len))
                            print_omitted_long_line(opts);
                        else
                            print_match_colored((unsigned char *)line, (size_t)len, bm.start, bm.end, opts);
                        stdout_emitted = true;
                    }
                }
            }
            if (opts->max_count > 0 && file_matches >= opts->max_count) break;
        } else if (opts->passthru) {
            bool prefix_printed = print_result_prefix(display_name, opts, line_num,
                                                      0, false, line_offset,
                                                      context_field_separator(opts));
            maybe_emit_initial_tab(opts, prefix_printed);
            print_plain_record_contents((unsigned char *)line, (size_t)len, opts);
            if (len == 0 || line[len - 1] != record_delimiter(opts))
                write_record_terminator(opts);
            bx_search_dev_counters_note_output_line_emitted();
            stdout_emitted = true;
        } else if (opts->stop_on_nonmatch && saw_match_record) {
            break;
        }
    }

    if (opts->quiet && file_matches > 0) status = 0;
    if (opts->count_only)
        print_count_result(display_name, opts, file_matches);
    if (opts->files_with_matches && file_matches > 0 && display_name) {
        if (opts->null_output) bx_search_printf_out("%s%c", display_name, '\0');
        else bx_search_printf_out("%s\n", display_name);
        bx_search_dev_counters_note_output_line_emitted();
    }
    if (opts->files_without_match && file_matches == 0 && display_name) {
        if (opts->null_output) bx_search_printf_out("%s%c", display_name, '\0');
        else bx_search_printf_out("%s\n", display_name);
        bx_search_dev_counters_note_output_line_emitted();
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

    return search_file_streaming_opened(f, use_stdin, display_name, progname, m, opts,
                                        match_count, record_stream, stats);
}

static bool search_plain_output_needs_binary_sensitive_path(const struct search_opts *opts) {
    return opts &&
           !opts->null_data &&
           !opts->binary_as_text &&
           (opts->binary_without_match ||
            (!opts->quiet && !opts->count_only &&
             !opts->files_with_matches && !opts->files_without_match));
}

static int search_file_opened_without_reopen(FILE *f,
                                             bool use_stdin,
                                             const char *display_name,
                                             const char *progname,
                                             struct bx_matcher *m,
                                             struct search_opts *opts,
                                             int *match_count,
                                             struct bx_search_scanner *scanner,
                                             struct bx_record_stream *record_stream,
                                             struct bx_search_stats *stats) {
    if (opts->multiline) {
        size_t len = 0u;
        unsigned char *buf = read_stream_all(f, &len);
        if (!use_stdin)
            fclose(f);
        if (!buf)
            return 2;
        if (stats)
            stats->files_searched++;
        return search_buffer_multiline(buf, len, display_name, m, opts, match_count, stats);
    }

    if (needs_line_buffering(opts) || search_plain_output_needs_binary_sensitive_path(opts))
        return search_file_buffered_opened(f, use_stdin, display_name, progname, m, opts,
                                           match_count, record_stream, stats);

    if (search_file_can_use_scanner(m, opts, use_stdin)
        && search_file_scanner_stream_is_eligible(f)) {
        return search_file_scanner_opened(f, use_stdin, display_name, progname, m, opts,
                                          match_count, scanner, stats);
    }

    return search_file_streaming_opened(f, use_stdin, display_name, progname, m, opts,
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
            bx_search_printf_out("%s%c", display_name, '\0');
        else
            bx_search_printf_out("%s\n", display_name);
        bx_search_dev_counters_note_output_line_emitted();
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
    bx_search_dev_counters_note_file_opened();

    bx_record_stream_prepare_file(f, record_stream);
    bool matched = binary_file_matches_opened(f, m, opts, record_stream);
    fclose(f);
    return matched;
}

static int search_transformed_buffer(unsigned char *buf, size_t len,
                                     const char *display_name,
                                     const char *progname,
                                     struct bx_matcher *m,
                                     struct search_opts *opts,
                                     int *match_count,
                                     struct bx_record_stream *record_stream,
                                     struct bx_search_stats *stats) {
    FILE *mem;
    int rc;

    if (opts->multiline) {
        if (stats)
            stats->files_searched++;
        return search_buffer_multiline(buf, len, display_name, m, opts, match_count, stats);
    }

    mem = fmemopen(buf, len, "r");
    if (!mem) {
        free(buf);
        report_path_error(progname, display_name ? display_name : "(memory)", errno, opts);
        return 2;
    }

    if (needs_line_buffering(opts) ||
        (!opts->null_data && !opts->binary_as_text &&
         (opts->binary_without_match ||
          (!opts->quiet && !opts->count_only &&
           !opts->files_with_matches && !opts->files_without_match)))) {
        rc = search_file_buffered_opened(mem, false, display_name, progname, m, opts,
                                         match_count, record_stream, stats);
    } else {
        rc = search_file_streaming_opened(mem, false, display_name, progname, m, opts,
                                          match_count, record_stream, stats);
    }
    free(buf);
    return rc;
}

static int search_file(const char *filename, const char *display_name_override, const char *progname,
                       struct bx_matcher *m, struct search_opts *opts,
                       int *match_count,
                       struct bx_search_scanner *scanner,
                       struct bx_record_stream *record_stream,
                       struct bx_search_stats *stats) {
    bool use_stdin = (!filename || strcmp(filename, "-") == 0);
    struct stat operand_st;
    bool operand_st_loaded = false;
    char *owned_display_name = NULL;
    const char *display_name = display_name_for_stream(filename, display_name_override, opts);
    int result = 1;

    if (!display_name_override && filename && strcmp(filename, "-") != 0) {
        owned_display_name = bx_rg_display_path_dup(filename, false, opts->path_separator);
        if (owned_display_name)
            display_name = owned_display_name;
    }
    if (!use_stdin && filename && strcmp(filename, "-") != 0)
        operand_st_loaded = stat(filename, &operand_st) == 0;

    bx_rg_tracef(opts, "search: %s", display_name ? display_name : "(stdin)");

    if (operand_st_loaded && bx_search_mode_is_special_input(operand_st.st_mode)) {
        FILE *f;

        if (bx_search_should_skip_special_input_mode(operand_st.st_mode, opts)) {
            result = 1;
            goto out;
        }

        f = open_search_input_stream(filename, progname, opts, record_stream, NULL);
        if (!f)
            goto out_error;

        result = search_file_opened_without_reopen(f, false, display_name, progname, m, opts,
                                                   match_count, scanner, record_stream, stats);
        goto out;
    }

    if (bx_rg_transform_maybe_needed(opts, filename, use_stdin,
                                     use_stdin ? fileno(stdin) : -1)) {
        unsigned char *transformed = NULL;
        size_t transformed_len = 0u;
        enum bx_rg_transform_result transform_rc =
            bx_rg_load_transformed_input(filename, progname, opts,
                                         bx_search_error_stream(),
                                         &transformed, &transformed_len);
        if (transform_rc == BX_RG_TRANSFORM_NO_MATCH) {
            result = 1;
            goto out;
        }
        if (transform_rc == BX_RG_TRANSFORM_ERROR) {
            result = 2;
            goto out;
        }
        result = search_transformed_buffer(transformed, transformed_len, display_name, progname,
                                           m, opts, match_count, record_stream, stats);
        goto out;
    }

    if (opts->multiline) {
        result = search_file_multiline(filename, display_name, progname, m, opts, match_count, stats);
        goto out;
    }
    if (display_name && !opts->recursive) {
        struct stat st;
        if (filename && strcmp(filename, "-") != 0 && lstat(filename, &st) == 0 && S_ISDIR(st.st_mode)) {
            report_path_error(progname, filename, EISDIR, opts);
            result = 2;
            goto out;
        }
    }

    const bool line_buffered_stdin_streaming =
        use_stdin && opts->line_buffered && !opts->null_data && !opts->binary_as_text &&
        !opts->quiet && !opts->count_only &&
        !opts->files_with_matches && !opts->files_without_match;

    if (use_stdin && !line_buffered_stdin_streaming &&
        search_plain_output_needs_binary_sensitive_path(opts)) {
        result = search_file_buffered(filename, display_name, progname, m, opts,
                                      match_count, record_stream, stats);
        goto out;
    }

    if (!use_stdin && !opts->null_data && !opts->binary_as_text) {
        FILE *f = open_search_input_stream(filename, progname, opts, record_stream, NULL);
        if (!f)
            goto out_error;

        bool is_binary_file = false;
        if (bx_record_stream_probe_binary_prefix(f, &is_binary_file)) {
            if (is_binary_file) {
                if (opts->binary_without_match) {
                    fclose(f);
                    result = search_binary_without_match(display_name, opts, match_count, stats);
                    goto out;
                }

                if (opts->quiet || opts->files_with_matches || opts->files_without_match || opts->count_only) {
                    if (needs_line_buffering(opts))
                        result = search_file_buffered_opened(f, false, display_name, progname, m, opts,
                                                             match_count, record_stream, stats);
                    else
                        result = search_file_streaming_opened(f, false, display_name, progname, m, opts,
                                                              match_count, record_stream, stats);
                    goto out;
                }

                bool matched = binary_file_matches_opened(f, m, opts, record_stream);
                fclose(f);
                if (matched) {
                    report_binary_match(progname, display_name);
                    if (match_count)
                        (*match_count)++;
                    result = 0;
                    goto out;
                }
                result = 1;
                goto out;
            }

            if (needs_line_buffering(opts))
                result = search_file_buffered_opened(f, false, display_name, progname, m, opts,
                                                     match_count, record_stream, stats);
            if (search_file_can_use_scanner(m, opts, false)
                && search_file_scanner_stream_is_eligible(f)) {
                result = search_file_scanner_opened(f, false, display_name, progname, m, opts,
                                                    match_count, scanner, stats);
                goto out;
            }
            if (needs_line_buffering(opts))
                goto out;
            result = search_file_streaming_opened(f, false, display_name, progname, m, opts,
                                                  match_count, record_stream, stats);
            goto out;
        }

        {
            struct stat opened_st;
            if (fstat(fileno(f), &opened_st) == 0 && !S_ISREG(opened_st.st_mode)) {
                result = search_file_opened_without_reopen(f, false, display_name, progname,
                                                           m, opts, match_count, scanner,
                                                           record_stream, stats);
                goto out;
            }
        }

        fclose(f);
    }

    if (!use_stdin && !opts->null_data && !opts->binary_as_text && is_binary(filename)) {
        if (opts->binary_without_match)
            result = search_binary_without_match(display_name, opts, match_count, stats);

        if (result != 1)
            goto out;

        if (opts->quiet || opts->files_with_matches || opts->files_without_match || opts->count_only) {
            if (needs_line_buffering(opts))
                result = search_file_buffered(filename, display_name, progname, m, opts,
                                              match_count, record_stream, stats);
            else
                result = search_file_streaming(filename, display_name, progname, m, opts,
                                               match_count, record_stream, stats);
            goto out;
        }

        if (binary_file_matches(filename, m, opts, record_stream)) {
            report_binary_match(progname, display_name);
            if (match_count)
                (*match_count)++;
            result = 0;
            goto out;
        }
        result = 1;
        goto out;
    }

    if (needs_line_buffering(opts))
        result = search_file_buffered(filename, display_name, progname, m, opts,
                                      match_count, record_stream, stats);
    else if (search_file_can_use_scanner(m, opts, use_stdin))
        result = search_file_scanner(filename, display_name, progname, m, opts,
                                     match_count, scanner, record_stream, stats);
    else
        result = search_file_streaming(filename, display_name, progname, m, opts,
                                       match_count, record_stream, stats);
    goto out;

out_error:
    result = 2;
out:
    free(owned_display_name);
    return result;
}

/* --- binary detection --- */

static bool is_binary(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    bx_search_dev_counters_note_file_opened();
    unsigned char buf[1024];
    size_t n = fread(buf, 1, sizeof(buf), f);
    bx_search_dev_counters_note_bytes_read(n);
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

struct grep_walk_state;
struct bx_search_operand_ref;

static struct bx_walk_opts bx_search_make_walk_opts(const char *progname,
                                                    enum bx_search_personality personality,
                                                    const struct search_opts *opts,
                                                    bool *stop);
static struct bx_walk_filter_opts bx_search_make_filter_opts(const struct search_opts *opts);
static struct bx_walk_ignore_opts bx_search_make_ignore_opts(const char *progname,
                                                             const struct search_opts *opts);
static bool grep_explicit_entry_selected(const struct grep_walk_state *gs,
                                         const char *path);

/*
 * Recursive rg parallel search keeps output ordered by path discovery order.
 * Batch multiple files into one worker record so tiny-file trees do not pay
 * one pool job plus one ordered-output record per file.
 */
#define BX_SEARCH_PARALLEL_JOB_BATCH_MAX_FILES 64u
#define BX_SEARCH_PARALLEL_JOB_BATCH_MAX_PATH_BYTES 16384u

struct bx_search_parallel_job_item {
    char *path;
    char *display_name;
};

struct bx_search_parallel_job {
    uint64_t seq;
    size_t count;
    size_t path_bytes;
    struct bx_search_parallel_job_item items[BX_SEARCH_PARALLEL_JOB_BATCH_MAX_FILES];
};

struct bx_search_parallel_record {
    uint64_t seq;
    char *stdout_buf;
    size_t stdout_len;
    char *stderr_buf;
    size_t stderr_len;
    struct bx_search_stats stats;
    int status;
    bool match_seen;
    bool error_seen;
    bool used_heading;
};

struct bx_search_parallel_worker {
    struct bx_matcher *matcher;
    struct bx_search_scanner scanner;
    struct bx_record_stream record_stream;
};

struct bx_search_parallel_state {
    const char *progname;
    const char *pattern;
    enum bx_search_personality personality;
    struct search_opts *opts;
    struct bx_cancel_state cancel;
    struct bx_work_pool *pool;
    struct bx_output_sink *sink;
    pthread_mutex_t lock;
    uint64_t next_seq;
    int exit_status;
    bool match_seen;
    bool error_seen;
    bool heading_output_started;
    bool fatal_error;
    struct bx_search_stats stats;
    char *fatal_message;
    struct bx_search_parallel_job *pending_job;
};

struct bx_search_parallel_walk_state {
    struct bx_search_parallel_state *parallel;
    bool strip_dot_prefix;
};

static void bx_search_parallel_set_fatal(struct bx_search_parallel_state *state,
                                         const char *message) {
    if (!state)
        return;

    pthread_mutex_lock(&state->lock);
    state->fatal_error = true;
    if (!state->fatal_message && message)
        state->fatal_message = strdup(message);
    pthread_mutex_unlock(&state->lock);

    bx_cancel_state_request(&state->cancel);
    if (state->pool)
        bx_work_pool_wake(state->pool);
    if (state->sink)
        bx_output_sink_wake(state->sink);
}

static void bx_search_parallel_free_job(void *user, void *job_ptr) {
    (void)user;
    struct bx_search_parallel_job *job = job_ptr;

    if (!job)
        return;
    for (size_t i = 0; i < job->count; i++) {
        free(job->items[i].path);
        free(job->items[i].display_name);
    }
    free(job);
}

static void bx_search_parallel_dispose_record(void *user, void *record_ptr) {
    (void)user;
    struct bx_search_parallel_record *record = record_ptr;

    if (!record)
        return;
    free(record->stdout_buf);
    free(record->stderr_buf);
    free(record);
}

static uint64_t bx_search_parallel_record_seq(const void *record_ptr, void *user) {
    (void)user;
    return ((const struct bx_search_parallel_record *)record_ptr)->seq;
}

static void bx_search_parallel_emit_record(void *user, void *record_ptr) {
    struct bx_search_parallel_state *state = user;
    struct bx_search_parallel_record *record = record_ptr;

    if (!state || !record)
        return;

    if (record->used_heading && record->stdout_len > 0u && state->heading_output_started)
        fputc('\n', stdout);
    if (record->stdout_len > 0u && record->stdout_buf)
        fwrite(record->stdout_buf, 1u, record->stdout_len, stdout);
    if (record->stderr_len > 0u && record->stderr_buf)
        fwrite(record->stderr_buf, 1u, record->stderr_len, stderr);
    if (record->used_heading && record->stdout_len > 0u)
        state->heading_output_started = true;

    state->stats.matches += record->stats.matches;
    state->stats.matched_lines += record->stats.matched_lines;
    state->stats.files_with_matches += record->stats.files_with_matches;
    state->stats.files_searched += record->stats.files_searched;
    state->stats.bytes_printed += record->stats.bytes_printed;
    state->stats.bytes_searched += record->stats.bytes_searched;

    if (record->match_seen) {
        state->match_seen = true;
        if (state->exit_status != 2)
            state->exit_status = 0;
    }
    if (record->error_seen) {
        state->error_seen = true;
        state->exit_status = 2;
    }
}

static bool bx_search_parallel_submit_record(struct bx_search_parallel_state *state,
                                             struct bx_search_parallel_record *record) {
    if (!state || !record)
        return false;
    if (bx_output_sink_submit(state->sink, record))
        return true;

    bx_search_parallel_set_fatal(state, "rg: failed to submit ordered output record\n");
    bx_search_parallel_dispose_record(NULL, record);
    return false;
}

static void bx_search_parallel_drop_empty_pending_job(struct bx_search_parallel_state *state) {
    if (!state || !state->pending_job || state->pending_job->count != 0u)
        return;

    free(state->pending_job);
    state->pending_job = NULL;
}

static size_t bx_search_parallel_job_item_cost(const char *path,
                                               const char *display_name) {
    size_t total = strlen(path) + 1u;
    if (display_name)
        total += strlen(display_name) + 1u;
    return total;
}

static bool bx_search_parallel_flush_pending_job(struct bx_search_parallel_state *state) {
    struct bx_search_parallel_job *job;

    if (!state || !state->pending_job)
        return true;

    job = state->pending_job;
    state->pending_job = NULL;
    job->seq = state->next_seq++;
    if (bx_work_pool_submit(state->pool, job))
        return true;

    bx_search_parallel_free_job(NULL, job);
    return false;
}

static bool bx_search_parallel_submit_path_error(struct bx_search_parallel_state *state,
                                                 const char *path,
                                                 int errnum) {
    struct bx_search_parallel_record *record = calloc(1u, sizeof(*record));
    FILE *err_stream = NULL;

    if (!record)
        return false;
    if (!bx_search_parallel_flush_pending_job(state)) {
        bx_search_parallel_dispose_record(NULL, record);
        return false;
    }
    record->seq = state->next_seq;
    record->status = 2;
    record->error_seen = true;

    if (state->opts && state->opts->suppress_errors) {
        if (!bx_search_parallel_submit_record(state, record))
            return false;
        state->next_seq++;
        return true;
    }

    err_stream = open_memstream(&record->stderr_buf, &record->stderr_len);
    if (!err_stream) {
        bx_search_parallel_dispose_record(NULL, record);
        return false;
    }

    if (progname_uses_os_error_style(state->progname))
        fprintf(err_stream, "%s: %s: %s (os error %d)\n",
                state->progname, path, strerror(errnum), errnum);
    else
        fprintf(err_stream, "%s: %s: %s\n",
                state->progname, path, strerror(errnum));
    fclose(err_stream);

    if (!bx_search_parallel_submit_record(state, record))
        return false;
    state->next_seq++;
    return true;
}

static bool bx_search_parallel_queue_path(struct bx_search_parallel_state *state,
                                          const char *path,
                                          const char *display_name) {
    struct bx_search_parallel_job *job;
    struct bx_search_parallel_job_item *item;
    char *path_copy = NULL;
    char *display_copy = NULL;
    size_t item_cost;

    if (!state || !path)
        return false;
    if (bx_cancel_state_requested(&state->cancel))
        return false;

    item_cost = bx_search_parallel_job_item_cost(path, display_name);
    job = state->pending_job;
    if (job &&
        (job->count >= BX_SEARCH_PARALLEL_JOB_BATCH_MAX_FILES ||
         (job->count > 0u &&
          job->path_bytes + item_cost > BX_SEARCH_PARALLEL_JOB_BATCH_MAX_PATH_BYTES))) {
        if (!bx_search_parallel_flush_pending_job(state))
            return false;
        job = NULL;
    }

    if (!job) {
        job = calloc(1u, sizeof(*job));
        if (!job)
            return false;
        state->pending_job = job;
    }

    path_copy = strdup(path);
    if (!path_copy) {
        bx_search_parallel_drop_empty_pending_job(state);
        return false;
    }

    if (display_name) {
        display_copy = strdup(display_name);
        if (!display_copy) {
            free(path_copy);
            bx_search_parallel_drop_empty_pending_job(state);
            return false;
        }
    }

    item = &job->items[job->count++];
    item->path = path_copy;
    item->display_name = display_copy;
    job->path_bytes += item_cost;

    if (job->count >= BX_SEARCH_PARALLEL_JOB_BATCH_MAX_FILES ||
        job->path_bytes >= BX_SEARCH_PARALLEL_JOB_BATCH_MAX_PATH_BYTES)
        return bx_search_parallel_flush_pending_job(state);
    return true;
}

static void *bx_search_parallel_worker_init(void *user, size_t worker_index) {
    struct bx_search_parallel_state *state = user;
    struct bx_search_parallel_worker *worker;
    char *errmsg = NULL;

    (void)worker_index;
    worker = calloc(1u, sizeof(*worker));
    if (!worker)
        return NULL;

    worker->matcher = compile_matcher(state->pattern, state->personality, state->opts, &errmsg);
    if (!worker->matcher) {
        if (errmsg) {
            bx_search_parallel_set_fatal(state, errmsg);
            free(errmsg);
        }
        free(worker);
        return NULL;
    }

    return worker;
}

static void bx_search_parallel_worker_fini(void *user, void *worker_local, size_t worker_index) {
    (void)user;
    (void)worker_index;
    struct bx_search_parallel_worker *worker = worker_local;

    if (!worker)
        return;
    matcher_free(worker->matcher);
    bx_search_scanner_dispose(&worker->scanner);
    bx_record_stream_dispose(&worker->record_stream);
    free(worker);
}

static void bx_search_parallel_process_job(void *user,
                                           void *worker_local,
                                           void *job_ptr,
                                           size_t worker_index) {
    struct bx_search_parallel_state *state = user;
    struct bx_search_parallel_worker *worker = worker_local;
    struct bx_search_parallel_job *job = job_ptr;
    struct bx_search_parallel_record *record = NULL;
    struct bx_search_output_ctx output_ctx = {0};
    struct bx_search_output_ctx *previous_ctx = NULL;
    FILE *out_stream = NULL;
    FILE *err_stream = NULL;
    int match_count = 0;

    (void)worker_index;
    if (!state || !worker || !job)
        return;

    record = calloc(1u, sizeof(*record));
    if (!record) {
        bx_search_parallel_set_fatal(state, "rg: failed to allocate worker output record\n");
        bx_search_parallel_free_job(NULL, job);
        return;
    }
    record->seq = job->seq;

    if (bx_cancel_state_requested(&state->cancel) && state->opts->quiet) {
        record->status = 1;
        bx_search_parallel_submit_record(state, record);
        bx_search_parallel_free_job(NULL, job);
        return;
    }

    out_stream = open_memstream(&record->stdout_buf, &record->stdout_len);
    err_stream = open_memstream(&record->stderr_buf, &record->stderr_len);
    if (!out_stream || !err_stream) {
        if (out_stream)
            fclose(out_stream);
        if (err_stream)
            fclose(err_stream);
        bx_search_parallel_set_fatal(state, "rg: failed to allocate worker output streams\n");
        bx_search_parallel_dispose_record(NULL, record);
        bx_search_parallel_free_job(NULL, job);
        return;
    }

    output_ctx.out = out_stream;
    output_ctx.err = err_stream;
    output_ctx.stats = state->opts->stats ? &record->stats : NULL;
    previous_ctx = bx_search_output_ctx_push(&output_ctx);
    record->status = 1;
    for (size_t i = 0; i < job->count; i++) {
        int status;

        if (state->opts->quiet && bx_cancel_state_requested(&state->cancel))
            break;

        status = search_file(job->items[i].path,
                             job->items[i].display_name,
                             state->progname,
                             worker->matcher,
                             state->opts,
                             &match_count,
                             &worker->scanner,
                             &worker->record_stream,
                             &record->stats);
        if (status == 2) {
            record->error_seen = true;
            record->status = 2;
            continue;
        }
        if (status == 0) {
            record->match_seen = true;
            if (record->status != 2)
                record->status = 0;
            if (state->opts->quiet && bx_cancel_state_request(&state->cancel)) {
                bx_work_pool_wake(state->pool);
                break;
            }
        }
    }
    bx_search_output_ctx_pop(previous_ctx);

    record->used_heading = output_ctx.used_heading;

    fclose(out_stream);
    fclose(err_stream);

    if (!bx_search_parallel_submit_record(state, record))
        bx_search_parallel_set_fatal(state, "rg: failed to queue worker output\n");
    bx_search_parallel_free_job(NULL, job);
}

static enum bx_walk_action bx_search_parallel_walk_cb(struct bx_walk_entry *entry, void *user) {
    struct bx_search_parallel_walk_state *state = user;
    char *display_name;

    if (!state || !state->parallel)
        return BX_WALK_ERROR;
    if (bx_cancel_state_requested(&state->parallel->cancel))
        return BX_WALK_STOP;
    if (entry->is_dir)
        return BX_WALK_CONTINUE;
    if (bx_search_entry_exceeds_max_filesize(entry, state->parallel->opts))
        return BX_WALK_CONTINUE;
    if (bx_search_should_skip_special_input_mode(entry->mode, state->parallel->opts))
        return BX_WALK_CONTINUE;

    display_name = display_path_for_output(entry->path, state->strip_dot_prefix,
                                           state->parallel->opts);
    if (!bx_search_parallel_queue_path(state->parallel, entry->path,
                                       display_name ? display_name : entry->path)) {
        free(display_name);
        return bx_cancel_state_requested(&state->parallel->cancel)
            ? BX_WALK_STOP
            : BX_WALK_ERROR;
    }
    free(display_name);
    return BX_WALK_CONTINUE;
}

static enum bx_walk_action bx_search_parallel_walk_error_cb(const char *path,
                                                            int errnum,
                                                            void *user) {
    struct bx_search_parallel_walk_state *state = user;

    if (!state || !state->parallel)
        return BX_WALK_ERROR;
    if (!bx_search_parallel_submit_path_error(state->parallel, path, errnum))
        return BX_WALK_ERROR;
    return bx_cancel_state_requested(&state->parallel->cancel)
        ? BX_WALK_STOP
        : BX_WALK_CONTINUE;
}

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
        .stay_on_filesystem = opts->stay_on_filesystem,
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
        .glob_case_insensitive = opts->glob_case_insensitive,
        .include_patterns = opts->include_patterns,
        .include_pattern_casefold = opts->include_pattern_casefold,
        .num_include_patterns = opts->num_include,
        .exclude_patterns = opts->exclude_patterns,
        .num_exclude_patterns = opts->num_exclude,
        .exclude_dirs = opts->exclude_dir_patterns,
        .num_exclude_dirs = opts->num_exclude_dir,
    };
}

static struct bx_walk_ignore_opts bx_search_make_ignore_opts(const char *progname,
                                                             const struct search_opts *opts) {
    return (struct bx_walk_ignore_opts){
        .no_ignore = opts->no_ignore,
        .no_ignore_parent = opts->no_ignore_parent,
        .no_ignore_vcs = opts->no_ignore_vcs,
        .no_ignore_dot = opts->no_ignore_dot,
        .no_ignore_exclude = opts->no_ignore_exclude,
        .no_ignore_files = opts->no_ignore_files,
        .no_ignore_global = opts->no_ignore_global,
        .no_require_git = opts->no_require_git,
        .ignore_file_case_insensitive = opts->ignore_file_case_insensitive,
        .suppress_ignore_messages = opts->suppress_ignore_messages,
        .os_error_style = progname_uses_os_error_style(progname),
        .error_prefix = progname,
        .git_root = NULL,
        .extra_ignore_files = opts->ignore_files,
        .num_extra_ignore_files = opts->num_ignore_files,
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
    if (bx_search_entry_exceeds_max_filesize(entry, st ? st->opts : NULL))
        return BX_WALK_CONTINUE;
    if (!entry->is_dir) {
        char *display = display_path_for_output(entry->path, st && st->strip_dot_prefix,
                                                st ? st->opts : NULL);
        printf("%s%c", display ? display : entry->path,
               (st && st->opts && st->opts->null_output) ? '\0' : '\n');
        free(display);
    }
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
            .glob_case_insensitive = gs->opts->glob_case_insensitive,
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
        int flags = gs->opts->glob_case_insensitive ? FNM_CASEFOLD : 0;
        if (fnmatch(gs->opts->exclude_patterns[i], name, flags) == 0)
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
    if (bx_search_entry_exceeds_max_filesize(entry, gs ? gs->opts : NULL))
        return BX_WALK_CONTINUE;
    if (bx_search_entry_should_skip_special_input(entry, gs ? gs->opts : NULL))
        return BX_WALK_CONTINUE;

    char *display_name = display_path_for_output(entry->path, gs->strip_dot_prefix, gs->opts);

    int r = search_file(entry->path, display_name, gs->progname, gs->m, gs->opts,
                        gs->match_count, gs->scanner, gs->record_stream, gs->stats);
    free(display_name);
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

static bool bx_search_parallel_rg_supported(enum bx_search_personality personality,
                                            const struct search_opts *opts,
                                            int num_files,
                                            bool rg_searches_stdin) {
    if (!opts || personality != BX_SEARCH_RG)
        return false;
    if (opts->files_only || opts->trace || opts->quiet || rg_searches_stdin)
        return false;
    if (bx_thread_count_resolve(opts->threads) <= 1u)
        return false;
    if (num_files == 0)
        return true;
    return opts->recursive || num_files > 1;
}

static int bx_search_run_parallel_rg(int argc,
                                     char **argv,
                                     int first_file,
                                     struct bx_search_operand_ref *sorted_operands,
                                     int sorted_operand_count,
                                     const char *progname,
                                     const char *pattern,
                                     enum bx_search_personality personality,
                                     struct search_opts *opts,
                                     struct bx_search_stats *stats_out,
                                     bool *match_seen_out,
                                     bool *error_seen_out) {
    struct bx_search_parallel_state state = {
        .progname = progname,
        .pattern = pattern,
        .personality = personality,
        .opts = opts,
        .exit_status = 1,
    };
    struct bx_work_pool pool = {0};
    struct bx_output_sink sink = {0};
    size_t thread_count = bx_thread_count_resolve(opts->threads);
    size_t queue_capacity = thread_count > (SIZE_MAX / 64u) ? thread_count : thread_count * 64u;
    int num_files = argc - first_file;
    bool pool_ready = false;
    bool sink_ready = false;
    bool walk_error_seen = false;

    if (queue_capacity < thread_count)
        queue_capacity = thread_count;

    bx_cancel_state_init(&state.cancel);
    if (pthread_mutex_init(&state.lock, NULL) != 0)
        return 2;

    struct bx_output_sink_opts sink_opts = {
        .max_pending = queue_capacity,
        .first_seq = 0u,
        .ordered = true,
        .user = &state,
        .record_seq = bx_search_parallel_record_seq,
        .emit_record = bx_search_parallel_emit_record,
        .dispose_record = bx_search_parallel_dispose_record,
    };
    if (!bx_output_sink_init(&sink, &sink_opts)) {
        pthread_mutex_destroy(&state.lock);
        return 2;
    }
    sink_ready = true;
    state.sink = &sink;

    struct bx_work_pool_opts pool_opts = {
        .thread_count = thread_count,
        .queue_capacity = queue_capacity,
        .user = &state,
        .cancel = &state.cancel,
        .worker_init = bx_search_parallel_worker_init,
        .worker_fini = bx_search_parallel_worker_fini,
        .process_job = bx_search_parallel_process_job,
        .dispose_job = bx_search_parallel_free_job,
    };
    if (!bx_work_pool_init(&pool, &pool_opts)) {
        bx_search_parallel_set_fatal(&state, "rg: failed to initialize worker pool\n");
        goto done;
    }
    pool_ready = true;
    state.pool = &pool;

    if (num_files == 0) {
        struct bx_search_parallel_walk_state walk_state = {
            .parallel = &state,
            .strip_dot_prefix = true,
        };
        struct bx_walk_opts walk_opts = bx_search_make_walk_opts(progname, personality, opts, NULL);
        struct bx_walk_filter_opts filter_opts = bx_search_make_filter_opts(opts);
        struct bx_walk_ignore_opts ignore_opts = bx_search_make_ignore_opts(progname, opts);
        struct bx_search_walk_config walk_config = {
            .walk_opts = &walk_opts,
            .filter_opts = &filter_opts,
            .ignore_opts = &ignore_opts,
            .visit = bx_search_parallel_walk_cb,
            .error = bx_search_parallel_walk_error_cb,
        };

        if (bx_search_walk(".", &walk_config, &walk_state) != 0)
            walk_error_seen = true;
    } else if (opts->recursive) {
        struct bx_walk_opts walk_opts = bx_search_make_walk_opts(progname, personality, opts, NULL);
        struct bx_walk_filter_opts filter_opts = bx_search_make_filter_opts(opts);
        struct bx_walk_ignore_opts ignore_opts = bx_search_make_ignore_opts(progname, opts);
        struct bx_search_walk_config walk_config = {
            .walk_opts = &walk_opts,
            .filter_opts = &filter_opts,
            .ignore_opts = &ignore_opts,
            .visit = bx_search_parallel_walk_cb,
            .error = bx_search_parallel_walk_error_cb,
        };
        struct bx_search_parallel_walk_state walk_state = {
            .parallel = &state,
            .strip_dot_prefix = false,
        };

        for (int operand_i = 0; operand_i < num_files; operand_i++) {
            int j;
            struct stat st;

            if (bx_cancel_state_requested(&state.cancel))
                break;
            j = sorted_operands
                    ? sorted_operands[opts->sort_paths_reverse
                                          ? (sorted_operand_count - 1 - operand_i)
                                          : operand_i]
                          .index
                    : (first_file + operand_i);
            if (stat(argv[j], &st) != 0) {
                if (!bx_search_parallel_submit_path_error(&state, argv[j], errno)) {
                    bx_search_parallel_set_fatal(&state, "rg: failed to queue traversal error\n");
                    break;
                }
                continue;
            }
            if (S_ISDIR(st.st_mode)) {
                if (bx_search_walk(argv[j], &walk_config, &walk_state) != 0)
                    walk_error_seen = true;
                continue;
            }
            if (bx_search_should_skip_special_input_mode(st.st_mode, opts))
                continue;
            if (!grep_explicit_entry_selected(&(struct grep_walk_state){ .opts = opts }, argv[j]))
                continue;
            if (bx_search_path_exceeds_max_filesize(argv[j], opts))
                continue;

            char *display_name = display_path_for_output(argv[j], false, opts);
            if (!bx_search_parallel_queue_path(&state, argv[j],
                                               display_name ? display_name : argv[j])) {
                free(display_name);
                if (!bx_cancel_state_requested(&state.cancel))
                    bx_search_parallel_set_fatal(&state, "rg: failed to queue file job\n");
                break;
            }
            free(display_name);
        }
    } else {
        for (int operand_i = 0; operand_i < num_files; operand_i++) {
            int j = sorted_operands
                        ? sorted_operands[opts->sort_paths_reverse
                                              ? (sorted_operand_count - 1 - operand_i)
                                              : operand_i]
                              .index
                        : (first_file + operand_i);
            if (argv[j] && strcmp(argv[j], "-") != 0) {
                struct stat st;
                if (lstat(argv[j], &st) == 0) {
                    if (S_ISDIR(st.st_mode)) {
                        if (!bx_search_parallel_submit_path_error(&state, argv[j], EISDIR))
                            bx_search_parallel_set_fatal(&state, "rg: failed to queue directory error\n");
                        continue;
                    }
                    if (bx_search_should_skip_special_input_mode(st.st_mode, opts))
                        continue;
                }
                if (bx_search_path_exceeds_max_filesize(argv[j], opts))
                    continue;
            }
            if (!bx_search_parallel_queue_path(&state, argv[j], NULL)) {
                if (!bx_cancel_state_requested(&state.cancel))
                    bx_search_parallel_set_fatal(&state, "rg: failed to queue file job\n");
                break;
            }
        }
    }

done:
    if (!state.fatal_error && !bx_search_parallel_flush_pending_job(&state))
        bx_search_parallel_set_fatal(&state, "rg: failed to queue file job\n");
    if (pool_ready) {
        bx_work_pool_close(&pool);
        if (!bx_work_pool_join(&pool) && !state.fatal_error)
            bx_search_parallel_set_fatal(&state, "rg: worker pool failed\n");
    }
    if (sink_ready) {
        bx_output_sink_close(&sink);
        bx_output_sink_join(&sink);
    }
    if (state.fatal_error) {
        state.error_seen = true;
        state.exit_status = 2;
        if (state.fatal_message && *state.fatal_message) {
            fputs(state.fatal_message, stderr);
            if (state.fatal_message[strlen(state.fatal_message) - 1] != '\n')
                fputc('\n', stderr);
        }
    }
    if (walk_error_seen) {
        state.error_seen = true;
        state.exit_status = 2;
    }

    if (stats_out)
        *stats_out = state.stats;
    if (match_seen_out)
        *match_seen_out = state.match_seen;
    if (error_seen_out)
        *error_seen_out = state.error_seen;

    if (pool_ready)
        bx_work_pool_dispose(&pool);
    if (sink_ready)
        bx_output_sink_dispose(&sink);
    if (state.pending_job)
        bx_search_parallel_free_job(NULL, state.pending_job);
    pthread_mutex_destroy(&state.lock);
    free(state.fatal_message);
    return state.exit_status;
}

/* --- main entry point --- */

int bx_search_main(int argc, char **argv, enum bx_search_personality personality) {
    struct search_opts opts;
    const char *pattern;
    int first_file;
    const char *progname = argv[0] ? argv[0] : "grep";

    bx_search_dev_counters_begin_from_env();

    int rc = bx_search_parse_options(argc, argv, &opts, personality, &pattern, &first_file);
    if (rc != 0) {
        bx_search_free_options(&opts);
        if (rc == 1)
            return finish_search_main(0);
        if (rc == 3)
            return finish_search_main(1);
        return finish_search_main(2);
    }

    if (opts.line_buffered) {
        setvbuf(stdout, NULL, _IOLBF, 0);
    } else if (opts.block_buffered) {
        setvbuf(stdout, NULL, _IOFBF, BUFSIZ);
    }

    if (bx_search_personality_is_rg(personality)) {
        struct bx_walk_ignore_opts ignore_opts = bx_search_make_ignore_opts(progname, &opts);
        bx_ignore_validate_explicit_ignore_files(&ignore_opts);
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
        struct bx_walk_ignore_opts ignore_opts = bx_search_make_ignore_opts(progname, &opts);
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
                if (bx_search_path_exceeds_max_filesize(argv[j], &opts))
                    continue;
                if (S_ISDIR(st.st_mode))
                    error_seen |= bx_search_walk(argv[j], &walk_config, &fstate) != 0;
                else
                    printf("%s%c", argv[j], opts.null_output ? '\0' : '\n');
            }
        }
        free(sorted_operands);
        bx_search_free_options(&opts);
        return finish_search_main(error_seen ? 2 : 0);
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

    char *search_pattern = build_search_pattern(pattern, personality, &opts);
    if (!search_pattern) {
        free(sorted_operands);
        bx_search_free_options(&opts);
        return finish_search_main(2);
    }

    int global_matches = 0;
    int exit_status = 1;
    bool match_seen = false;
    bool error_seen = false;
    bool ran_search = false;
    struct bx_search_stats stats = {0};
    struct bx_search_output_ctx main_output_ctx = {
        .out = stdout,
        .err = stderr,
        .stats = opts.stats ? &stats : NULL,
    };
    struct bx_search_output_ctx *previous_output_ctx = bx_search_output_ctx_push(&main_output_ctx);

    struct bx_matcher *m;
    char *compile_error = NULL;
    struct bx_search_scanner scanner = {0};
    struct bx_record_stream record_stream = {0};

    m = compile_matcher(search_pattern, personality, &opts, &compile_error);

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
        exit_status = 2;
        error_seen = true;
        goto done;
    }

    if (bx_search_parallel_rg_supported(personality, &opts, num_files, rg_searches_stdin)) {
        matcher_free(m);
        ran_search = true;
        exit_status = bx_search_run_parallel_rg(argc, argv, first_file,
                                                sorted_operands, sorted_operand_count,
                                                progname, search_pattern, personality,
                                                &opts, &stats, &match_seen, &error_seen);
        goto done;
    }
    ran_search = true;

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
            struct bx_walk_ignore_opts ignore_opts = bx_search_make_ignore_opts(progname, &opts);
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
        struct bx_walk_ignore_opts ignore_opts = bx_search_make_ignore_opts(progname, &opts);
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
            } else {
                if (bx_search_should_skip_special_input_mode(st.st_mode, &opts))
                    continue;
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
                if (lstat(argv[j], &st) == 0) {
                    if (S_ISDIR(st.st_mode)) {
                        if (opts.directory_mode == BX_GREP_DIR_SKIP)
                            continue;
                        report_path_error(progname, argv[j], EISDIR, &opts);
                        exit_status = 2;
                        error_seen = true;
                        continue;
                    }
                    if (bx_search_should_skip_special_input_mode(st.st_mode, &opts))
                        continue;
                }
                if (bx_search_path_exceeds_max_filesize(argv[j], &opts))
                    continue;
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
    bx_search_scanner_dispose(&scanner);
    bx_record_stream_dispose(&record_stream);
done:
    if (opts.stats && ran_search)
        print_stats_summary(&stats);
    bx_search_output_ctx_pop(previous_output_ctx);
    free(search_pattern);
    free(sorted_operands);
    bx_search_free_options(&opts);
    if (opts.quiet && match_seen)
        return finish_search_main(0);
    if (error_seen)
        return finish_search_main(2);
    return finish_search_main(match_seen ? 0 : 1);
}
