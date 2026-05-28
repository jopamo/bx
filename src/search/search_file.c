#define _GNU_SOURCE
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "dev_counters.h"
#include "literal.h"
#include "literal_scan.h"
#include "pcre2_matcher.h"
#include "rg_output.h"
#include "rg_transform.h"
#include "scanner.h"
#include "search_buffered.h"
#include "search_chunk_overlap.h"
#include "search_input.h"
#include "search_internal.h"
#include "search_multiline.h"
#include "search_plan.h"
#include "search_raw_presence.h"
#include "search_scanner.h"
#include "search_streaming.h"

static void search_file_report_record_stream_error(const char *progname,
                                                   const char *path,
                                                   struct bx_record_stream *record_stream,
                                                   const struct search_opts *opts);
static int binary_presence_opened(FILE *f,
                                  bool use_stdin,
                                  const char *display_name,
                                  const char *progname,
                                  struct bx_matcher *m,
                                  struct search_opts *opts,
                                  int *match_count,
                                  struct bx_record_stream *record_stream,
                                  struct bx_search_stats *stats);
static bool search_file_find_literal_precheck_match(const struct bx_lit_plan *absence_plan,
                                                    const unsigned char *buf,
                                                    size_t len,
                                                    size_t start,
                                                    struct bx_match *out);

#define BX_SEARCH_DEFERRED_CHUNK_CAP (256u * 1024u)
#define BX_SEARCH_DEFERRED_PREFIX_POLICY_CAP 1024u

enum bx_search_deferred_precheck_result {
    BX_SEARCH_DEFERRED_PRECHECK_UNSUPPORTED = -1,
    BX_SEARCH_DEFERRED_PRECHECK_POSSIBLE_MATCH = 0,
    BX_SEARCH_DEFERRED_PRECHECK_NO_MATCH = 1,
    BX_SEARCH_DEFERRED_PRECHECK_ERROR = 2,
    BX_SEARCH_DEFERRED_PRECHECK_TRANSFORM_NEEDED = 3,
};

struct bx_search_deferred_prefix_policy {
    bool transform_needed;
    bool binary_nul_found;
    size_t binary_nul_offset;
};

struct bx_search_display_name_state {
    const char *filename;
    const char *display_name_override;
    bool strip_dot_prefix;
    char *owned_display_name;
};

static bool search_file_stat_from_walk_entry(const struct bx_walk_entry *entry,
                                             const char *filename,
                                             struct stat *st) {
    if (!entry || !filename || !st || !entry->metadata_loaded)
        return false;
    /*
     * Walker metadata is valid only for the live entry that owns the path
     * buffer. Do not reuse metadata through an equal-by-text path copy.
     */
    if (entry->path != filename)
        return false;

    memset(st, 0, sizeof(*st));
    st->st_dev = entry->dev;
    st->st_ino = entry->inode;
    st->st_mode = entry->mode;
    st->st_nlink = entry->nlink;
    st->st_uid = entry->uid;
    st->st_gid = entry->gid;
    st->st_size = entry->size;
    st->st_blksize = entry->block_size;
    st->st_atim = entry->atime;
    st->st_mtim = entry->mtime;
    st->st_ctim = entry->ctime;
    return true;
}

static bool search_file_should_stat_path_for_slow_policy(const char *filename,
                                                         bool use_stdin,
                                                         const struct search_opts *opts) {
    if (!filename || use_stdin || strcmp(filename, "-") == 0)
        return false;
    if (!opts)
        return false;
    /*
     * Path stat fallback is cold policy work. Keep it out of the recursive
     * default literal path unless an explicit output option needs exact size,
     * or a non-recursive explicit operand needs exact type for directory and
     * special-file policy.
     */
    return opts->initial_tab || !opts->recursive;
}

static const char *display_name_for_stream(const char *filename,
                                           const char *display_name_override,
                                           struct search_opts *opts) {
    if (display_name_override)
        return display_name_override;
    if (!filename || strcmp(filename, "-") == 0)
        return opts->label ? opts->label : "(standard input)";
    return filename;
}

static const char *search_file_resolve_display_name(
    struct bx_search_display_name_state *state,
    struct search_opts *opts) {
    const char *fallback;

    if (!state)
        return NULL;
    fallback = display_name_for_stream(state->filename, state->display_name_override, opts);
    if (state->display_name_override || !state->filename || strcmp(state->filename, "-") == 0)
        return fallback;
    if (!state->strip_dot_prefix &&
        (!opts || opts->path_separator == '\0' || opts->path_separator == '/')) {
        return fallback;
    }
    if (!state->owned_display_name) {
        state->owned_display_name = bx_search_display_path_for_output(state->filename,
                                                                      state->strip_dot_prefix,
                                                                      opts);
    }
    return state->owned_display_name ? state->owned_display_name : fallback;
}

static enum bx_search_file_kernel_kind
search_file_resolve_opened_kernel(FILE *f,
                                  enum bx_search_file_kernel_kind desired_kernel) {
    if (desired_kernel != BX_SEARCH_FILE_KERNEL_SCANNER)
        return desired_kernel;
    return bx_search_scanner_stream_is_eligible(f)
        ? BX_SEARCH_FILE_KERNEL_SCANNER
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
                                             bx_search_matcher_absence_plan(m), opts,
                                             match_count, scanner, stats);
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
    case BX_SEARCH_FILE_KERNEL_SCANNER: {
        bool use_stdin = false;
        FILE *f = bx_search_input_open_stream(filename, progname, opts, record_stream, &use_stdin);

        if (!f)
            return 2;
        return search_file_run_opened_kernel(search_file_resolve_opened_kernel(f, kernel),
                                             f, use_stdin, filename, display_name, progname,
                                             m, opts, match_count, scanner,
                                             record_stream, stats);
    }
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

static bool search_file_find_literal_precheck_match(const struct bx_lit_plan *absence_plan,
                                                    const unsigned char *buf,
                                                    size_t len,
                                                    size_t start,
                                                    struct bx_match *out) {
    if (!absence_plan || !buf || !out || start > len)
        return false;

    size_t literal_match_off = SIZE_MAX;
    if (bx_literal_scan_absent(absence_plan, buf + start, len - start, &literal_match_off)
        != BX_LIT_FOUND) {
        return false;
    }

    out->start = start + literal_match_off;
    out->end = out->start + absence_plan->needle_len;
    return true;
}

static struct bx_search_deferred_prefix_policy
search_file_check_deferred_prefix_policy(struct search_opts *opts,
                                         const unsigned char *buf,
                                         size_t len) {
    struct bx_search_deferred_prefix_policy policy = {0};

    if (!opts || !buf || len == 0u)
        return policy;

    if (opts->encoding_mode == BX_RG_ENCODING_AUTO) {
        policy.transform_needed = bx_rg_transform_prefix_needs_decode(buf, len);
        if (policy.transform_needed)
            return policy;
    }

    if (!opts->null_data && !opts->binary_as_text) {
        bx_search_dev_counters_note_binary_prefix_check();
        const unsigned char *nul = memchr(buf, '\0', len);
        if (nul) {
            policy.binary_nul_found = true;
            policy.binary_nul_offset = (size_t)(nul - buf);
        }
    }
    return policy;
}

static int search_file_deferred_literal_precheck_path(const char *filename,
                                                      struct bx_search_display_name_state *display_name_state,
                                                      const char *progname,
                                                      struct bx_matcher *m,
                                                      const struct bx_search_exec_plan *exec_plan,
                                                      struct search_opts *opts,
                                                      struct bx_search_scanner *scanner,
                                                      struct bx_search_stats *stats) {
    const struct bx_lit_plan *absence_plan;
    int fd;
    int result = BX_SEARCH_DEFERRED_PRECHECK_UNSUPPORTED;
    size_t searched_len = 0u;
    bool prefix_policy_checked = false;

    if (!filename || !m || !exec_plan || !exec_plan->deferred_literal_precheck ||
        !opts || !scanner) {
        return BX_SEARCH_DEFERRED_PRECHECK_UNSUPPORTED;
    }
    absence_plan = bx_search_matcher_absence_plan(m);
    if (!absence_plan)
        return BX_SEARCH_DEFERRED_PRECHECK_UNSUPPORTED;

    fd = bx_search_input_open_fd(filename, opts);
    if (fd < 0) {
        bx_search_report_path_error(progname, filename, errno, opts);
        return BX_SEARCH_DEFERRED_PRECHECK_ERROR;
    }
    bx_search_dev_counters_note_file_opened();

    {
        size_t chunk_cap = opts->quiet ? BX_SEARCH_RAW_PRESENCE_CHUNK_CAP
                                       : BX_SEARCH_DEFERRED_CHUNK_CAP;
        size_t overlap = absence_plan->min_overlap_len;
        size_t carry = 0u;

        if (!bx_search_scanner_reserve(scanner, chunk_cap + overlap))
            goto out;
        for (;;) {
            scanner->len = bx_search_chunk_overlap_prepend_carry(scanner->buf,
                                                                 scanner->len,
                                                                 carry);

            size_t read_cap = (chunk_cap + overlap) - scanner->len;
            ssize_t nread;
            if (!prefix_policy_checked && read_cap > BX_SEARCH_DEFERRED_PREFIX_POLICY_CAP)
                read_cap = BX_SEARCH_DEFERRED_PREFIX_POLICY_CAP;
            do {
                nread = read(fd, scanner->buf + scanner->len, read_cap);
            } while (nread < 0 && errno == EINTR);
            if (nread < 0)
                goto out_error;
            scanner->len += (size_t)nread;
            searched_len += (size_t)nread;
            bx_search_dev_counters_note_content_read((size_t)nread);

            if (!prefix_policy_checked && nread > 0) {
                struct bx_search_deferred_prefix_policy prefix_policy;
                prefix_policy_checked = true;
                prefix_policy = search_file_check_deferred_prefix_policy(opts,
                                                                         scanner->buf,
                                                                         scanner->len);
                if (prefix_policy.transform_needed) {
                    result = BX_SEARCH_DEFERRED_PRECHECK_TRANSFORM_NEEDED;
                    goto out;
                }

                if (prefix_policy.binary_nul_found) {
                    struct bx_match bm;
                    bx_search_dev_counters_note_binary_policy_check();
                    /*
                     * Default plain-output rg treats the first NUL as a
                     * binary cut-off. Consult only bytes already read by the
                     * main scan; do not issue a separate prefix pread or a
                     * second policy scan over the same prefix bytes.
                     */
                    if (search_file_find_literal_precheck_match(absence_plan,
                                                                scanner->buf,
                                                                prefix_policy.binary_nul_offset,
                                                                0u,
                                                                &bm)) {
                        result = BX_SEARCH_DEFERRED_PRECHECK_POSSIBLE_MATCH;
                        goto out;
                    }
                    if (!opts->quiet) {
                        bx_search_dev_counters_note_file_cut_off_by_binary_prefix();
                        result = BX_SEARCH_DEFERRED_PRECHECK_NO_MATCH;
                        goto out;
                    }
                }
            }

            if (bx_search_chunk_overlap_has_fresh_bytes(scanner->len, carry)) {
                struct bx_match bm;
                size_t scan_start = bx_search_chunk_overlap_scan_start(carry, overlap);
                size_t overlap_bytes_scanned =
                    bx_search_chunk_overlap_rescanned_bytes(carry, overlap);

                bx_search_dev_counters_note_literal_overlap_bytes_scanned(overlap_bytes_scanned);
                if (search_file_find_literal_precheck_match(absence_plan,
                                                            scanner->buf,
                                                            scanner->len,
                                                            scan_start,
                                                            &bm)) {
                    bx_search_chunk_overlap_note_cross_chunk_match(bm.start, bm.end, carry);
                    result = BX_SEARCH_DEFERRED_PRECHECK_POSSIBLE_MATCH;
                    goto out;
                }
            }

            if (nread == 0) {
                result = BX_SEARCH_DEFERRED_PRECHECK_NO_MATCH;
                goto out;
            }
            carry = bx_search_chunk_overlap_carry_len(scanner->len, overlap);
        }
    }

out_error:
    bx_search_report_path_error(progname,
                                search_file_resolve_display_name(display_name_state, opts),
                                errno ? errno : EIO,
                                opts);
    result = BX_SEARCH_DEFERRED_PRECHECK_ERROR;

out:
    bx_search_dev_counters_note_content_close_call();
    close(fd);
    if (stats && (result == BX_SEARCH_DEFERRED_PRECHECK_NO_MATCH ||
                  (result == BX_SEARCH_DEFERRED_PRECHECK_POSSIBLE_MATCH &&
                   opts->quiet && exec_plan && exec_plan->raw_presence_supported))) {
        stats->files_searched++;
        stats->bytes_searched += searched_len;
        if (result == BX_SEARCH_DEFERRED_PRECHECK_POSSIBLE_MATCH) {
            stats->matches++;
            stats->matched_lines++;
            stats->files_with_matches++;
        }
    }
    return result;
}

static int search_file_opened_without_reopen(FILE *f,
                                             bool use_stdin,
                                             const char *display_name,
                                             const char *progname,
                                             struct bx_matcher *m,
                                             const struct bx_search_exec_plan *exec_plan,
                                             struct search_opts *opts,
                                             int *match_count,
                                             struct bx_search_scanner *scanner,
                                             struct bx_record_stream *record_stream,
                                             struct bx_search_stats *stats) {
    enum bx_search_file_kernel_kind kernel =
        exec_plan ? exec_plan->opened_special_kernel : BX_SEARCH_FILE_KERNEL_STREAMING;

    return search_file_run_opened_kernel(kernel, f, use_stdin, NULL, display_name, progname,
                                         m, opts, match_count, scanner,
                                         record_stream, stats);
}

static int search_file_handle_binary_prefix(FILE *f,
                                            bool use_stdin,
                                            const char *display_name,
                                            const char *progname,
                                            struct bx_matcher *m,
                                            struct search_opts *opts,
                                            int *match_count,
                                            struct bx_record_stream *record_stream,
                                            struct bx_search_stats *stats) {
    bool is_binary_file = false;

    if (!f || !opts || opts->null_data || opts->binary_as_text || !record_stream)
        return -1;

    if (!bx_record_stream_probe_binary_prefix(f, record_stream, &is_binary_file)) {
        if (bx_record_stream_had_error(record_stream)) {
            search_file_report_record_stream_error(progname,
                                                   display_name ? display_name
                                                                : "(standard input)",
                                                   record_stream,
                                                   opts);
            if (!use_stdin)
                fclose(f);
            return 2;
        }
        return -1;
    }
    if (!is_binary_file)
        return -1;

    bx_search_dev_counters_note_binary_policy_check();
    if (opts->binary_without_match) {
        bx_search_dev_counters_note_file_cut_off_by_binary_prefix();
        if (!use_stdin)
            fclose(f);
        return bx_search_binary_without_match(display_name, opts, match_count, stats);
    }

    if (opts->count_only)
        return -2;

    return binary_presence_opened(f, use_stdin, display_name, progname, m, opts,
                                  match_count, record_stream, stats);
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

static void search_file_report_record_stream_error(const char *progname,
                                                   const char *path,
                                                   struct bx_record_stream *record_stream,
                                                   const struct search_opts *opts) {
    int errnum;

    if (!record_stream)
        return;

    errnum = bx_record_stream_error(record_stream);
    if (errnum == EOVERFLOW) {
        bx_search_report_record_too_large(progname, path, opts);
        return;
    }

    bx_search_report_path_error(progname, path, errnum != 0 ? errnum : EIO, opts);
}

static bool binary_segment_matches(const unsigned char *buf,
                                   size_t len,
                                   struct bx_matcher *m,
                                   struct search_opts *opts) {
    struct bx_match bm;
    bool matched =
        bx_search_matcher_find_with_opts(m, buf, len, 0, opts, &bm) == 0;

    if (opts->invert_match)
        matched = !matched;
    return matched;
}

static int binary_presence_opened(FILE *f,
                                  bool use_stdin,
                                  const char *display_name,
                                  const char *progname,
                                  struct bx_matcher *m,
                                  struct search_opts *opts,
                                  int *match_count,
                                  struct bx_record_stream *record_stream,
                                  struct bx_search_stats *stats) {
    unsigned char chunk[8192];
    unsigned char *segment = NULL;
    size_t segment_len = 0u;
    size_t segment_cap = 0u;
    size_t segment_limit = bx_record_stream_record_limit(record_stream);
    bool matched = false;

    if (segment_limit == 0u)
        segment_limit = bx_record_stream_default_record_limit();

    if (stats)
        stats->files_searched++;

    for (;;) {
        size_t nread = bx_record_stream_read_chunk(f, record_stream, chunk, sizeof(chunk));
        size_t start = 0u;

        if (stats)
            stats->bytes_searched += nread;
        if (nread == 0u)
            break;

        while (start < nread) {
            size_t span = start;

            while (span < nread && chunk[span] != '\n' && chunk[span] != '\0')
                span++;

            if (span > start) {
                size_t piece = span - start;
                size_t needed = segment_len + piece;
                if (needed > segment_limit) {
                    if (record_stream)
                        record_stream->errnum = EOVERFLOW;
                    goto out_error;
                }
                if (needed > segment_cap) {
                    size_t new_cap = segment_cap == 0u ? 256u : segment_cap;
                    while (new_cap < needed) {
                        if (new_cap > (SIZE_MAX / 2u)) {
                            if (record_stream)
                                record_stream->errnum = ENOMEM;
                            goto out_error;
                        }
                        new_cap *= 2u;
                    }
                    unsigned char *tmp = realloc(segment, new_cap);
                    if (!tmp) {
                        if (record_stream)
                            record_stream->errnum = ENOMEM;
                        goto out_error;
                    }
                    segment = tmp;
                    segment_cap = new_cap;
                }
                memcpy(segment + segment_len, chunk + start, piece);
                segment_len += piece;
            }

            if (span < nread) {
                if (binary_segment_matches(segment, segment_len, m, opts)) {
                    matched = true;
                    goto out_done;
                }
                segment_len = 0u;
            }

            start = span + 1u;
        }
    }

    if (bx_record_stream_had_error(record_stream))
        goto out_error;

    if (segment_len > 0u && binary_segment_matches(segment, segment_len, m, opts))
        matched = true;

out_done:
    free(segment);
    if (!use_stdin)
        fclose(f);

    if (matched) {
        if (stats) {
            stats->matches++;
            stats->matched_lines++;
            stats->files_with_matches++;
        }
        if (match_count)
            (*match_count)++;
        if (opts->quiet)
            return 0;
        if (opts->files_with_matches && display_name) {
            if (opts->null_output)
                bx_search_printf_out("%s%c", display_name, '\0');
            else
                bx_search_printf_out("%s\n", display_name);
            bx_search_dev_counters_note_output_line_emitted();
            return 0;
        }
        if (!(opts->files_without_match || opts->count_only))
            bx_search_report_binary_match(progname, display_name);
        return 0;
    }

    if (opts->files_without_match && display_name) {
        if (opts->null_output)
            bx_search_printf_out("%s%c", display_name, '\0');
        else
            bx_search_printf_out("%s\n", display_name);
        bx_search_dev_counters_note_output_line_emitted();
    }
    return 1;

out_error:
    free(segment);
    search_file_report_record_stream_error(progname,
                                           display_name ? display_name : "(standard input)",
                                           record_stream,
                                           opts);
    if (!use_stdin)
        fclose(f);
    return 2;
}

static int binary_file_matches_opened(FILE *f,
                                      const char *display_name,
                                      const char *progname,
                                      struct bx_matcher *m,
                                      struct search_opts *opts,
                                      struct bx_record_stream *record_stream,
                                      bool *matched_out) {
    ssize_t len;
    bool matched = false;

    if (matched_out)
        *matched_out = false;

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

    if (bx_record_stream_had_error(record_stream)) {
        search_file_report_record_stream_error(progname, display_name, record_stream, opts);
        return 2;
    }

    if (matched_out)
        *matched_out = matched;
    return 0;
}

static int binary_file_matches(const char *filename,
                               const char *display_name,
                               const char *progname,
                               struct bx_matcher *m,
                               struct search_opts *opts,
                               struct bx_record_stream *record_stream,
                               bool *matched_out) {
    FILE *f = bx_search_input_fopen(filename, opts);

    if (!f)
        return 1;
    bx_search_dev_counters_note_file_opened();

    bx_record_stream_prepare_file(f, record_stream);
    int rc = binary_file_matches_opened(f, display_name, progname, m, opts, record_stream,
                                        matched_out);
    fclose(f);
    return rc;
}

int bx_search_search_transformed_buffer(unsigned char *buf,
                                        size_t len,
                                        const char *display_name,
                                        const char *progname,
                                        struct bx_matcher *m,
                                        const struct bx_search_exec_plan *exec_plan,
                                        struct search_opts *opts,
                                        int *match_count,
                                        struct bx_record_stream *record_stream,
                                        struct bx_search_stats *stats) {
    enum bx_search_file_kernel_kind kernel =
        exec_plan ? exec_plan->transformed_buffer_kernel : BX_SEARCH_FILE_KERNEL_STREAMING;

    if (kernel == BX_SEARCH_FILE_KERNEL_MULTILINE) {
        if (stats)
            stats->files_searched++;
        return bx_search_multiline_buffer(buf, len, display_name, m, opts, match_count, stats);
    }

    FILE *mem = fmemopen(buf, len, "r");
    if (!mem) {
        free(buf);
        bx_search_report_path_error(progname, display_name ? display_name : "(memory)",
                                    errno, opts);
        return 2;
    }
    if (record_stream)
        bx_record_stream_prepare_file(mem, record_stream);

    int rc = search_file_run_opened_kernel(kernel, mem, false, NULL, display_name, progname,
                                           m, opts, match_count, NULL,
                                           record_stream, stats);
    free(buf);
    return rc;
}

static int search_file_run_transformed_input(const char *filename,
                                             struct bx_search_display_name_state *display_name_state,
                                             const char *progname,
                                             struct bx_matcher *m,
                                             const struct bx_search_exec_plan *exec_plan,
                                             struct search_opts *opts,
                                             int *match_count,
                                             struct bx_record_stream *record_stream,
                                             struct bx_search_stats *stats) {
    unsigned char *transformed = NULL;
    size_t transformed_len = 0u;
    const char *display_name;
    enum bx_rg_transform_result transform_rc =
        bx_rg_load_transformed_input(filename, progname, opts,
                                     bx_search_error_output_stream(),
                                     &transformed, &transformed_len);

    if (transform_rc == BX_RG_TRANSFORM_NO_MATCH)
        return 1;
    if (transform_rc == BX_RG_TRANSFORM_ERROR)
        return 2;

    display_name = search_file_resolve_display_name(display_name_state, opts);
    return bx_search_search_transformed_buffer(transformed, transformed_len, display_name,
                                               progname, m, exec_plan, opts, match_count,
                                               record_stream, stats);
}

static int search_file_run_nonstdin_regular_path(const char *filename,
                                                 struct bx_search_display_name_state *display_name_state,
                                                 const char *progname,
                                                 struct bx_matcher *m,
                                                 const struct bx_search_exec_plan *exec_plan,
                                                 struct search_opts *opts,
                                                 int *match_count,
                                                 struct bx_search_scanner *scanner,
                                                 struct bx_record_stream *record_stream,
                                                 struct bx_search_stats *stats,
                                                 bool candidate_triggered) {
    FILE *f = bx_search_input_open_stream(filename, progname, opts, record_stream, NULL);
    const char *display_name;

    if (!f)
        return 2;

    if (bx_search_input_opened_needs_auto_transform(f, opts)) {
        fclose(f);
        return search_file_run_transformed_input(filename, display_name_state, progname,
                                                 m, exec_plan, opts, match_count,
                                                 record_stream, stats);
    }

    display_name = search_file_resolve_display_name(display_name_state, opts);
    if (exec_plan && exec_plan->raw_presence_supported) {
        return search_file_run_opened_kernel(BX_SEARCH_FILE_KERNEL_RAW_PRESENCE,
                                             f, false, filename, display_name, progname,
                                             m, opts, match_count, scanner, record_stream,
                                             stats);
    }

    {
        int binary_result = search_file_handle_binary_prefix(
            f, false, display_name, progname, m, opts, match_count, record_stream, stats
        );
        if (binary_result >= 0)
            return binary_result;
        if (binary_result == -2) {
            return search_file_run_opened_kernel(
                exec_plan ? exec_plan->binary_search_kernel : BX_SEARCH_FILE_KERNEL_STREAMING,
                f, false, filename, display_name, progname, m, opts,
                match_count, scanner, record_stream, stats);
        }

        enum bx_search_file_kernel_kind nonbinary_kernel =
            exec_plan ? exec_plan->opened_nonbinary_kernel
                      : BX_SEARCH_FILE_KERNEL_STREAMING;
        enum bx_search_file_kernel_kind resolved_kernel =
            search_file_resolve_opened_kernel(f, nonbinary_kernel);

        if (candidate_triggered && resolved_kernel == BX_SEARCH_FILE_KERNEL_SCANNER)
            bx_search_dev_counters_note_candidate_triggered_scanner_entry();
        return search_file_run_opened_kernel(
            resolved_kernel, f, false, filename, display_name, progname, m, opts,
            match_count, scanner, record_stream, stats);
    }
}

static int search_file_default_literal_raw_path(const char *filename,
                                                struct bx_search_display_name_state *display_name_state,
                                                const char *progname,
                                                struct bx_matcher *m,
                                                const struct bx_search_exec_plan *exec_plan,
                                                struct search_opts *opts,
                                                int *match_count,
                                                struct bx_search_scanner *scanner,
                                                struct bx_record_stream *record_stream,
                                                struct bx_search_stats *stats) {
    int precheck = search_file_deferred_literal_precheck_path(filename, display_name_state,
                                                              progname, m, exec_plan, opts,
                                                              scanner, stats);

    if (precheck == BX_SEARCH_DEFERRED_PRECHECK_NO_MATCH ||
        precheck == BX_SEARCH_DEFERRED_PRECHECK_ERROR) {
        return precheck;
    }
    if (precheck == BX_SEARCH_DEFERRED_PRECHECK_TRANSFORM_NEEDED) {
        return search_file_run_transformed_input(filename, display_name_state, progname,
                                                 m, exec_plan, opts, match_count,
                                                 record_stream, stats);
    }
    if (precheck == BX_SEARCH_DEFERRED_PRECHECK_POSSIBLE_MATCH &&
        opts->quiet && exec_plan && exec_plan->raw_presence_supported) {
        bx_search_dev_counters_note_literal_candidate_hit();
        if (match_count)
            (*match_count)++;
        return 0;
    }
    if (precheck == BX_SEARCH_DEFERRED_PRECHECK_POSSIBLE_MATCH) {
        bx_search_dev_counters_note_literal_candidate_hit();
        bx_search_dev_counters_note_candidate_triggered_reopen_call();
    }

    return search_file_run_nonstdin_regular_path(filename, display_name_state, progname, m,
                                                 exec_plan, opts, match_count, scanner,
                                                 record_stream, stats,
                                                 precheck == BX_SEARCH_DEFERRED_PRECHECK_POSSIBLE_MATCH);
}

static int search_file(const char *filename,
                       const char *display_name_override,
                       bool strip_dot_prefix,
                       const struct bx_walk_entry *walk_entry,
                       const char *progname,
                       struct bx_matcher *m,
                       const struct bx_search_exec_plan *exec_plan,
                       struct search_opts *opts,
                       int *match_count,
                       struct bx_search_scanner *scanner,
                       struct bx_record_stream *record_stream,
                       struct bx_search_stats *stats) {
    bool use_stdin = (!filename || strcmp(filename, "-") == 0);
    struct stat operand_st;
    bool operand_st_loaded = false;
    struct bx_search_display_name_state display_name_state = {
        .filename = filename,
        .display_name_override = display_name_override,
        .strip_dot_prefix = strip_dot_prefix,
    };
    int result = 1;
    int previous_offset_width = bx_search_output_get_offset_width();
    if (search_file_stat_from_walk_entry(walk_entry, filename, &operand_st)) {
        operand_st_loaded = true;
    } else if (search_file_should_stat_path_for_slow_policy(filename, use_stdin, opts)) {
        operand_st_loaded = stat(filename, &operand_st) == 0;
    }

    bx_search_output_set_offset_width(0);
    if (opts->initial_tab) {
        struct stat offset_width_st;

        if (use_stdin) {
            bx_search_dev_counters_note_content_fstat_call();
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

    if (bx_rg_trace_enabled(opts)) {
        bx_rg_tracef(opts, "search: %s",
                     search_file_resolve_display_name(&display_name_state, opts));
    }

    if (operand_st_loaded && (S_ISCHR(operand_st.st_mode) || S_ISBLK(operand_st.st_mode)
                              || S_ISFIFO(operand_st.st_mode) || S_ISSOCK(operand_st.st_mode))) {
        FILE *f;
        int binary_result;
        const char *display_name;

        if (bx_search_should_skip_special_input_mode(operand_st.st_mode, opts)) {
            result = 1;
            goto out;
        }

        f = bx_search_input_open_stream(filename, progname, opts, record_stream, NULL);
        if (!f)
            goto out_error;

        display_name = search_file_resolve_display_name(&display_name_state, opts);
        binary_result = search_file_handle_binary_prefix(f, false, display_name, progname, m,
                                                         opts, match_count, record_stream, stats);
        if (binary_result >= 0) {
            result = binary_result;
            goto out;
        }
        if (binary_result == -2) {
            result = search_file_run_opened_kernel(
                exec_plan ? exec_plan->binary_search_kernel : BX_SEARCH_FILE_KERNEL_STREAMING,
                f, false, filename, display_name, progname, m, opts,
                match_count, scanner, record_stream, stats);
            goto out;
        }

        result = search_file_opened_without_reopen(f, false, display_name, progname, m,
                                                   exec_plan, opts, match_count, scanner,
                                                   record_stream, stats);
        goto out;
    }

    if (bx_search_input_needs_early_transform_load(filename, use_stdin, opts)) {
        result = search_file_run_transformed_input(filename, &display_name_state,
                                                   progname, m, exec_plan, opts,
                                                   match_count, record_stream, stats);
        goto out;
    }

    if (opts->multiline) {
        const char *display_name = search_file_resolve_display_name(&display_name_state, opts);
        result = search_file_run_path_kernel(BX_SEARCH_FILE_KERNEL_MULTILINE,
                                             filename, display_name, progname, m, opts,
                                             match_count, scanner, record_stream, stats);
        goto out;
    }
    if (!use_stdin && !opts->recursive) {
        struct stat st;
        if (filename && strcmp(filename, "-") != 0 &&
            lstat(filename, &st) == 0 && S_ISDIR(st.st_mode)) {
            bx_search_report_path_error(progname, filename, EISDIR, opts);
            result = 2;
            goto out;
        }
    }

    if (use_stdin) {
        enum bx_search_file_kernel_kind stdin_kernel =
            exec_plan ? exec_plan->stdin_path_kernel : BX_SEARCH_FILE_KERNEL_STREAMING;
        FILE *f;
        int binary_result;
        const char *display_name;

        f = bx_search_input_open_stream(filename, progname, opts, record_stream, NULL);
        if (!f)
            goto out_error;

        display_name = search_file_resolve_display_name(&display_name_state, opts);
        binary_result = search_file_handle_binary_prefix(f, true, display_name, progname, m,
                                                         opts, match_count, record_stream, stats);
        if (binary_result >= 0) {
            result = binary_result;
            goto out;
        }
        if (binary_result == -2) {
            result = search_file_run_opened_kernel(
                exec_plan ? exec_plan->binary_search_kernel : BX_SEARCH_FILE_KERNEL_STREAMING,
                f, true, filename, display_name, progname, m, opts,
                match_count, scanner, record_stream, stats);
            goto out;
        }

        result = search_file_run_opened_kernel(stdin_kernel,
                                               f, true, filename, display_name, progname, m, opts,
                                               match_count, scanner, record_stream, stats);
        goto out;
    }

    if (!use_stdin && exec_plan && exec_plan->deferred_literal_precheck) {
        result = search_file_default_literal_raw_path(filename, &display_name_state,
                                                      progname, m, exec_plan, opts,
                                                      match_count, scanner, record_stream, stats);
        goto out;
    }

    if (!use_stdin && !opts->null_data && !opts->binary_as_text) {
        result = search_file_run_nonstdin_regular_path(filename, &display_name_state,
                                                       progname, m, exec_plan, opts,
                                                       match_count, scanner, record_stream,
                                                       stats, false);
        goto out;
    }

    if (!use_stdin && !opts->null_data && !opts->binary_as_text &&
        bx_search_input_is_binary_path(filename, opts)) {
        const char *display_name = search_file_resolve_display_name(&display_name_state, opts);
        bx_search_dev_counters_note_binary_policy_check();
        if (opts->binary_without_match) {
            bx_search_dev_counters_note_file_cut_off_by_binary_prefix();
            result = bx_search_binary_without_match(display_name, opts, match_count, stats);
            goto out;
        }

        if (opts->quiet || opts->files_with_matches ||
            opts->files_without_match || opts->count_only) {
            result = search_file_run_path_kernel(exec_plan
                                                     ? exec_plan->binary_search_kernel
                                                     : BX_SEARCH_FILE_KERNEL_STREAMING,
                                                 filename, display_name, progname, m, opts,
                                                 match_count, scanner, record_stream, stats);
            goto out;
        }

        bool matched = false;
        int binary_rc = binary_file_matches(filename, display_name, progname, m, opts,
                                            record_stream, &matched);
        if (binary_rc == 2) {
            result = 2;
            goto out;
        }
        if (matched) {
            bx_search_report_binary_match(progname, display_name);
            if (match_count)
                (*match_count)++;
            result = 0;
            goto out;
        }
        result = 1;
        goto out;
    }

    {
        const char *display_name = search_file_resolve_display_name(&display_name_state, opts);
        result = search_file_run_path_kernel(exec_plan
                                                 ? exec_plan->regular_path_kernel
                                                 : BX_SEARCH_FILE_KERNEL_STREAMING,
                                             filename, display_name, progname, m, opts,
                                             match_count, scanner, record_stream, stats);
    }
    goto out;

out_error:
    result = 2;
out:
    bx_search_output_set_offset_width(previous_offset_width);
    free(display_name_state.owned_display_name);
    return result;
}

int bx_search_search_file(const char *filename,
                          const char *display_name_override,
                          bool strip_dot_prefix,
                          const char *progname,
                          struct bx_matcher *m,
                          const struct bx_search_exec_plan *exec_plan,
                          struct search_opts *opts,
                          int *match_count,
                          struct bx_search_scanner *scanner,
                          struct bx_record_stream *record_stream,
                          struct bx_search_stats *stats) {
    return search_file(filename, display_name_override, strip_dot_prefix, NULL, progname, m,
                       exec_plan, opts,
                       match_count, scanner, record_stream, stats);
}

int bx_search_search_walk_entry(const struct bx_walk_entry *entry,
                                const char *display_name_override,
                                bool strip_dot_prefix,
                                const char *progname,
                                struct bx_matcher *m,
                                const struct bx_search_exec_plan *exec_plan,
                                struct search_opts *opts,
                                int *match_count,
                                struct bx_search_scanner *scanner,
                                struct bx_record_stream *record_stream,
                                struct bx_search_stats *stats) {
    if (!entry || !entry->path)
        return bx_search_search_file(NULL, display_name_override, strip_dot_prefix,
                                     progname, m, exec_plan, opts, match_count,
                                     scanner, record_stream, stats);
    return search_file(entry->path, display_name_override, strip_dot_prefix, entry,
                       progname, m, exec_plan, opts, match_count, scanner,
                       record_stream, stats);
}
