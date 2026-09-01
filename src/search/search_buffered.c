#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "context_stage.h"
#include "dev_counters.h"
#include "lib/color.h"
#include "pcre2_matcher.h"
#include "record_stream.h"
#include "search_buffered.h"
#include "search_input.h"
#include "search_internal.h"
#include "search_plan.h"

static void bx_search_buffered_close_input(FILE *f, bool use_stdin) {
    if (!use_stdin)
        fclose(f);
}

static int bx_search_buffered_error(FILE *f,
                                    bool use_stdin,
                                    FILE *records,
                                    FILE *intervals,
                                    const char *display_name,
                                    const char *progname,
                                    struct search_opts *opts,
                                    int errnum) {
    bx_search_buffered_close_input(f, use_stdin);
    if (records)
        fclose(records);
    if (intervals)
        fclose(intervals);
    bx_search_report_path_error(progname,
                                display_name ? display_name : "(standard input)",
                                errnum != 0 ? errnum : EIO,
                                opts);
    return 2;
}

static int bx_search_buffered_matcher_error(FILE *f,
                                            bool use_stdin,
                                            FILE *records,
                                            FILE *intervals,
                                            const char *display_name,
                                            const char *progname,
                                            struct bx_matcher *m,
                                            struct search_opts *opts) {
    bx_search_buffered_close_input(f, use_stdin);
    if (records)
        fclose(records);
    if (intervals)
        fclose(intervals);
    (void)bx_search_report_matcher_error(progname, display_name, m, opts);
    return 2;
}

static int bx_search_buffered_emit_summary(const char *display_name,
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

static bool bx_search_buffered_emit_record(
    const struct bx_search_staged_record *record,
    const unsigned char *text,
    bool selected,
    const char *display_name,
    struct bx_matcher *m,
    struct search_opts *opts,
    bool *heading_printed_for_file
) {
    if (selected) {
        struct bx_match bm = {0};
        size_t match_len = bx_search_record_match_len(text, record->len, opts);

        if (!opts->invert_match &&
            bx_search_matcher_find_with_opts(m, text, match_len, 0, opts, &bm) < 0) {
            return false;
        }
        if (opts->vimgrep && !opts->invert_match) {
            bx_search_maybe_print_heading(display_name, opts, heading_printed_for_file);
            bx_search_print_vimgrep_matches(
                text, record->len,
                *heading_printed_for_file ? NULL : display_name,
                (int)record->line_number, record->byte_offset, m, opts);
        } else if (opts->only_matching && !opts->invert_match) {
            bx_search_maybe_print_heading(display_name, opts, heading_printed_for_file);
            bx_search_print_only_matches_from_first(
                text, record->len,
                *heading_printed_for_file ? NULL : display_name,
                (int)record->line_number, record->byte_offset, m, &bm, opts);
        } else {
            bool prefix_printed;

            bx_search_maybe_print_heading(display_name, opts, heading_printed_for_file);
            prefix_printed = bx_search_print_result_prefix(
                *heading_printed_for_file ? NULL : display_name,
                opts, (int)record->line_number, bm.start + 1u, true,
                record->byte_offset, bx_search_match_field_separator(opts));
            if (opts->only_matching && opts->invert_match)
                return true;
            bx_search_maybe_emit_initial_tab(opts, prefix_printed);
            if (opts->invert_match) {
                bx_search_print_plain_record_contents(text, record->len, opts);
                if (record->len == 0u ||
                    text[record->len - 1u] != (unsigned char)bx_search_record_delimiter(opts)) {
                    bx_search_write_record_terminator(opts);
                }
                bx_search_dev_counters_note_output_line_emitted();
                return true;
            }
            if (bx_search_should_omit_long_match_line(opts, record->len)) {
                bx_search_print_omitted_long_line(opts);
            } else if (opts->replace) {
                bx_search_print_replaced_record(text, record->len, m, opts);
            } else {
                unsigned char delimiter =
                    (unsigned char)bx_search_record_delimiter(opts);
                bool has_delim = record->len > 0u &&
                    text[record->len - 1u] == delimiter;

                bx_search_print_match_colored_cached(text, record->len,
                                                     bm.start, bm.end, has_delim,
                                                     opts, bx_search_output_stream(),
                                                     bx_color_enabled(), delimiter);
            }
        }
        return !bx_search_matcher_had_error(m);
    }

    bx_search_maybe_print_heading(display_name, opts, heading_printed_for_file);
    bool prefix_printed = bx_search_print_result_prefix(
        *heading_printed_for_file ? NULL : display_name,
        opts, (int)record->line_number, 0u, false, record->byte_offset,
        bx_search_context_field_separator(opts));
    bx_search_maybe_emit_initial_tab(opts, prefix_printed);
    bx_search_print_plain_record_contents(text, record->len, opts);
    if (record->len == 0u ||
        text[record->len - 1u] != (unsigned char)bx_search_record_delimiter(opts)) {
        bx_search_write_record_terminator(opts);
    }
    bx_search_dev_counters_note_output_line_emitted();
    return true;
}

static bool bx_search_buffered_emit_staged(FILE *records,
                                           FILE *intervals,
                                           bool with_context,
                                           const char *display_name,
                                           struct bx_matcher *m,
                                           struct search_opts *opts,
                                           struct bx_record_stream *record_stream) {
    struct bx_search_staged_record record;
    struct bx_search_context_interval interval = {0};
    bool have_interval = false;
    bool heading_printed_for_file = false;
    bool emitted_file_separator = false;
    bool printed_any_line = false;
    int interval_rc = 0;
    int read_rc;

    if (!bx_search_staged_rewind(records))
        return false;
    if (with_context) {
        interval_rc = bx_search_context_interval_read(intervals, &interval);
        if (interval_rc < 0)
            return false;
        have_interval = interval_rc > 0;
    }

    while ((read_rc = bx_search_staged_record_read(
                records, &record, record_stream->record)) > 0) {
        bool print_record = record.selected;

        record_stream->record[record.len] = '\0';
        if (with_context) {
            while (have_interval && record.line_number > interval.last) {
                interval_rc = bx_search_context_interval_read(intervals, &interval);
                if (interval_rc < 0)
                    return false;
                have_interval = interval_rc > 0;
            }
            print_record = have_interval &&
                record.line_number >= interval.first &&
                record.line_number <= interval.last;
            if (!print_record)
                continue;
            if (!emitted_file_separator) {
                (void)bx_search_maybe_emit_context_file_separator(opts);
                emitted_file_separator = true;
            }
            if (printed_any_line && record.line_number == interval.first &&
                record.line_number > 1u && !opts->suppress_group_separator) {
                bx_search_printf_out("%s\n",
                                     opts->group_separator ? opts->group_separator : "--");
                bx_search_dev_counters_note_output_line_emitted();
            }
        } else if (!print_record) {
            continue;
        }

        if (!bx_search_buffered_emit_record(&record,
                                            (unsigned char *)record_stream->record,
                                            record.selected,
                                            display_name, m, opts,
                                            &heading_printed_for_file)) {
            return false;
        }
        printed_any_line = true;
    }
    if (read_rc < 0)
        return false;
    if (with_context && printed_any_line)
        bx_search_note_context_output_started();
    return true;
}

int bx_search_buffered_opened(FILE *f,
                              bool use_stdin,
                              const char *display_name,
                              const char *progname,
                              struct bx_matcher *m,
                              struct search_opts *opts,
                              int *match_count,
                              struct bx_record_stream *record_stream,
                              struct bx_search_stats *stats) {
    FILE *records = NULL;
    FILE *intervals = NULL;
    ssize_t len;
    int file_matches = 0;
    int after_left = -1;
    size_t file_offset = 0u;
    size_t record_count = 0u;
    bool saw_binary = false;
    bool saw_match_record = false;
    bool with_context = bx_search_plan_needs_line_buffering(opts);
    bool needs_text_records = !opts->quiet && !opts->count_only &&
        !opts->files_with_matches && !opts->files_without_match;

    if (needs_text_records) {
        records = tmpfile();
        if (!records)
            return bx_search_buffered_error(f, use_stdin, NULL, NULL, display_name,
                                            progname, opts, errno);
    }

    while ((len = bx_search_input_read_record(f, record_stream, opts)) != -1) {
        char *raw = record_stream->record;
        struct bx_match bm = {0};
        size_t match_len;
        int match_rc;
        bool matched;
        bool selected;
        int record_match_count = 0;

        if (!opts->null_data && memchr(raw, '\0', (size_t)len) != NULL)
            saw_binary = true;
        if (stats)
            stats->bytes_searched += (size_t)len;

        match_len = bx_search_record_match_len((unsigned char *)raw, (size_t)len, opts);
        match_rc =
            bx_search_matcher_find_with_opts(m, (unsigned char *)raw, match_len, 0, opts, &bm);
        if (match_rc < 0)
            return bx_search_buffered_matcher_error(f, use_stdin, records, NULL,
                                                    display_name, progname, m, opts);
        matched = match_rc == 0;
        if (opts->invert_match)
            matched = !matched;
        selected = matched;

        if (matched && !opts->invert_match) {
            record_match_count = opts->count_matches
                ? bx_search_count_record_matches(m, (unsigned char *)raw, match_len, opts)
                : 1;
            if (bx_search_matcher_had_error(m))
                return bx_search_buffered_matcher_error(f, use_stdin, records, NULL,
                                                        display_name, progname, m, opts);
        }
        if (matched && opts->max_count > 0 && file_matches >= opts->max_count)
            selected = false;

        record_count++;
        if (selected) {
            file_matches += opts->count_matches ? record_match_count : 1;
            if (stats) {
                stats->matches += opts->count_matches ? record_match_count : 1;
                stats->matched_lines++;
            }
            saw_match_record = true;
        }

        if (!selected && opts->stop_on_nonmatch && saw_match_record) {
            record_count--;
            break;
        }
        if (needs_text_records && (with_context || selected)) {
            const struct bx_search_staged_record staged = {
                .len = (size_t)len,
                .byte_offset = file_offset,
                .line_number = record_count,
                .selected = selected,
            };
            if (!bx_search_staged_record_write(records, &staged, raw))
                return bx_search_buffered_error(f, use_stdin, records, NULL,
                                                display_name, progname, opts,
                                                errno != 0 ? errno : EIO);
        }
        if (with_context)
            bx_search_dev_counters_note_context_buffer_entry();
        file_offset += (size_t)len;

        if (selected && opts->max_count > 0 && file_matches >= opts->max_count) {
            after_left = opts->after_context;
            if (after_left == 0)
                break;
            continue;
        }
        if (after_left > 0) {
            after_left--;
            if (after_left == 0)
                break;
        }
    }
    bx_search_buffered_close_input(f, use_stdin);
    f = NULL;

    if (bx_record_stream_had_error(record_stream)) {
        int errnum = bx_record_stream_error(record_stream);
        if (records)
            fclose(records);
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
        return 2;
    }
    if (stats)
        stats->files_searched++;

    if (saw_binary && !opts->binary_as_text) {
        bx_search_dev_counters_note_binary_policy_check();
        if (records)
            fclose(records);
        if (opts->binary_without_match)
            return bx_search_binary_without_match(display_name, opts, match_count, stats);
        if (opts->quiet && file_matches > 0) {
            if (stats)
                stats->files_with_matches++;
            if (match_count)
                *match_count += file_matches;
            return 0;
        }
        if (opts->count_only || opts->files_with_matches || opts->files_without_match)
            return bx_search_buffered_emit_summary(display_name, opts, match_count, stats,
                                                   file_matches);
        if (file_matches > 0) {
            bx_search_report_binary_match(progname, display_name);
            if (stats)
                stats->files_with_matches++;
            if (match_count)
                *match_count += file_matches;
            return 0;
        }
        return 1;
    }

    if (opts->quiet && file_matches > 0) {
        if (records)
            fclose(records);
        if (stats)
            stats->files_with_matches++;
        if (match_count)
            *match_count += file_matches;
        return 0;
    }
    if (opts->count_only || opts->files_with_matches || opts->files_without_match) {
        if (records)
            fclose(records);
        return bx_search_buffered_emit_summary(display_name, opts, match_count, stats,
                                               file_matches);
    }

    if (with_context && file_matches > 0) {
        intervals = tmpfile();
        if (!intervals ||
            !bx_search_context_stage_build_intervals(records, intervals, record_count,
                                                     opts->before_context,
                                                     opts->after_context)) {
            int errnum = errno != 0 ? errno : EIO;
            if (records)
                fclose(records);
            if (intervals)
                fclose(intervals);
            bx_search_report_path_error(progname,
                                        display_name ? display_name : "(standard input)",
                                        errnum, opts);
            return 2;
        }
    }

    if (file_matches > 0 &&
        !bx_search_buffered_emit_staged(records, intervals, with_context,
                                        display_name, m, opts, record_stream)) {
        int errnum = errno != 0 ? errno : EIO;
        bool matcher_error = bx_search_matcher_had_error(m);
        fclose(records);
        if (intervals)
            fclose(intervals);
        if (matcher_error) {
            (void)bx_search_report_matcher_error(progname, display_name, m, opts);
        } else {
            bx_search_report_path_error(progname,
                                        display_name ? display_name : "(standard input)",
                                        errnum, opts);
        }
        return 2;
    }
    if (records)
        fclose(records);
    if (intervals)
        fclose(intervals);
    if (stats && file_matches > 0)
        stats->files_with_matches++;
    if (match_count)
        *match_count += file_matches;
    return file_matches > 0 ? 0 : 1;
}

int bx_search_buffered_path(const char *filename,
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
    return bx_search_buffered_opened(f, use_stdin, display_name, progname, m, opts,
                                     match_count, record_stream, stats);
}
