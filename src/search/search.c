#define _GNU_SOURCE
#include <errno.h>
#include <fnmatch.h>
#include <stdbool.h>
#include <stdio.h>
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
#include "search_buffered.h"
#include "search_input.h"
#include "search_multiline.h"
#include "options.h"
#include "rg_parallel.h"
#include "search_raw_presence.h"
#include "search_scanner.h"
#include "search_streaming.h"
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
#include "rg_transform.h"
#include "scanner.h"
#include "sort.h"
#include "traverse.h"
#include "bx/diag.h"

static bool progname_uses_os_error_style(const char *progname) {
    if (!progname) return false;
    progname = bx_cli_progname(progname, "grep");
    return strcmp(progname, "rg") == 0;
}

bool bx_search_progname_uses_os_error_style(const char *progname) {
    return progname_uses_os_error_style(progname);
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
        fprintf(bx_search_error_output_stream(), "%s: %s: %s (os error %d)\n",
                progname, path, strerror(errnum), errnum);
    else
        fprintf(bx_search_error_output_stream(), "%s: %s: %s\n", progname, path, strerror(errnum));
}

static void report_binary_match(const char *progname, const char *path) {
    fprintf(bx_search_error_output_stream(), "%s: %s: binary file matches\n", progname, path);
}

void bx_search_report_binary_match(const char *progname, const char *path) {
    report_binary_match(progname, path);
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

int bx_search_search_transformed_buffer(unsigned char *buf, size_t len,
                                        const char *display_name,
                                        const char *progname,
                                        struct bx_matcher *m,
                                        struct search_opts *opts,
                                        int *match_count,
                                        struct bx_record_stream *record_stream,
                                        struct bx_search_stats *stats);
char bx_search_record_delimiter(const struct search_opts *opts);
size_t bx_search_record_match_len(const unsigned char *buf, size_t len, const struct search_opts *opts);
int bx_search_matcher_find_with_opts(struct bx_matcher *m, const unsigned char *buf, size_t len,
                                  size_t start, struct search_opts *opts, struct bx_match *out);

/* --- unified matcher (regex or literal) --- */

enum matcher_kind {
    MATCHER_REGEX,
    MATCHER_POSIX,
    MATCHER_LITERAL,
    MATCHER_LITERAL_SET,
};

struct bx_literal_set {
    struct bx_literal_matcher **items;
    size_t count;
};

struct bx_matcher {
    enum matcher_kind kind;
    union {
        struct bx_regex *regex;
        struct bx_literal_matcher *literal;
        struct bx_literal_set literal_set;
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
    if (m->kind == MATCHER_LITERAL_SET) {
        struct bx_match best = {0};
        bool found = false;

        for (size_t i = 0; i < m->literal_set.count; ++i) {
            struct bx_match candidate = {0};

            if (bx_literal_find(m->literal_set.items[i], buf, len, start, &candidate) != 0)
                continue;
            if (!found || candidate.start < best.start ||
                (candidate.start == best.start && candidate.end < best.end)) {
                best = candidate;
                found = true;
            }
        }

        if (!found)
            return -1;
        *out = best;
        return 0;
    }
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

int bx_search_matcher_find_with_opts(struct bx_matcher *m, const unsigned char *buf, size_t len,
                                  size_t start, struct search_opts *opts, struct bx_match *out) {
    bx_search_dev_counters_note_matcher_invocation();
    if (opts->line_regexp &&
        (m->kind == MATCHER_LITERAL || m->kind == MATCHER_LITERAL_SET)) {
        if (start != 0u)
            return -1;
        if (m->kind == MATCHER_LITERAL) {
            if (!bx_literal_verify_at(m->literal, buf, len, 0u, out))
                return -1;
            return out->start == 0u && out->end == len ? 0 : -1;
        }

        for (size_t i = 0; i < m->literal_set.count; ++i) {
            if (!bx_literal_verify_at(m->literal_set.items[i], buf, len, 0u, out))
                continue;
            if (out->start == 0u && out->end == len)
                return 0;
        }
        return -1;
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

bool bx_search_matcher_verify_literal_candidate_with_opts(struct bx_matcher *m,
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
    else if (m->kind == MATCHER_LITERAL_SET) {
        for (size_t i = 0; i < m->literal_set.count; ++i)
            bx_literal_free(m->literal_set.items[i]);
        free(m->literal_set.items);
    }
    else if (m->kind == MATCHER_POSIX)
        regfree(&m->posix);
    else
        bx_regex_free(m->regex);
    free(m);
}

void bx_search_matcher_free(struct bx_matcher *m) {
    matcher_free(m);
}

struct bx_literal_matcher *bx_search_matcher_literal(const struct bx_matcher *m) {
    if (!m || m->kind != MATCHER_LITERAL)
        return NULL;
    return m->literal;
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
    size_t literal_pattern_count = 1u;

    if (opts->num_extra_patterns > 0 && !bx_search_personality_is_rg(personality) &&
        opts->perl_regexp) {
        if (errmsg && !*errmsg)
            *errmsg = strdup("the -P option only supports a single pattern");
        return NULL;
    }

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

    if (opts->fixed_strings) {
        literal_pattern_count = 1u + (size_t)opts->num_extra_patterns;
        if (literal_pattern_count > 1u) {
            m->literal_set.items = calloc(literal_pattern_count,
                                          sizeof(*m->literal_set.items));
            if (!m->literal_set.items) {
                free(wrapped);
                free(m);
                return NULL;
            }

            if (bx_literal_compile(&m->literal_set.items[0], final_pattern,
                                   (flags & BX_REGEX_ICASE) != 0) != 0) {
                if (errmsg && !*errmsg)
                    *errmsg = strdup("empty fixed-string pattern is not supported");
                matcher_free(m);
                free(wrapped);
                return NULL;
            }
            m->literal_set.count = 1u;

            for (int k = 0; k < opts->num_extra_patterns; ++k) {
                if (bx_literal_compile(&m->literal_set.items[m->literal_set.count],
                                       opts->extra_patterns[k],
                                       (flags & BX_REGEX_ICASE) != 0) != 0) {
                    if (errmsg && !*errmsg)
                        *errmsg = strdup("empty fixed-string pattern is not supported");
                    matcher_free(m);
                    free(wrapped);
                    return NULL;
                }
                m->literal_set.count++;
            }

            m->kind = MATCHER_LITERAL_SET;
            free(wrapped);
            return m;
        }
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

bool bx_search_matcher_is_scanner_literal_eligible(const struct bx_matcher *m,
                                                const struct search_opts *opts) {
    if (!m || !opts || m->kind != MATCHER_LITERAL)
        return false;
    return !bx_literal_contains_byte(m->literal, (unsigned char)bx_search_record_delimiter(opts));
}

/* --- shared record helpers --- */

char bx_search_record_delimiter(const struct search_opts *opts) {
    return opts->null_data ? '\0' : '\n';
}

size_t bx_search_record_match_len(const unsigned char *buf, size_t len, const struct search_opts *opts) {
    return bx_rg_record_match_len(buf, len, bx_search_record_delimiter(opts), opts->crlf);
}

static int finish_search_main(int status) {
    bx_search_dev_counters_report(stderr);
    bx_search_dev_counters_reset();
    return status;
}

int bx_search_count_record_matches(struct bx_matcher *m, const unsigned char *buf, size_t len,
                                struct search_opts *opts) {
    size_t start = 0;
    int count = 0;
    while (start <= len) {
        struct bx_match bm;
        if (bx_search_matcher_find_with_opts(m, buf, len, start, opts, &bm) != 0)
            break;
        count++;
        if (bm.end > bm.start)
            start = bm.end;
        else
            start = bm.start + 1;
    }
    return count;
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
    FILE *f = bx_search_input_open_stream(filename, progname, opts, record_stream, &use_stdin);
    if (!f)
        return 2;
    if (!bx_search_scanner_stream_is_eligible(f))
        return bx_search_streaming_opened(f, use_stdin, display_name, progname, m, opts,
                                          match_count, record_stream, stats);

    return bx_search_scanner_opened(f, use_stdin, display_name, progname, m, opts,
                                    match_count, scanner, stats);
}

enum bx_search_file_kernel_kind {
    BX_SEARCH_FILE_KERNEL_MULTILINE = 0,
    BX_SEARCH_FILE_KERNEL_RAW_PRESENCE,
    BX_SEARCH_FILE_KERNEL_SCANNER,
    BX_SEARCH_FILE_KERNEL_BUFFERED,
    BX_SEARCH_FILE_KERNEL_STREAMING,
};

static enum bx_search_file_kernel_kind
search_file_select_buffered_memory_kernel(const struct search_opts *opts) {
    if (opts->multiline)
        return BX_SEARCH_FILE_KERNEL_MULTILINE;
    if (bx_search_plan_needs_line_buffering(opts)
        || bx_search_plan_plain_output_needs_binary_sensitive_path(opts)) {
        return BX_SEARCH_FILE_KERNEL_BUFFERED;
    }
    return BX_SEARCH_FILE_KERNEL_STREAMING;
}

static enum bx_search_file_kernel_kind
search_file_select_opened_kernel(FILE *f,
                                 struct bx_matcher *m,
                                 struct search_opts *opts,
                                 bool use_stdin) {
    if (opts->multiline)
        return BX_SEARCH_FILE_KERNEL_MULTILINE;
    if (bx_search_plan_needs_line_buffering(opts)
        || bx_search_plan_plain_output_needs_binary_sensitive_path(opts)) {
        return BX_SEARCH_FILE_KERNEL_BUFFERED;
    }
    if (bx_search_scanner_can_use(m, opts, use_stdin)
        && bx_search_scanner_stream_is_eligible(f)) {
        return BX_SEARCH_FILE_KERNEL_SCANNER;
    }
    return BX_SEARCH_FILE_KERNEL_STREAMING;
}

static enum bx_search_file_kernel_kind
search_file_select_opened_nonbinary_kernel(FILE *f,
                                           struct bx_matcher *m,
                                           struct search_opts *opts,
                                           bool use_stdin) {
    if (opts->multiline)
        return BX_SEARCH_FILE_KERNEL_MULTILINE;
    if (bx_search_plan_needs_line_buffering(opts))
        return BX_SEARCH_FILE_KERNEL_BUFFERED;
    if (bx_search_scanner_can_use(m, opts, use_stdin)
        && bx_search_scanner_stream_is_eligible(f)) {
        return BX_SEARCH_FILE_KERNEL_SCANNER;
    }
    return BX_SEARCH_FILE_KERNEL_STREAMING;
}

static enum bx_search_file_kernel_kind
search_file_select_path_kernel(struct bx_matcher *m,
                               struct search_opts *opts,
                               bool use_stdin) {
    if (opts->multiline)
        return BX_SEARCH_FILE_KERNEL_MULTILINE;
    if ((use_stdin
         && !bx_search_streaming_uses_line_buffered_stdin(opts, use_stdin)
         && bx_search_plan_plain_output_needs_binary_sensitive_path(opts))
        || bx_search_plan_needs_line_buffering(opts)) {
        return BX_SEARCH_FILE_KERNEL_BUFFERED;
    }
    if (bx_search_scanner_can_use(m, opts, use_stdin))
        return BX_SEARCH_FILE_KERNEL_SCANNER;
    return BX_SEARCH_FILE_KERNEL_STREAMING;
}

static enum bx_search_file_kernel_kind
search_file_select_binary_search_kernel(const struct search_opts *opts) {
    return bx_search_plan_needs_line_buffering(opts)
        ? BX_SEARCH_FILE_KERNEL_BUFFERED
        : BX_SEARCH_FILE_KERNEL_STREAMING;
}

static int search_file_run_opened_kernel(enum bx_search_file_kernel_kind kernel,
                                         FILE *f,
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
    switch (kernel) {
    case BX_SEARCH_FILE_KERNEL_MULTILINE:
        return bx_search_multiline_opened(f, use_stdin, display_name, m, opts,
                                          match_count, stats);
    case BX_SEARCH_FILE_KERNEL_RAW_PRESENCE:
        return bx_search_raw_presence_opened(f, use_stdin, filename, display_name, progname,
                                               m, opts, match_count, scanner,
                                               record_stream, stats);
    case BX_SEARCH_FILE_KERNEL_SCANNER:
        return bx_search_scanner_opened(f, use_stdin, display_name, progname, m, opts,
                                        match_count, scanner, stats);
    case BX_SEARCH_FILE_KERNEL_BUFFERED:
        return bx_search_buffered_opened(f, use_stdin, display_name, progname, m, opts,
                                         match_count, record_stream, stats);
    case BX_SEARCH_FILE_KERNEL_STREAMING:
        return bx_search_streaming_opened(f, use_stdin, display_name, progname, m, opts,
                                          match_count, record_stream, stats);
    }
    if (!use_stdin)
        fclose(f);
    return 2;
}

static int search_file_run_path_kernel(enum bx_search_file_kernel_kind kernel,
                                       const char *filename,
                                       const char *display_name,
                                       const char *progname,
                                       struct bx_matcher *m,
                                       struct search_opts *opts,
                                       int *match_count,
                                       struct bx_search_scanner *scanner,
                                       struct bx_record_stream *record_stream,
                                       struct bx_search_stats *stats) {
    switch (kernel) {
    case BX_SEARCH_FILE_KERNEL_MULTILINE:
        return bx_search_multiline_path(filename, display_name, progname, m, opts,
                                        match_count, stats);
    case BX_SEARCH_FILE_KERNEL_SCANNER:
        return search_file_scanner(filename, display_name, progname, m, opts,
                                   match_count, scanner, record_stream, stats);
    case BX_SEARCH_FILE_KERNEL_BUFFERED:
        return bx_search_buffered_path(filename, display_name, progname, m, opts,
                                       match_count, record_stream, stats);
    case BX_SEARCH_FILE_KERNEL_STREAMING:
        return bx_search_streaming_path(filename, display_name, progname, m, opts,
                                        match_count, record_stream, stats);
    case BX_SEARCH_FILE_KERNEL_RAW_PRESENCE:
        break;
    }
    return 2;
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
    enum bx_search_file_kernel_kind kernel =
        search_file_select_opened_kernel(f, m, opts, use_stdin);

    return search_file_run_opened_kernel(kernel, f, use_stdin, NULL, display_name, progname,
                                         m, opts, match_count, scanner, record_stream, stats);
}

int bx_search_binary_without_match(const char *display_name,
                                   struct search_opts *opts,
                                   int *match_count,
                                   struct bx_search_stats *stats) {
    if (stats)
        stats->files_searched++;
    if (opts->count_only)
        bx_search_print_count_result(display_name, opts, 0);
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

    while ((len = bx_search_input_read_record(f, record_stream, opts)) != -1) {
        char *line = record_stream->record;
        struct bx_match bm;
        size_t match_len = bx_search_record_match_len((unsigned char *)line, (size_t)len, opts);
        if (!opts->null_data && memchr(line, '\0', match_len) != NULL) {
            size_t chunk_start = 0;
            matched = false;
            while (chunk_start <= match_len) {
                size_t chunk_len = 0;
                while (chunk_start + chunk_len < match_len &&
                       line[chunk_start + chunk_len] != '\0') {
                    chunk_len++;
                }
                matched = bx_search_matcher_find_with_opts(
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
            matched = bx_search_matcher_find_with_opts(m, (unsigned char *)line, match_len,
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
    FILE *f = bx_search_input_fopen(filename, opts);
    if (!f)
        return false;
    bx_search_dev_counters_note_file_opened();

    bx_record_stream_prepare_file(f, record_stream);
    bool matched = binary_file_matches_opened(f, m, opts, record_stream);
    fclose(f);
    return matched;
}

int bx_search_search_transformed_buffer(unsigned char *buf, size_t len,
                                        const char *display_name,
                                        const char *progname,
                                        struct bx_matcher *m,
                                        struct search_opts *opts,
                                        int *match_count,
                                        struct bx_record_stream *record_stream,
                                        struct bx_search_stats *stats) {
    enum bx_search_file_kernel_kind kernel =
        search_file_select_buffered_memory_kernel(opts);
    FILE *mem;

    if (kernel == BX_SEARCH_FILE_KERNEL_MULTILINE) {
        if (stats)
            stats->files_searched++;
        return bx_search_multiline_buffer(buf, len, display_name, m, opts, match_count, stats);
    }

    mem = fmemopen(buf, len, "r");
    if (!mem) {
        free(buf);
        bx_search_report_path_error(progname, display_name ? display_name : "(memory)", errno, opts);
        return 2;
    }

    int rc = search_file_run_opened_kernel(kernel, mem, false, NULL, display_name, progname,
                                           m, opts, match_count, NULL, record_stream, stats);
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
    int previous_offset_width = bx_search_output_get_offset_width();

    if (!display_name_override && filename && strcmp(filename, "-") != 0) {
        owned_display_name = bx_rg_display_path_dup(filename, false, opts->path_separator);
        if (owned_display_name)
            display_name = owned_display_name;
    }
    if (!opts->recursive && !use_stdin && filename && strcmp(filename, "-") != 0)
        operand_st_loaded = stat(filename, &operand_st) == 0;

    bx_search_output_set_offset_width(0);
    if (opts->initial_tab) {
        struct stat offset_width_st;

        if (use_stdin) {
            if (fstat(STDIN_FILENO, &offset_width_st) == 0) {
                bx_search_output_set_offset_width(
                    bx_search_compute_offset_width_from_stat(&offset_width_st, opts));
            }
        } else if (operand_st_loaded) {
            bx_search_output_set_offset_width(
                bx_search_compute_offset_width_from_stat(&operand_st, opts));
        } else if (filename && strcmp(filename, "-") != 0 &&
                   stat(filename, &offset_width_st) == 0) {
            bx_search_output_set_offset_width(
                bx_search_compute_offset_width_from_stat(&offset_width_st, opts));
        }
    }

    bx_rg_tracef(opts, "search: %s", display_name ? display_name : "(stdin)");

    if (operand_st_loaded && bx_search_mode_is_special_input(operand_st.st_mode)) {
        FILE *f;

        if (bx_search_should_skip_special_input_mode(operand_st.st_mode, opts)) {
            result = 1;
            goto out;
        }

        f = bx_search_input_open_stream(filename, progname, opts, record_stream, NULL);
        if (!f)
            goto out_error;

        result = search_file_opened_without_reopen(f, false, display_name, progname, m, opts,
                                                   match_count, scanner, record_stream, stats);
        goto out;
    }

    if (bx_search_input_needs_early_transform_load(filename, use_stdin, opts)) {
        unsigned char *transformed = NULL;
        size_t transformed_len = 0u;
        enum bx_rg_transform_result transform_rc =
            bx_rg_load_transformed_input(filename, progname, opts,
                                         bx_search_error_output_stream(),
                                         &transformed, &transformed_len);
        if (transform_rc == BX_RG_TRANSFORM_NO_MATCH) {
            result = 1;
            goto out;
        }
        if (transform_rc == BX_RG_TRANSFORM_ERROR) {
            result = 2;
            goto out;
        }
        result = bx_search_search_transformed_buffer(transformed, transformed_len, display_name, progname,
                                           m, opts, match_count, record_stream, stats);
        goto out;
    }

    if (opts->multiline) {
        result = search_file_run_path_kernel(BX_SEARCH_FILE_KERNEL_MULTILINE,
                                             filename, display_name, progname, m, opts,
                                             match_count, scanner, record_stream, stats);
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

    if (use_stdin && !bx_search_streaming_uses_line_buffered_stdin(opts, use_stdin) &&
        bx_search_plan_plain_output_needs_binary_sensitive_path(opts)) {
        result = search_file_run_path_kernel(BX_SEARCH_FILE_KERNEL_BUFFERED,
                                             filename, display_name, progname, m, opts,
                                             match_count, scanner, record_stream, stats);
        goto out;
    }

    if (!use_stdin && !opts->null_data && !opts->binary_as_text) {
        FILE *f = bx_search_input_open_stream(filename, progname, opts, record_stream, NULL);
        if (!f)
            goto out_error;

        if (bx_search_scanner_can_raw_shortcut_file_presence(m, opts)) {
            result = search_file_run_opened_kernel(BX_SEARCH_FILE_KERNEL_RAW_PRESENCE,
                                                   f, false, filename, display_name, progname,
                                                   m, opts, match_count, scanner,
                                                   record_stream, stats);
            goto out;
        }

        if (bx_search_input_opened_needs_auto_transform(f, opts)) {
            unsigned char *transformed = NULL;
            size_t transformed_len = 0u;
            enum bx_rg_transform_result transform_rc;

            fclose(f);
            transform_rc = bx_rg_load_transformed_input(filename, progname, opts,
                                                        bx_search_error_output_stream(),
                                                        &transformed, &transformed_len);
            if (transform_rc == BX_RG_TRANSFORM_NO_MATCH) {
                result = 1;
                goto out;
            }
            if (transform_rc == BX_RG_TRANSFORM_ERROR) {
                result = 2;
                goto out;
            }
            result = bx_search_search_transformed_buffer(transformed, transformed_len, display_name,
                                               progname, m, opts, match_count,
                                               record_stream, stats);
            goto out;
        }

        bool is_binary_file = false;
        if (bx_record_stream_probe_binary_prefix(f, &is_binary_file)) {
            if (is_binary_file) {
                if (opts->binary_without_match) {
                    fclose(f);
                    result = bx_search_binary_without_match(display_name, opts, match_count, stats);
                    goto out;
                }

                if (opts->quiet || opts->files_with_matches || opts->files_without_match || opts->count_only) {
                    result = search_file_run_opened_kernel(
                        search_file_select_binary_search_kernel(opts),
                        f, false, filename, display_name, progname, m, opts,
                        match_count, scanner, record_stream, stats);
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

            result = search_file_run_opened_kernel(
                search_file_select_opened_nonbinary_kernel(f, m, opts, false),
                f, false, filename, display_name, progname, m, opts,
                match_count, scanner, record_stream, stats);
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

    if (!use_stdin && !opts->null_data && !opts->binary_as_text &&
        bx_search_input_is_binary_path(filename, opts)) {
        if (opts->binary_without_match) {
            result = bx_search_binary_without_match(display_name, opts, match_count, stats);
            goto out;
        }

        if (opts->quiet || opts->files_with_matches || opts->files_without_match || opts->count_only) {
            result = search_file_run_path_kernel(search_file_select_binary_search_kernel(opts),
                                                 filename, display_name, progname, m, opts,
                                                 match_count, scanner, record_stream, stats);
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

    result = search_file_run_path_kernel(search_file_select_path_kernel(m, opts, use_stdin),
                                         filename, display_name, progname, m, opts,
                                         match_count, scanner, record_stream, stats);
    goto out;

out_error:
    result = 2;
out:
    bx_search_output_set_offset_width(previous_offset_width);
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
        bx_search_print_stats_summary(&stats);
    bx_search_output_ctx_pop(previous_output_ctx);
    bx_search_free_options(&opts);
    return finish_search_main(run_result.status);
}
