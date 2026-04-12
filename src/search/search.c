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
#include "rg_parallel.h"
#include "search_internal.h"
#include "search_plan.h"
#include "search_run.h"
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
#include "sort.h"
#include "traverse.h"
#include "lib/color.h"
#include "bx/diag.h"

static bool progname_uses_os_error_style(const char *progname) {
    if (!progname) return false;
    progname = bx_cli_progname(progname, "grep");
    return strcmp(progname, "rg") == 0;
}

bool bx_search_progname_uses_os_error_style(const char *progname) {
    return progname_uses_os_error_style(progname);
}

static FILE *bx_search_output_stream(void);
static FILE *bx_search_error_stream(void);
static FILE *bx_search_null_stream(void);

static FILE *bx_search_null_stream(void) {
    static FILE *stream = NULL;

    if (!stream)
        stream = fopen("/dev/null", "wb");
    return stream ? stream : stderr;
}

bool bx_search_path_exceeds_max_filesize(const char *path,
                                                const struct search_opts *opts) {
    if (!path || !opts || !opts->max_filesize_set)
        return false;

    struct stat st;
    if (stat(path, &st) != 0)
        return false;
    return S_ISREG(st.st_mode) && st.st_size > (off_t)opts->max_filesize;
}

bool bx_search_entry_exceeds_max_filesize(struct bx_walk_entry *entry,
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
        || bx_search_sort_is_path(opts);
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

void bx_search_report_path_error(const char *progname, const char *path, int errnum,
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

bool bx_search_should_skip_special_input_mode(mode_t mode,
                                                     const struct search_opts *opts) {
    return opts && opts->device_mode == BX_GREP_DEVICE_SKIP
        && bx_search_mode_is_special_input(mode);
}

bool bx_search_entry_should_skip_special_input(struct bx_walk_entry *entry,
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

char *bx_search_display_path_for_output(const char *path,
                                        bool strip_dot_prefix,
                                        const struct search_opts *opts) {
    return display_path_for_output(path, strip_dot_prefix, opts);
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

static _Thread_local struct bx_search_output_ctx *current_output_ctx = NULL;

struct bx_search_output_ctx *bx_search_output_ctx_push(struct bx_search_output_ctx *ctx) {
    struct bx_search_output_ctx *previous = current_output_ctx;
    current_output_ctx = ctx;
    return previous;
}

void bx_search_output_ctx_pop(struct bx_search_output_ctx *previous) {
    current_output_ctx = previous;
}

static FILE *bx_search_output_stream(void) {
    if (current_output_ctx && !current_output_ctx->out &&
        current_output_ctx->capture_out_buf && current_output_ctx->capture_out_len) {
        current_output_ctx->out = open_memstream(current_output_ctx->capture_out_buf,
                                                 current_output_ctx->capture_out_len);
        if (current_output_ctx->out)
            bx_search_dev_counters_note_rg_sched(BX_SEARCH_RG_SCHED_MEMSTREAMS_OPENED, 1u);
        else
            current_output_ctx->capture_failed = true;
    }
    if (current_output_ctx && current_output_ctx->capture_failed)
        return bx_search_null_stream();
    return current_output_ctx && current_output_ctx->out ? current_output_ctx->out : stdout;
}

static FILE *bx_search_error_stream(void) {
    if (current_output_ctx && !current_output_ctx->err &&
        current_output_ctx->capture_err_buf && current_output_ctx->capture_err_len) {
        current_output_ctx->err = open_memstream(current_output_ctx->capture_err_buf,
                                                 current_output_ctx->capture_err_len);
        if (current_output_ctx->err)
            bx_search_dev_counters_note_rg_sched(BX_SEARCH_RG_SCHED_MEMSTREAMS_OPENED, 1u);
        else
            current_output_ctx->capture_failed = true;
    }
    if (current_output_ctx && current_output_ctx->capture_failed)
        return bx_search_null_stream();
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

static size_t bx_search_fwrite_stream(FILE *stream, const void *buf, size_t len) {
    size_t written;

    if (!stream || len == 0u)
        return 0u;
    written = fwrite(buf, 1u, len, stream);
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

static int bx_search_fputs_stream(FILE *stream, const char *text) {
    int rc;

    if (!stream)
        return EOF;
    rc = fputs(text, stream);
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

static int bx_search_putc_stream(FILE *stream, int ch) {
    int rc;

    if (!stream)
        return EOF;
    rc = fputc(ch, stream);
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

static int bx_search_printf_stream(FILE *stream, const char *fmt, ...) {
    va_list ap;
    int rc;

    if (!stream)
        return -1;
    va_start(ap, fmt);
    rc = vfprintf(stream, fmt, ap);
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
static int search_transformed_buffer(unsigned char *buf, size_t len,
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
    if (!m)
        return;
    if (m->kind == MATCHER_LITERAL)
        bx_literal_free(m->literal);
    else if (m->kind == MATCHER_POSIX)
        regfree(&m->posix);
    else
        bx_regex_free(m->regex);
    free(m);
}

void bx_search_matcher_free(struct bx_matcher *m) {
    matcher_free(m);
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

struct bx_matcher *bx_search_compile_matcher(const char *pattern,
                                             enum bx_search_personality personality,
                                             struct search_opts *opts,
                                             char **errmsg) {
    return compile_matcher(pattern, personality, opts, errmsg);
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
    FILE *out = current_output_ctx ? bx_search_output_stream() : stdout;
    size_t trim_prefix = printable_trim_prefix(line, len, opts);
    bx_search_fwrite_stream(out, line + trim_prefix, len - trim_prefix);
    stats_count_bytes(len - trim_prefix);
}

static void print_match_colored_cached(const unsigned char *line, size_t len,
                                       size_t match_start, size_t match_end,
                                       bool has_delim,
                                       struct search_opts *opts,
                                       FILE *out,
                                       bool color,
                                       unsigned char delimiter) {
    size_t trim_prefix = 0u;

    if (opts->trim)
        trim_prefix = printable_trim_prefix(line, len, opts);
    if (trim_prefix > match_start)
        trim_prefix = match_start;
    if (!opts->only_matching) {
        bx_search_fwrite_stream(out, line + trim_prefix, match_start - trim_prefix);
        stats_count_bytes(match_start - trim_prefix);
        if (color)
            bx_rg_emit_color_style_start_file(out, &opts->rg_colors.match);
        bx_search_fwrite_stream(out, line + match_start, match_end - match_start);
        stats_count_bytes(match_end - match_start);
        if (color)
            bx_rg_emit_color_reset_file(out);
        bx_search_fwrite_stream(out, line + match_end, len - match_end);
        stats_count_bytes(len - match_end);
        if (!has_delim)
            bx_search_putc_stream(out, delimiter);
    } else {
        bx_search_fwrite_stream(out, line + match_start, match_end - match_start);
        stats_count_bytes(match_end - match_start);
        bx_search_putc_stream(out, delimiter);
    }
    bx_search_dev_counters_note_output_line_emitted();
}

static void print_match_colored(const unsigned char *line, size_t len,
                                size_t match_start, size_t match_end,
                                struct search_opts *opts) {
    FILE *out = current_output_ctx ? bx_search_output_stream() : stdout;
    bool color = bx_color_enabled();
    unsigned char delimiter = (unsigned char)record_delimiter(opts);
    bool has_delim = len > 0u && line[len - 1u] == delimiter;

    print_match_colored_cached(line, len, match_start, match_end, has_delim,
                               opts, out, color, delimiter);
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
    FILE *out = current_output_ctx ? bx_search_output_stream() : stdout;

    bx_search_fputs_stream(out, "[Omitted long matching line]");
    stats_count_bytes(strlen("[Omitted long matching line]"));
    bx_search_putc_stream(out, (unsigned char)record_delimiter(opts));
    bx_search_dev_counters_note_output_line_emitted();
}

static const char *match_field_separator(struct search_opts *opts) {
    return opts->field_match_separator ? opts->field_match_separator : ":";
}

static const char *context_field_separator(struct search_opts *opts) {
    return opts->field_context_separator ? opts->field_context_separator : "-";
}

static bool print_result_prefix_cached(const char *display_name,
                                       size_t display_name_len,
                                       struct search_opts *opts,
                                       int line_num,
                                       size_t column,
                                       bool has_column,
                                       size_t byte_offset,
                                       const char *sep,
                                       size_t sep_len,
                                       FILE *out,
                                       bool color) {
    bool printed = false;

    if (opts->show_filename && display_name) {
        bool hyperlink_enabled = color
            && opts->hyperlink_format
            && opts->hyperlink_format[0] != '\0';
        char *hyperlink = NULL;

        if (!color && !opts->show_line_number && !opts->show_column
            && !opts->show_byte_offset && !opts->null_filename) {
            bx_search_fwrite_stream(out, display_name, display_name_len);
            stats_count_bytes(display_name_len);
            bx_search_fwrite_stream(out, sep, sep_len);
            stats_count_bytes(sep_len);
            return true;
        }

        if (hyperlink_enabled) {
            hyperlink = bx_rg_hyperlink_open_dup(opts->hyperlink_format,
                                                 opts->hostname_bin,
                                                 display_name,
                                                 (size_t)line_num,
                                                 column,
                                                 opts->show_line_number,
                                                 has_column && opts->show_column);
        }
        if (hyperlink)
            bx_search_fputs_stream(out, hyperlink);
        if (color)
            bx_rg_emit_color_style_start_file(out, &opts->rg_colors.path);
        bx_search_fwrite_stream(out, display_name, display_name_len);
        stats_count_bytes(display_name_len);
        if (color)
            bx_rg_emit_color_reset_file(out);
        if (hyperlink) {
            bx_search_fputs_stream(out, bx_rg_hyperlink_close());
            free(hyperlink);
        }
        if (opts->null_filename)
            bx_search_putc_stream(out, '\0');
        else
            bx_search_fwrite_stream(out, sep, sep_len);
        stats_count_bytes(sep_len);
        printed = true;
    }
    if (opts->show_line_number) {
        if (color)
            bx_rg_emit_color_style_start_file(out, &opts->rg_colors.line);
        int n = bx_search_printf_stream(out, opts->initial_tab ? "%2d" : "%d", line_num);
        if (n > 0) stats_count_bytes((size_t)n);
        if (color)
            bx_rg_emit_color_reset_file(out);
        bx_search_fwrite_stream(out, sep, sep_len);
        stats_count_bytes(sep_len);
        printed = true;
    }
    if (opts->show_column && has_column) {
        if (color)
            bx_rg_emit_color_style_start_file(out, &opts->rg_colors.column);
        int n = bx_search_printf_stream(out, opts->initial_tab ? "%2zu" : "%zu", column);
        if (n > 0) stats_count_bytes((size_t)n);
        if (color)
            bx_rg_emit_color_reset_file(out);
        bx_search_fwrite_stream(out, sep, sep_len);
        stats_count_bytes(sep_len);
        printed = true;
    }
    if (opts->show_byte_offset) {
        if (color)
            bx_rg_emit_color_style_start_file(out, &opts->rg_colors.line);
        int n = bx_search_printf_stream(out, opts->initial_tab ? "%2zu" : "%zu", byte_offset);
        if (n > 0) stats_count_bytes((size_t)n);
        if (color)
            bx_rg_emit_color_reset_file(out);
        bx_search_fwrite_stream(out, sep, sep_len);
        stats_count_bytes(sep_len);
        printed = true;
    }
    return printed;
}

static bool print_result_prefix(const char *display_name, struct search_opts *opts,
                                int line_num, size_t column, bool has_column,
                                size_t byte_offset, const char *sep) {
    FILE *out = current_output_ctx ? bx_search_output_stream() : stdout;
    bool color = bx_color_enabled();
    size_t display_name_len = display_name ? strlen(display_name) : 0u;
    size_t sep_len = (opts && opts->null_filename) ? 1u : strlen(sep);

    return print_result_prefix_cached(display_name, display_name_len,
                                      opts, line_num, column, has_column,
                                      byte_offset, sep, sep_len, out, color);
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

static void print_vimgrep_matches(const unsigned char *line, size_t len,
                                  const char *display_name, int line_num,
                                  size_t byte_offset,
                                  struct bx_matcher *m, struct search_opts *opts) {
    size_t match_len = record_match_len(line, len, opts);
    size_t start = 0u;
    FILE *out = current_output_ctx ? bx_search_output_stream() : stdout;
    bool color = bx_color_enabled();
    unsigned char delimiter = (unsigned char)record_delimiter(opts);
    bool has_delim = len > 0u && line[len - 1u] == delimiter;

    while (start <= match_len) {
        struct bx_match bm;
        if (matcher_find_with_opts(m, line, match_len, start, opts, &bm) != 0)
            break;
        bool prefix_printed = print_result_prefix(display_name, opts, line_num,
                                                  bm.start + 1u, true,
                                                  byte_offset + bm.start,
                                                  match_field_separator(opts));
        maybe_emit_initial_tab(opts, prefix_printed);
        if (should_omit_long_match_line(opts, len))
            print_omitted_long_line(opts);
        else if (opts->replace)
            print_replaced_record(line, len, m, opts);
        else
            print_match_colored_cached(line, len, bm.start, bm.end, has_delim,
                                       opts, out, color, delimiter);
        if (bm.end > bm.start)
            start = bm.end;
        else
            start = bm.start + 1u;
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
        bx_search_report_path_error(progname, filename, errno, opts);
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
            bx_search_report_path_error(progname, filename, errno, opts);
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

#define BX_SEARCH_SCANNER_MIN_FILE_SIZE 1u
#define BX_SEARCH_SCANNER_RAW_CHUNK_CAP 65536u

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
    if (bx_search_plan_needs_line_buffering(opts) || opts->replace || opts->only_matching
        || opts->passthru || opts->vimgrep)
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

static bool search_file_scanner_can_raw_shortcut_file_presence(const struct bx_matcher *m,
                                                               const struct search_opts *opts) {
    if (!search_file_scanner_can_shortcut_file_presence(opts))
        return false;
    if (!m || m->kind != MATCHER_LITERAL)
        return false;
    return !opts->line_regexp && !opts->word_regexp;
}

static bool search_file_needs_early_transform_load(const char *filename,
                                                   bool use_stdin,
                                                   const struct search_opts *opts) {
    if (!opts)
        return false;
    if (use_stdin)
        return bx_rg_transform_maybe_needed(opts, filename, true, fileno(stdin));
    return bx_rg_transform_needs_file_preload(opts, filename);
}

static bool search_file_opened_needs_auto_transform(FILE *f,
                                                    const struct search_opts *opts) {
    if (!f || !opts)
        return false;
    return bx_rg_transform_auto_encoding_needs_fd(opts, fileno(f));
}

static bool bx_search_scanner_reserve_raw_buffer(struct bx_search_scanner *scanner,
                                                 size_t needed) {
    if (!scanner)
        return false;
    if (scanner->cap >= needed)
        return true;

    size_t new_cap = scanner->cap == 0u ? BX_SEARCH_SCANNER_RAW_CHUNK_CAP : scanner->cap;
    while (new_cap < needed) {
        if (new_cap > (SIZE_MAX / 2u))
            return false;
        new_cap *= 2u;
    }

    unsigned char *tmp = realloc(scanner->buf, new_cap);
    if (!tmp)
        return false;
    scanner->buf = tmp;
    scanner->cap = new_cap;
    return true;
}

static int search_file_raw_presence_opened(FILE *f,
                                           bool use_stdin,
                                           const char *filename,
                                           const char *display_name,
                                           const char *progname,
                                           struct bx_matcher *m,
                                           struct search_opts *opts,
                                           int *match_count,
                                           struct bx_search_scanner *scanner,
                                           struct bx_record_stream *record_stream,
                                           struct bx_search_stats *stats) {
    int status = 1;
    int file_matches = 0;
    size_t plen;
    size_t overlap;
    size_t carry = 0u;
    bool counted_file = false;
    bool first_chunk = true;

    if (!f || !m || !opts || !scanner)
        return 2;

    plen = bx_literal_len(m->literal);
    overlap = plen > 0u ? plen - 1u : 0u;
    scanner->len = 0u;
    if (!bx_search_scanner_reserve_raw_buffer(scanner,
                                              BX_SEARCH_SCANNER_RAW_CHUNK_CAP + overlap)) {
        if (!use_stdin)
            fclose(f);
        return 2;
    }

    for (;;) {
        if (carry > 0u)
            memmove(scanner->buf, scanner->buf + scanner->len - carry, carry);
        scanner->len = carry;

        size_t nread = fread(scanner->buf + carry, 1u, scanner->cap - carry, f);
        scanner->len += nread;
        bx_search_dev_counters_note_bytes_read(nread);

        if (first_chunk) {
            first_chunk = false;
            if (filename &&
                bx_rg_transform_auto_encoding_needs_prefix(opts, scanner->buf, scanner->len)) {
                unsigned char *transformed = NULL;
                size_t transformed_len = 0u;
                enum bx_rg_transform_result transform_rc;

                if (!use_stdin)
                    fclose(f);
                transform_rc = bx_rg_load_transformed_input(filename, progname, opts,
                                                            bx_search_error_stream(),
                                                            &transformed, &transformed_len);
                if (transform_rc == BX_RG_TRANSFORM_NO_MATCH)
                    return 1;
                if (transform_rc == BX_RG_TRANSFORM_ERROR)
                    return 2;
                return search_transformed_buffer(transformed, transformed_len, display_name,
                                                 progname, m, opts, match_count,
                                                 record_stream, stats);
            }
        }

        if (!counted_file) {
            counted_file = true;
            if (stats)
                stats->files_searched++;
        }
        if (stats)
            stats->bytes_searched += nread;

        if (scanner->len > 0u) {
            struct bx_match bm;
            if (bx_literal_find(m->literal, scanner->buf, scanner->len, 0u, &bm) == 0) {
                bx_search_dev_counters_note_candidate_hit();
                bx_search_dev_counters_note_matcher_invocation();
                file_matches = 1;
                if (stats) {
                    stats->matches++;
                    stats->matched_lines++;
                    stats->files_with_matches++;
                }
                status = 0;
                break;
            }
        }

        if (nread == 0u) {
            if (ferror(f)) {
                bx_search_report_path_error(progname, display_name, errno ? errno : EIO, opts);
                status = 2;
            }
            break;
        }

        carry = overlap < scanner->len ? overlap : scanner->len;
    }

    if (status != 2) {
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
        if (match_count)
            *match_count += file_matches;
    }

    if (!use_stdin)
        fclose(f);
    return status;
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

    bool want_group_separator = bx_search_plan_needs_line_buffering(opts);
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
            if (opts->vimgrep && !opts->invert_match) {
                maybe_print_heading(display_name, opts, &heading_printed_for_file);
                print_vimgrep_matches((unsigned char *)lines[i].text, lines[i].len,
                                      heading_printed_for_file ? NULL : display_name,
                                      i + 1, lines[i].byte_offset, m, opts);
            } else if (opts->only_matching && !opts->invert_match) {
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
    bool shortcut_file_presence = search_file_scanner_can_shortcut_file_presence(opts);
    bool need_line_numbers = opts->show_line_number;
    bool exact_literal_candidates = !opts->word_regexp
        && !opts->line_regexp
        && bx_literal_candidates_are_exact(m->literal);
    bool heading_enabled = use_heading_output(display_name, opts);
    bool need_initial_tab = opts->initial_tab;
    bool can_omit_long_line = opts->max_columns > 0 && !opts->only_matching;
    bool color = bx_color_enabled();
    bool fast_plain_line_output = !color
        && !opts->trim
        && !opts->only_matching
        && !need_line_numbers
        && !opts->show_column
        && !opts->show_byte_offset
        && !need_initial_tab;
    const char *match_sep = match_field_separator(opts);
    size_t match_sep_len = opts->null_filename ? 1u : strlen(match_sep);
    size_t display_name_len = display_name ? strlen(display_name) : 0u;
    unsigned char delimiter = (unsigned char)record_delimiter(opts);
    FILE *out = NULL;
    bool output_is_captured = current_output_ctx
        && current_output_ctx->capture_out_buf
        && current_output_ctx->capture_out_len;
    char *fast_plain_prefix = NULL;
    size_t fast_plain_prefix_len = 0u;

    if (search_file_scanner_can_raw_shortcut_file_presence(m, opts))
        return search_file_raw_presence_opened(f, use_stdin, NULL, display_name, progname,
                                               m, opts, match_count, scanner, NULL, stats);

    if (stats)
        stats->files_searched++;

    if (fast_plain_line_output && !heading_enabled && opts->show_filename && display_name) {
        fast_plain_prefix_len = display_name_len + (opts->null_filename ? 1u : match_sep_len);
        if (fast_plain_prefix_len > 0u) {
            fast_plain_prefix = malloc(fast_plain_prefix_len);
            if (fast_plain_prefix) {
                if (display_name_len > 0u)
                    memcpy(fast_plain_prefix, display_name, display_name_len);
                if (opts->null_filename)
                    fast_plain_prefix[display_name_len] = '\0';
                else if (match_sep_len > 0u)
                    memcpy(fast_plain_prefix + display_name_len, match_sep, match_sep_len);
            } else {
                fast_plain_prefix_len = 0u;
            }
        }
    }

    bx_search_scanner_begin_file(scanner, (char)delimiter, need_line_numbers);
    while (!stop && bx_search_scanner_read_chunk(scanner, f)) {
        size_t next_line_num = scanner->records_before_buf + 1u;
        size_t numbered_until = 0u;

        if (stats)
            stats->bytes_searched += scanner->scan_len;

        size_t cursor = 0u;
        while (!stop) {
            struct bx_search_candidate candidate;
            if (!bx_search_scanner_next_literal_candidate(scanner, m->literal, &cursor, &candidate))
                break;

            struct bx_match bm;
            if (shortcut_file_presence) {
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

            size_t candidate_record_off = candidate.chunk_off - record.chunk_off;
            size_t match_len = 0u;
            if (exact_literal_candidates) {
                bm.start = candidate_record_off;
                bm.end = candidate_record_off + candidate.anchor_len;
            } else {
                match_len = record_match_len(record.data, record.len, opts);
                if (!matcher_verify_literal_candidate_with_opts(m, record.data, match_len,
                                                                candidate_record_off, opts, &bm)) {
                    if (matcher_find_with_opts(m, record.data, match_len, 0, opts, &bm) != 0)
                        continue;
                }
            }

            cursor = record.chunk_off + record.len;
            size_t line_num = 0u;
            if (need_line_numbers) {
                if (record.chunk_off > numbered_until) {
                    next_line_num += bx_search_scanner_count_delimiters_range(scanner,
                                                                              numbered_until,
                                                                              record.chunk_off);
                }
                line_num = next_line_num;
                numbered_until = record.chunk_off + record.len;
                next_line_num++;
            }
            int record_match_count = 1;
            if (opts->count_matches) {
                if (match_len == 0u)
                    match_len = record_match_len(record.data, record.len, opts);
                record_match_count = count_record_matches(m, record.data, match_len, opts);
            }
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

            if (heading_enabled)
                maybe_print_heading(display_name, opts, &heading_printed_for_file);
            if (!out)
                out = current_output_ctx ? bx_search_output_stream() : stdout;
            if (can_omit_long_line && (int)record.len > opts->max_columns)
                print_omitted_long_line(opts);
            else if (fast_plain_line_output) {
                const char *prefix_name = heading_printed_for_file ? NULL : display_name;
                size_t prefix_name_len = heading_printed_for_file ? 0u : display_name_len;
                size_t printed_bytes = 0u;

                if (!output_is_captured)
                    flockfile(out);
                if (fast_plain_prefix && !heading_printed_for_file) {
                    printed_bytes += fwrite_unlocked(fast_plain_prefix, 1u,
                                                     fast_plain_prefix_len, out);
                } else if (opts->show_filename && prefix_name) {
                    if (prefix_name_len > 0u) {
                        printed_bytes += fwrite_unlocked(prefix_name, 1u, prefix_name_len, out);
                    }
                    if (opts->null_filename) {
                        if (putc_unlocked('\0', out) != EOF)
                            printed_bytes++;
                    } else if (match_sep_len > 0u) {
                        printed_bytes += fwrite_unlocked(match_sep, 1u, match_sep_len, out);
                    }
                }
                if (record.len > 0u)
                    printed_bytes += fwrite_unlocked(record.data, 1u, record.len, out);
                if (!record.has_delim && putc_unlocked((int)delimiter, out) != EOF)
                    printed_bytes++;
                if (!output_is_captured)
                    funlockfile(out);
                if (printed_bytes > 0u)
                    bx_search_note_stdout_output();
                stats_count_bytes(printed_bytes);
                bx_search_dev_counters_note_output_line_emitted();
            } else {
                bool prefix_printed = print_result_prefix_cached(
                    heading_printed_for_file ? NULL : display_name,
                    heading_printed_for_file ? 0u : display_name_len,
                    opts, (int)line_num, bm.start + 1u, true,
                    (size_t)record.file_off,
                    match_sep, match_sep_len, out, color);
                if (need_initial_tab && prefix_printed) {
                    bx_search_putc_stream(out, '\t');
                    stats_count_bytes(1u);
                }
                print_match_colored_cached(record.data, record.len, bm.start, bm.end,
                                           record.has_delim, opts, out, color, delimiter);
            }

            if (opts->max_count > 0 && file_matches >= opts->max_count)
                stop = true;
        }
    }

    if (ferror(f)) {
        free(fast_plain_prefix);
        bx_search_report_path_error(progname, display_name, errno ? errno : EIO, opts);
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
    free(fast_plain_prefix);
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
            if (opts->vimgrep && !opts->invert_match) {
                maybe_print_heading(display_name, opts, &heading_printed_for_file);
                print_vimgrep_matches((unsigned char *)line, (size_t)len,
                                      heading_printed_for_file ? NULL : display_name,
                                      line_num, line_offset, m, opts);
                stdout_emitted = true;
            } else if (opts->only_matching && !opts->invert_match) {
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

    if (bx_search_plan_needs_line_buffering(opts) || bx_search_plan_plain_output_needs_binary_sensitive_path(opts))
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
        bx_search_report_path_error(progname, display_name ? display_name : "(memory)", errno, opts);
        return 2;
    }

    if (bx_search_plan_needs_line_buffering(opts)
        || bx_search_plan_plain_output_needs_binary_sensitive_path(opts)) {
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
    if (!opts->recursive && !use_stdin && filename && strcmp(filename, "-") != 0)
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

    if (search_file_needs_early_transform_load(filename, use_stdin, opts)) {
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
            bx_search_report_path_error(progname, filename, EISDIR, opts);
            result = 2;
            goto out;
        }
    }

    const bool line_buffered_stdin_streaming =
        use_stdin && opts->line_buffered && !opts->null_data && !opts->binary_as_text &&
        !opts->quiet && !opts->count_only &&
        !opts->files_with_matches && !opts->files_without_match;

    if (use_stdin && !line_buffered_stdin_streaming &&
        bx_search_plan_plain_output_needs_binary_sensitive_path(opts)) {
        result = search_file_buffered(filename, display_name, progname, m, opts,
                                      match_count, record_stream, stats);
        goto out;
    }

    if (!use_stdin && !opts->null_data && !opts->binary_as_text) {
        FILE *f = open_search_input_stream(filename, progname, opts, record_stream, NULL);
        if (!f)
            goto out_error;

        if (search_file_scanner_can_raw_shortcut_file_presence(m, opts)) {
            result = search_file_raw_presence_opened(f, false, filename, display_name, progname,
                                                     m, opts, match_count, scanner,
                                                     record_stream, stats);
            goto out;
        }

        if (search_file_opened_needs_auto_transform(f, opts)) {
            unsigned char *transformed = NULL;
            size_t transformed_len = 0u;
            enum bx_rg_transform_result transform_rc;

            fclose(f);
            transform_rc = bx_rg_load_transformed_input(filename, progname, opts,
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
            result = search_transformed_buffer(transformed, transformed_len, display_name,
                                               progname, m, opts, match_count,
                                               record_stream, stats);
            goto out;
        }

        bool is_binary_file = false;
        if (bx_record_stream_probe_binary_prefix(f, &is_binary_file)) {
            if (is_binary_file) {
                if (opts->binary_without_match) {
                    fclose(f);
                    result = search_binary_without_match(display_name, opts, match_count, stats);
                    goto out;
                }

                if (opts->quiet || opts->files_with_matches || opts->files_without_match || opts->count_only) {
                    if (bx_search_plan_needs_line_buffering(opts))
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

            if (bx_search_plan_needs_line_buffering(opts))
                result = search_file_buffered_opened(f, false, display_name, progname, m, opts,
                                                     match_count, record_stream, stats);
            if (search_file_can_use_scanner(m, opts, false)
                && search_file_scanner_stream_is_eligible(f)) {
                result = search_file_scanner_opened(f, false, display_name, progname, m, opts,
                                                    match_count, scanner, stats);
                goto out;
            }
            if (bx_search_plan_needs_line_buffering(opts))
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
            if (bx_search_plan_needs_line_buffering(opts))
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

    if (bx_search_plan_needs_line_buffering(opts))
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

int bx_search_search_file(const char *filename,
                          const char *display_name_override,
                          const char *progname,
                          struct bx_matcher *m,
                          struct search_opts *opts,
                          int *match_count,
                          struct bx_search_scanner *scanner,
                          struct bx_record_stream *record_stream,
                          struct bx_search_stats *stats) {
    return search_file(filename, display_name_override, progname, m, opts,
                       match_count, scanner, record_stream, stats);
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

/* --- recursive search using shared walker --- */

static const char *const rg_ignore_filenames[] = {
    ".gitignore",
    ".ignore",
    ".rgignore",
};

struct bx_walk_opts bx_search_make_walk_opts(const char *progname,
                                                    enum bx_search_personality personality,
                                                    const struct search_opts *opts,
                                                    bool *stop) {
    return (struct bx_walk_opts){
        .sort_entries = bx_search_use_rg_sort_policy(personality, opts),
        .reverse_sort = bx_search_sort_is_path(opts) && bx_search_sort_is_descending(opts),
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

struct bx_walk_filter_opts bx_search_make_filter_opts(const struct search_opts *opts) {
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

struct bx_walk_ignore_opts bx_search_make_ignore_opts(const char *progname,
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

bool bx_search_explicit_entry_selected(const struct search_opts *opts,
                                       const char *path) {
    const char *name = bx_path_basename_ptr(path);

    if (opts->num_include > 0) {
        struct bx_walk_filter_opts filter_opts = {
            .hidden = opts->hidden,
            .glob_case_insensitive = opts->glob_case_insensitive,
            .include_patterns = opts->include_patterns,
            .include_pattern_casefold = opts->include_pattern_casefold,
            .num_include_patterns = opts->num_include,
        };
        struct bx_walk_filter_state filter_state;
        bx_walk_filter_init(&filter_state, &filter_opts, path);
        if (!bx_walk_filter_matches_include(&filter_state, name, path))
            return false;
    }

    for (int i = 0; i < opts->num_exclude; i++) {
        int flags = opts->glob_case_insensitive ? FNM_CASEFOLD : 0;
        if (fnmatch(opts->exclude_patterns[i], name, flags) == 0)
            return false;
    }

    return true;
}

/* --- main entry point --- */

int bx_search_main(int argc, char **argv, enum bx_search_personality personality) {
    struct search_opts opts;
    struct bx_search_plan plan = {0};
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

    int num_files = argc - first_file;
    bool rg_searches_stdin = (bx_search_personality_is_rg(personality)
                              && !opts.files_only
                              && num_files == 0
                              && bx_search_run_should_search_stdin());
    bx_search_plan_build(&plan, personality, &opts, num_files, rg_searches_stdin);
    if (bx_search_personality_is_rg(personality) && bx_search_plan_debug_enabled())
        bx_search_plan_debug_dump(stderr, &plan);
    struct bx_search_stats stats = {0};
    struct bx_search_output_ctx main_output_ctx = {
        .out = stdout,
        .err = stderr,
        .stats = opts.stats ? &stats : NULL,
    };
    struct bx_search_output_ctx *previous_output_ctx = bx_search_output_ctx_push(&main_output_ctx);

    struct bx_search_run_args run_args = {
        .argc = argc,
        .argv = argv,
        .first_file = first_file,
        .pattern = pattern,
        .progname = progname,
        .personality = personality,
        .plan = &plan,
        .opts = &opts,
        .stats = &stats,
    };
    struct bx_search_run_result run_result = {0};

    bx_search_run(&run_args, &run_result);
    if (opts.stats && run_result.ran_search)
        print_stats_summary(&stats);
    bx_search_output_ctx_pop(previous_output_ctx);
    bx_search_free_options(&opts);
    return finish_search_main(run_result.status);
}
