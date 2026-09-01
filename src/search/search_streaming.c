#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "dev_counters.h"
#include "lib/color.h"
#include "pcre2_matcher.h"
#include "record_stream.h"
#include "search_input.h"
#include "search_internal.h"
#include "search_streaming.h"

bool bx_search_streaming_uses_line_buffered_stdin(const struct search_opts *opts,
                                                  bool use_stdin) {
    return use_stdin && opts->line_buffered && !opts->null_data && !opts->binary_as_text &&
           !opts->quiet && !opts->count_only &&
           !opts->files_with_matches && !opts->files_without_match;
}

static void bx_search_streaming_maybe_flush_line_buffered(const struct search_opts *opts) {
    FILE *out;

    if (!opts || !opts->line_buffered)
        return;
    out = bx_search_output_stream();
    if (out)
        fflush(out);
}

int bx_search_streaming_opened(FILE *f,
                               bool use_stdin,
                               const char *display_name,
                               const char *progname,
                               struct bx_matcher *m,
                               struct search_opts *opts,
                               int *match_count,
                               struct bx_record_stream *record_stream,
                               struct bx_search_stats *stats) {
    ssize_t len;
    int line_num = 0;
    int file_matches = 0;
    int status = 1;
    size_t file_offset = 0u;
    bool saw_match_record = false;
    bool heading_printed_for_file = false;
    bool stdout_emitted = false;
    bool binary_seen_before_output = false;
    const bool line_buffered_stdin_binary_watch =
        bx_search_streaming_uses_line_buffered_stdin(opts, use_stdin);

    if (stats)
        stats->files_searched++;

    while ((len = line_buffered_stdin_binary_watch
                      ? bx_record_stream_read_live(f, record_stream,
                                                   bx_search_record_delimiter(opts))
                      : bx_search_input_read_record(f, record_stream, opts)) != -1) {
        char *line = record_stream->record;
        size_t line_offset = file_offset;
        size_t record_len = (size_t)len;

        file_offset += record_len;
        if (stats)
            stats->bytes_searched += record_len;
        line_num++;
        if (line_buffered_stdin_binary_watch
            && !stdout_emitted
            && memchr(line, '\0', record_len) != NULL) {
            binary_seen_before_output = true;
        }

        struct bx_match bm;
        size_t match_len = bx_search_record_match_len((unsigned char *)line, record_len, opts);
        int match_rc =
            bx_search_matcher_find_with_opts(m, (unsigned char *)line, match_len, 0, opts, &bm);
        if (match_rc < 0)
            break;
        bool matched = match_rc == 0;
        if (opts->invert_match)
            matched = !matched;

        if (matched) {
            int record_match_count = (!opts->invert_match && opts->count_matches)
                ? bx_search_count_record_matches(m, (unsigned char *)line, match_len, opts)
                : 1;
            if (bx_search_matcher_had_error(m))
                break;

            file_matches += record_match_count;
            if (stats) {
                stats->matches += record_match_count;
                stats->matched_lines++;
            }
            status = 0;
            saw_match_record = true;

            if (opts->quiet)
                break;
            if (opts->count_only) {
                if (opts->max_count > 0 && file_matches >= opts->max_count)
                    break;
                continue;
            }
            if (opts->files_with_matches || opts->files_without_match) {
                if (!opts->stats)
                    break;
                if (opts->max_count > 0 && file_matches >= opts->max_count)
                    break;
                continue;
            }
            if (binary_seen_before_output) {
                bx_search_dev_counters_note_binary_policy_check();
                bx_search_report_binary_match(progname, display_name);
                break;
            }
            if (opts->vimgrep && !opts->invert_match) {
                bx_search_maybe_print_heading(display_name, opts, &heading_printed_for_file);
                bx_search_print_vimgrep_matches((unsigned char *)line, record_len,
                                                heading_printed_for_file ? NULL : display_name,
                                                line_num, line_offset, m, opts);
                stdout_emitted = true;
            } else if (opts->only_matching && !opts->invert_match) {
                bx_search_maybe_print_heading(display_name, opts, &heading_printed_for_file);
                bx_search_print_only_matches_from_first(
                    (unsigned char *)line, record_len,
                    heading_printed_for_file ? NULL : display_name,
                    line_num, line_offset, m, &bm, opts
                );
                stdout_emitted = true;
            } else if (!(opts->only_matching && opts->invert_match)) {
                bx_search_maybe_print_heading(display_name, opts, &heading_printed_for_file);
                bool prefix_printed = bx_search_print_result_prefix(
                    heading_printed_for_file ? NULL : display_name,
                    opts,
                    line_num,
                    bm.start + 1u,
                    true,
                    line_offset,
                    bx_search_match_field_separator(opts));

                bx_search_maybe_emit_initial_tab(opts, prefix_printed);
                if (opts->invert_match) {
                    bx_search_print_plain_record_contents((unsigned char *)line, record_len, opts);
                    if (len == 0 || line[len - 1] != bx_search_record_delimiter(opts))
                        bx_search_write_record_terminator(opts);
                    bx_search_dev_counters_note_output_line_emitted();
                    bx_search_streaming_maybe_flush_line_buffered(opts);
                    stdout_emitted = true;
                } else if (opts->replace) {
                    bx_search_print_replaced_record((unsigned char *)line, record_len, m, opts);
                    bx_search_streaming_maybe_flush_line_buffered(opts);
                    stdout_emitted = true;
                } else {
                    if (bx_search_should_omit_long_match_line(opts, record_len)) {
                        bx_search_print_omitted_long_line(opts);
                    } else {
                        FILE *out = bx_search_output_stream();
                        bool color = bx_color_enabled();
                        unsigned char delimiter =
                            (unsigned char)bx_search_record_delimiter(opts);
                        bool has_delim = record_len > 0u
                            && ((unsigned char)line[record_len - 1u] == delimiter);

                        bx_search_print_match_colored_cached((unsigned char *)line,
                                                             record_len,
                                                             bm.start,
                                                             bm.end,
                                                             has_delim,
                                                             opts,
                                                             out,
                                                             color,
                                                             delimiter);
                    }
                    bx_search_streaming_maybe_flush_line_buffered(opts);
                    stdout_emitted = true;
                }
            }
            if (opts->max_count > 0 && file_matches >= opts->max_count)
                break;
        } else if (opts->passthru) {
            bool prefix_printed = bx_search_print_result_prefix(
                display_name,
                opts,
                line_num,
                0u,
                false,
                line_offset,
                bx_search_context_field_separator(opts));

            bx_search_maybe_emit_initial_tab(opts, prefix_printed);
            bx_search_print_plain_record_contents((unsigned char *)line, record_len, opts);
            if (len == 0 || line[len - 1] != bx_search_record_delimiter(opts))
                bx_search_write_record_terminator(opts);
            bx_search_dev_counters_note_output_line_emitted();
            bx_search_streaming_maybe_flush_line_buffered(opts);
            stdout_emitted = true;
        } else if (opts->stop_on_nonmatch && saw_match_record) {
            break;
        }
    }

    if (bx_search_matcher_had_error(m)) {
        (void)bx_search_report_matcher_error(progname, display_name, m, opts);
        if (!use_stdin)
            fclose(f);
        return 2;
    }
    if (bx_record_stream_had_error(record_stream)) {
        int errnum = bx_record_stream_error(record_stream);
        if (errnum == EOVERFLOW) {
            bx_search_report_record_too_large(progname,
                                              display_name ? display_name : "(standard input)",
                                              opts);
        } else {
            bx_search_report_path_error(progname,
                                        display_name ? display_name : "(standard input)",
                                        errnum != 0 ? errnum : EIO,
                                        opts);
        }
        if (!use_stdin)
            fclose(f);
        return 2;
    }

    if (opts->quiet && file_matches > 0)
        status = 0;
    if (opts->count_only)
        bx_search_print_count_result(display_name, opts, file_matches);
    if (opts->files_with_matches && file_matches > 0 && display_name) {
        bx_search_print_path_record(display_name, opts);
        bx_search_dev_counters_note_output_line_emitted();
        bx_search_streaming_maybe_flush_line_buffered(opts);
    }
    if (opts->files_without_match && file_matches == 0 && display_name) {
        bx_search_print_path_record(display_name, opts);
        bx_search_dev_counters_note_output_line_emitted();
        bx_search_streaming_maybe_flush_line_buffered(opts);
    }
    if (stats && file_matches > 0)
        stats->files_with_matches++;
    if (match_count)
        *match_count += file_matches;
    if (!use_stdin)
        fclose(f);
    return status;
}

int bx_search_streaming_path(const char *filename,
                             const char *display_name,
                             const char *progname,
                             struct bx_matcher *m,
                             struct search_opts *opts,
                             int *match_count,
                             struct bx_record_stream *record_stream,
                             struct bx_search_stats *stats) {
    bool use_stdin = false;
    FILE *f = bx_search_input_open_stream(filename, progname, opts, record_stream, &use_stdin);

    if (!f)
        return 2;
    return bx_search_streaming_opened(f, use_stdin, display_name, progname, m, opts,
                                      match_count, record_stream, stats);
}
