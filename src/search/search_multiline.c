#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dev_counters.h"
#include "pcre2_matcher.h"
#include "search_input.h"
#include "search_internal.h"
#include "search_multiline.h"

static size_t bx_search_multiline_line_number_for_offset(const unsigned char *buf,
                                                         size_t offset) {
    size_t line = 1u;

    for (size_t i = 0; i < offset; i++) {
        if (buf[i] == '\n')
            line++;
    }
    return line;
}

static size_t bx_search_multiline_line_start_offset(const unsigned char *buf,
                                                    size_t offset) {
    while (offset > 0u && buf[offset - 1u] != '\n')
        offset--;
    return offset;
}

static size_t bx_search_multiline_column_number_for_offset(const unsigned char *buf,
                                                           size_t offset) {
    return offset - bx_search_multiline_line_start_offset(buf, offset) + 1u;
}

static size_t bx_search_multiline_line_end_offset(const unsigned char *buf,
                                                  size_t len,
                                                  size_t offset) {
    while (offset < len && buf[offset] != '\n')
        offset++;
    if (offset < len)
        offset++;
    return offset;
}

static void bx_search_multiline_write_output(const void *buf, size_t len) {
    size_t written;

    if (len == 0u)
        return;
    written = fwrite(buf, 1u, len, bx_search_output_stream());
    if (written > 0u)
        bx_search_note_stdout_output();
    bx_search_stats_count_bytes(written);
}

static int bx_search_multiline_emit_summary(const char *display_name,
                                            struct search_opts *opts,
                                            int *match_count,
                                            struct bx_search_stats *stats,
                                            int file_matches) {
    if (opts->count_only)
        bx_search_print_count_result(display_name, opts, file_matches);
    if (opts->files_with_matches && file_matches > 0 && display_name) {
        bx_search_print_path_record(display_name, opts);
        bx_search_dev_counters_note_output_line_emitted();
    }
    if (opts->files_without_match && file_matches == 0 && display_name) {
        bx_search_print_path_record(display_name, opts);
        bx_search_dev_counters_note_output_line_emitted();
    }

    if (stats && file_matches > 0)
        stats->files_with_matches++;
    if (match_count)
        *match_count += file_matches;
    return file_matches > 0 ? 0 : 1;
}

int bx_search_multiline_buffer(unsigned char *buf,
                               size_t len,
                               const char *display_name,
                               struct bx_matcher *m,
                               struct search_opts *opts,
                               int *match_count,
                               struct bx_search_stats *stats) {
    size_t start = 0u;
    int file_matches = 0;
    int status = 1;
    bool heading_printed_for_file = false;

    if (stats)
        stats->bytes_searched += len;

    while (start <= len) {
        struct bx_match bm;

        if (bx_search_matcher_find_with_opts(m, buf, len, start, opts, &bm) != 0)
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
            start = bm.end > bm.start ? bm.end : bm.start + 1u;
            if (opts->max_count > 0 && file_matches >= opts->max_count)
                break;
            continue;
        }
        if (opts->files_with_matches || opts->files_without_match) {
            if (!opts->stats)
                break;
            start = bm.end > bm.start ? bm.end : bm.start + 1u;
            if (opts->max_count > 0 && file_matches >= opts->max_count)
                break;
            continue;
        }

        size_t line_num = bx_search_multiline_line_number_for_offset(buf, bm.start);
        if (opts->only_matching && !opts->invert_match) {
            if (bm.end == bm.start) {
                if (opts->max_count > 0 && file_matches >= opts->max_count)
                    break;
                start = bm.start + 1u;
                continue;
            }
            bx_search_maybe_print_heading(display_name, opts, &heading_printed_for_file);
            bool prefix_printed = bx_search_print_result_prefix(
                heading_printed_for_file ? NULL : display_name,
                opts,
                (int)line_num,
                bx_search_multiline_column_number_for_offset(buf, bm.start),
                true,
                bm.start,
                bx_search_match_field_separator(opts));

            bx_search_maybe_emit_initial_tab(opts, prefix_printed);
            bx_search_multiline_write_output(buf + bm.start, bm.end - bm.start);
            bx_search_write_record_terminator(opts);
            bx_search_dev_counters_note_output_line_emitted();
        } else {
            size_t out_start = bx_search_multiline_line_start_offset(buf, bm.start);
            size_t out_end = bx_search_multiline_line_end_offset(buf, len, bm.end);

            bx_search_maybe_print_heading(display_name, opts, &heading_printed_for_file);
            bool prefix_printed = bx_search_print_result_prefix(
                heading_printed_for_file ? NULL : display_name,
                opts,
                (int)bx_search_multiline_line_number_for_offset(buf, out_start),
                bx_search_multiline_column_number_for_offset(buf, bm.start),
                true,
                out_start,
                bx_search_match_field_separator(opts));

            bx_search_maybe_emit_initial_tab(opts, prefix_printed);
            if (bx_search_should_omit_long_match_line(opts, out_end - out_start)) {
                bx_search_print_omitted_long_line(opts);
            } else if (opts->replace) {
                bx_search_print_replaced_record(buf + out_start, out_end - out_start, m, opts);
            } else {
                bx_search_print_plain_record_contents(buf + out_start, out_end - out_start,
                                                      opts);
                if (out_end == out_start
                    || buf[out_end - 1u] != (unsigned char)bx_search_record_delimiter(opts)) {
                    bx_search_write_record_terminator(opts);
                }
                bx_search_dev_counters_note_output_line_emitted();
            }
        }

        if (opts->max_count > 0 && file_matches >= opts->max_count)
            break;
        start = bm.end > bm.start ? bm.end : bm.start + 1u;
    }

    if (status != 2)
        status = bx_search_multiline_emit_summary(display_name, opts, match_count, stats,
                                                  file_matches);
    free(buf);
    return status;
}

int bx_search_multiline_path(const char *filename,
                             const char *display_name,
                             const char *progname,
                             struct bx_matcher *m,
                             struct search_opts *opts,
                             int *match_count,
                             struct bx_search_stats *stats) {
    FILE *f = stdin;
    bool use_stdin = (!filename || strcmp(filename, "-") == 0);
    size_t len = 0u;
    unsigned char *buf;

    if (!use_stdin) {
        f = bx_search_input_fopen(filename, opts);
        if (!f) {
            bx_search_report_path_error(progname, filename, errno, opts);
            return 2;
        }
        bx_search_dev_counters_note_file_opened();
    }

    buf = bx_search_input_read_stream_all(f, &len);
    if (!use_stdin)
        fclose(f);
    if (!buf)
        return 2;
    if (stats)
        stats->files_searched++;
    return bx_search_multiline_buffer(buf, len, display_name, m, opts, match_count, stats);
}

int bx_search_multiline_opened(FILE *f,
                               bool use_stdin,
                               const char *display_name,
                               struct bx_matcher *m,
                               struct search_opts *opts,
                               int *match_count,
                               struct bx_search_stats *stats) {
    size_t len = 0u;
    unsigned char *buf = bx_search_input_read_stream_all(f, &len);

    if (!use_stdin)
        fclose(f);
    if (!buf)
        return 2;
    if (stats)
        stats->files_searched++;
    return bx_search_multiline_buffer(buf, len, display_name, m, opts, match_count, stats);
}
