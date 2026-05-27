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
#include "pcre2_matcher.h"
#include "rg_output.h"
#include "rg_transform.h"
#include "scanner.h"
#include "search_buffered.h"
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

#define BX_SEARCH_DEFERRED_CHUNK_CAP (256u * 1024u)

enum bx_search_deferred_precheck_result {
    BX_SEARCH_DEFERRED_PRECHECK_UNSUPPORTED = -1,
    BX_SEARCH_DEFERRED_PRECHECK_POSSIBLE_MATCH = 0,
    BX_SEARCH_DEFERRED_PRECHECK_NO_MATCH = 1,
    BX_SEARCH_DEFERRED_PRECHECK_ERROR = 2,
};

static const char *display_name_for_stream(const char *filename,
                                           const char *display_name_override,
                                           struct search_opts *opts) {
    if (display_name_override)
        return display_name_override;
    if (!filename || strcmp(filename, "-") == 0)
        return opts->label ? opts->label : "(standard input)";
    return filename;
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
                                             m, opts, match_count, scanner, stats);
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

static int search_file_deferred_literal_precheck_path(const char *filename,
                                                      const char *display_name,
                                                      const char *progname,
                                                      struct bx_matcher *m,
                                                      const struct bx_search_exec_plan *exec_plan,
                                                      struct search_opts *opts,
                                                      struct bx_search_scanner *scanner,
                                                      struct bx_search_stats *stats) {
    struct bx_literal_matcher *literal;
    struct stat st;
    int fd;
    int result = BX_SEARCH_DEFERRED_PRECHECK_UNSUPPORTED;
    size_t searched_len = 0u;
    unsigned char prefix[1024];
    ssize_t prefix_len = 0;

    if (!filename || !m || !exec_plan || !exec_plan->deferred_literal_precheck ||
        !opts || !scanner) {
        return BX_SEARCH_DEFERRED_PRECHECK_UNSUPPORTED;
    }
    literal = bx_search_matcher_literal(m);
    if (!literal)
        return BX_SEARCH_DEFERRED_PRECHECK_UNSUPPORTED;

    fd = bx_search_input_open_fd(filename, opts);
    if (fd < 0) {
        bx_search_report_path_error(progname, filename, errno, opts);
        return BX_SEARCH_DEFERRED_PRECHECK_ERROR;
    }
    bx_search_dev_counters_note_file_opened();

    if (fstat(fd, &st) != 0)
        goto out_error;
    if (!S_ISREG(st.st_mode) || st.st_size < 0)
        goto out;

    do {
        prefix_len = pread(fd, prefix, sizeof(prefix), 0);
    } while (prefix_len < 0 && errno == EINTR);
    if (prefix_len < 0)
        goto out_error;
    bx_search_dev_counters_note_bytes_read((size_t)prefix_len);
    if (bx_rg_transform_auto_encoding_needs_prefix(opts, prefix, (size_t)prefix_len))
        goto out;

    if (!opts->null_data && !opts->binary_as_text) {
        unsigned char *nul = memchr(prefix, '\0', (size_t)prefix_len);
        if (nul) {
            struct bx_match bm;
            /*
             * Default plain-output rg currently treats the first NUL as a
             * definitive binary cut-off, but quiet mode may still consult the
             * later binary presence path. Keep the cheap early-positive here,
             * and only let quiet searches fall through to a full-file
             * conservative precheck before rejecting the file.
             */
            if (bx_literal_find(literal, prefix, (size_t)(nul - prefix), 0u, &bm) == 0) {
                searched_len = (size_t)prefix_len;
                result = BX_SEARCH_DEFERRED_PRECHECK_POSSIBLE_MATCH;
                goto out;
            }
            if (!opts->quiet) {
                searched_len = (size_t)prefix_len;
                result = BX_SEARCH_DEFERRED_PRECHECK_NO_MATCH;
                goto out;
            }
        }
    }

    if (st.st_size == 0) {
        result = BX_SEARCH_DEFERRED_PRECHECK_NO_MATCH;
        goto out;
    }

    {
        size_t plen = bx_literal_len(literal);
        size_t overlap = plen > 0u ? plen - 1u : 0u;
        size_t carry = 0u;

        if (!bx_search_scanner_reserve(scanner, BX_SEARCH_DEFERRED_CHUNK_CAP + overlap))
            goto out;
        for (;;) {
            if (carry > 0u)
                memmove(scanner->buf, scanner->buf + scanner->len - carry, carry);
            scanner->len = carry;

            ssize_t nread;
            do {
                nread = read(fd, scanner->buf + carry, scanner->cap - carry);
            } while (nread < 0 && errno == EINTR);
            if (nread < 0)
                goto out_error;
            scanner->len += (size_t)nread;
            searched_len += (size_t)nread;
            bx_search_dev_counters_note_bytes_read((size_t)nread);

            if (scanner->len > 0u) {
                struct bx_match bm;
                if (bx_literal_find(literal, scanner->buf, scanner->len, 0u, &bm) == 0) {
                    result = BX_SEARCH_DEFERRED_PRECHECK_POSSIBLE_MATCH;
                    goto out;
                }
            }

            if (nread == 0) {
                result = BX_SEARCH_DEFERRED_PRECHECK_NO_MATCH;
                goto out;
            }
            carry = overlap < scanner->len ? overlap : scanner->len;
        }
    }

out_error:
    bx_search_report_path_error(progname,
                                display_name ? display_name : filename,
                                errno ? errno : EIO,
                                opts);
    result = BX_SEARCH_DEFERRED_PRECHECK_ERROR;

out:
    close(fd);
    if (result == BX_SEARCH_DEFERRED_PRECHECK_NO_MATCH && stats) {
        stats->files_searched++;
        stats->bytes_searched += searched_len;
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

    if (opts->binary_without_match) {
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

static int search_file(const char *filename,
                       const char *display_name_override,
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
    char *owned_display_name = NULL;
    const char *display_name = display_name_for_stream(filename, display_name_override, opts);
    int result = 1;
    int previous_offset_width = bx_search_output_get_offset_width();

    if (!display_name_override && filename && strcmp(filename, "-") != 0 &&
        opts->path_separator != '\0' && opts->path_separator != '/') {
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

    if (operand_st_loaded && (S_ISCHR(operand_st.st_mode) || S_ISBLK(operand_st.st_mode)
                              || S_ISFIFO(operand_st.st_mode) || S_ISSOCK(operand_st.st_mode))) {
        FILE *f;
        int binary_result;

        if (bx_search_should_skip_special_input_mode(operand_st.st_mode, opts)) {
            result = 1;
            goto out;
        }

        f = bx_search_input_open_stream(filename, progname, opts, record_stream, NULL);
        if (!f)
            goto out_error;

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
        result = bx_search_search_transformed_buffer(transformed, transformed_len, display_name,
                                                     progname, m, exec_plan, opts, match_count,
                                                     record_stream, stats);
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

        f = bx_search_input_open_stream(filename, progname, opts, record_stream, NULL);
        if (!f)
            goto out_error;

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

    if (!use_stdin) {
        int precheck = search_file_deferred_literal_precheck_path(filename, display_name,
                                                                  progname, m, exec_plan, opts,
                                                                  scanner, stats);
        if (precheck == BX_SEARCH_DEFERRED_PRECHECK_NO_MATCH ||
            precheck == BX_SEARCH_DEFERRED_PRECHECK_ERROR) {
            result = precheck;
            goto out;
        }
    }

    if (!use_stdin && !opts->null_data && !opts->binary_as_text) {
        FILE *f = bx_search_input_open_stream(filename, progname, opts, record_stream, NULL);
        if (!f)
            goto out_error;

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
            result = bx_search_search_transformed_buffer(transformed, transformed_len,
                                                         display_name, progname, m, exec_plan, opts,
                                                         match_count, record_stream, stats);
            goto out;
        }

        if (exec_plan && exec_plan->raw_presence_supported) {
            result = search_file_run_opened_kernel(BX_SEARCH_FILE_KERNEL_RAW_PRESENCE,
                                                   f, false, filename, display_name, progname,
                                                   m, opts, match_count, scanner, record_stream,
                                                   stats);
            goto out;
        }

        {
            int binary_result = search_file_handle_binary_prefix(
                f, false, display_name, progname, m, opts, match_count, record_stream, stats
            );
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

            enum bx_search_file_kernel_kind nonbinary_kernel =
                exec_plan ? exec_plan->opened_nonbinary_kernel
                          : BX_SEARCH_FILE_KERNEL_STREAMING;

            result = search_file_run_opened_kernel(
                search_file_resolve_opened_kernel(f, nonbinary_kernel),
                f, false, filename, display_name, progname, m, opts,
                match_count, scanner, record_stream, stats);
            goto out;
        }
    }

    if (!use_stdin && !opts->null_data && !opts->binary_as_text &&
        bx_search_input_is_binary_path(filename, opts)) {
        if (opts->binary_without_match) {
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

    result = search_file_run_path_kernel(exec_plan
                                             ? exec_plan->regular_path_kernel
                                             : BX_SEARCH_FILE_KERNEL_STREAMING,
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
                          const struct bx_search_exec_plan *exec_plan,
                          struct search_opts *opts,
                          int *match_count,
                          struct bx_search_scanner *scanner,
                          struct bx_record_stream *record_stream,
                          struct bx_search_stats *stats) {
    return search_file(filename, display_name_override, progname, m, exec_plan, opts,
                       match_count, scanner, record_stream, stats);
}
