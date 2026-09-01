#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "binary_scan.h"
#include "context_roll.h"
#include "dev_counters.h"
#include "lib/color.h"
#include "pcre2_matcher.h"
#include "record_stream.h"
#include "search_buffered.h"
#include "search_input.h"
#include "search_internal.h"
#include "search_plan.h"

struct bx_search_buffered_output_state {
    bool heading_printed_for_file;
    bool context_output_started_for_file;
    bool printed_context_line;
    size_t last_emitted_line;
};

static void bx_search_buffered_close_input(FILE *f, bool use_stdin) {
    if (!use_stdin)
        fclose(f);
}

static int bx_search_buffered_path_error(
    FILE *f,
    bool use_stdin,
    struct bx_search_context_roll *before,
    const char *display_name,
    const char *progname,
    struct search_opts *opts,
    int errnum
) {
    bx_search_buffered_close_input(f, use_stdin);
    bx_search_context_roll_dispose(before);
    bx_search_report_path_error(progname,
                                display_name ? display_name : "(standard input)",
                                errnum != 0 ? errnum : EIO,
                                opts);
    return 2;
}

static int bx_search_buffered_matcher_error(
    FILE *f,
    bool use_stdin,
    struct bx_search_context_roll *before,
    const char *display_name,
    const char *progname,
    struct bx_matcher *m,
    struct search_opts *opts
) {
    bx_search_buffered_close_input(f, use_stdin);
    bx_search_context_roll_dispose(before);
    (void)bx_search_report_matcher_error(progname, display_name, m, opts);
    return 2;
}

static void bx_search_buffered_emit_summary(const char *display_name,
                                            struct search_opts *opts,
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
}

static bool bx_search_buffered_emit_record(
    const struct bx_search_context_record *record,
    bool selected,
    const struct bx_match *selected_match,
    const char *display_name,
    struct bx_matcher *m,
    struct search_opts *opts,
    struct bx_search_buffered_output_state *output
) {
    const unsigned char *text = record->text;

    if (selected) {
        struct bx_match bm = selected_match ? *selected_match : (struct bx_match){0};

        if (opts->vimgrep && !opts->invert_match) {
            bx_search_maybe_print_heading(display_name, opts,
                                          &output->heading_printed_for_file);
            bx_search_print_vimgrep_matches(
                text, record->len,
                output->heading_printed_for_file ? NULL : display_name,
                (int)record->line_number, record->byte_offset, m, opts);
        } else if (opts->only_matching && !opts->invert_match) {
            bx_search_maybe_print_heading(display_name, opts,
                                          &output->heading_printed_for_file);
            bx_search_print_only_matches_from_first(
                text, record->len,
                output->heading_printed_for_file ? NULL : display_name,
                (int)record->line_number, record->byte_offset, m, &bm, opts);
        } else {
            bool prefix_printed;

            bx_search_maybe_print_heading(display_name, opts,
                                          &output->heading_printed_for_file);
            prefix_printed = bx_search_print_result_prefix(
                output->heading_printed_for_file ? NULL : display_name,
                opts, (int)record->line_number, bm.start + 1u, true,
                record->byte_offset, bx_search_match_field_separator(opts));
            if (opts->only_matching && opts->invert_match)
                return true;
            bx_search_maybe_emit_initial_tab(opts, prefix_printed);
            if (opts->invert_match) {
                bx_search_print_plain_record_contents(text, record->len, opts);
                if (record->len == 0u ||
                    text[record->len - 1u] !=
                        (unsigned char)bx_search_record_delimiter(opts)) {
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

                bx_search_print_match_colored_cached(
                    text, record->len, bm.start, bm.end, has_delim,
                    opts, bx_search_output_stream(), bx_color_enabled(), delimiter);
            }
        }
        return !bx_search_matcher_had_error(m);
    }

    bx_search_maybe_print_heading(display_name, opts,
                                  &output->heading_printed_for_file);
    {
        bool prefix_printed = bx_search_print_result_prefix(
            output->heading_printed_for_file ? NULL : display_name,
            opts, (int)record->line_number, 0u, false, record->byte_offset,
            bx_search_context_field_separator(opts));

        bx_search_maybe_emit_initial_tab(opts, prefix_printed);
    }
    bx_search_print_plain_record_contents(text, record->len, opts);
    if (record->len == 0u ||
        text[record->len - 1u] != (unsigned char)bx_search_record_delimiter(opts)) {
        bx_search_write_record_terminator(opts);
    }
    bx_search_dev_counters_note_output_line_emitted();
    return true;
}

static bool bx_search_buffered_prepare_context_record(
    size_t line_number,
    struct search_opts *opts,
    struct bx_search_buffered_output_state *output
) {
    if (!output->context_output_started_for_file) {
        (void)bx_search_maybe_emit_context_file_separator(opts);
        output->context_output_started_for_file = true;
        bx_search_note_context_output_started();
    }
    if (output->printed_context_line &&
        line_number > output->last_emitted_line &&
        line_number - output->last_emitted_line > 1u &&
        !opts->suppress_group_separator) {
        bx_search_printf_out("%s\n",
                             opts->group_separator ? opts->group_separator : "--");
        bx_search_dev_counters_note_output_line_emitted();
    }
    if (output->printed_context_line &&
        line_number <= output->last_emitted_line) {
        return false;
    }
    return true;
}

static bool bx_search_buffered_emit_context_record(
    const struct bx_search_context_record *record,
    bool selected,
    const struct bx_match *selected_match,
    const char *display_name,
    struct bx_matcher *m,
    struct search_opts *opts,
    struct bx_search_buffered_output_state *output
) {
    if (!bx_search_buffered_prepare_context_record(record->line_number, opts, output))
        return true;
    if (!bx_search_buffered_emit_record(record, selected, selected_match,
                                        display_name, m, opts, output)) {
        return false;
    }
    output->printed_context_line = true;
    output->last_emitted_line = record->line_number;
    return true;
}

static bool bx_search_buffered_emit_before(
    struct bx_search_context_roll *before,
    const char *display_name,
    struct bx_matcher *m,
    struct search_opts *opts,
    struct bx_search_buffered_output_state *output
) {
    size_t count = bx_search_context_roll_count(before);

    for (size_t i = 0u; i < count; i++) {
        struct bx_search_context_record record;

        if (!bx_search_context_roll_get(before, i, &record) ||
            !bx_search_buffered_emit_context_record(
                &record, false, NULL, display_name, m, opts, output)) {
            return false;
        }
    }
    bx_search_context_roll_clear(before);
    return true;
}

static void bx_search_buffered_add_stats(struct bx_search_stats *stats,
                                         int file_matches,
                                         size_t matched_lines) {
    if (!stats)
        return;
    stats->files_searched++;
    if (file_matches > 0) {
        stats->matches += file_matches;
        stats->matched_lines += matched_lines;
        stats->files_with_matches++;
    }
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
    struct bx_search_context_roll before = {0};
    struct bx_search_buffered_output_state output = {0};
    const bool with_context = bx_search_plan_needs_line_buffering(opts);
    const bool emits_text = !opts->quiet && !opts->count_only &&
        !opts->files_with_matches && !opts->files_without_match;
    ssize_t len;
    int file_matches = 0;
    size_t matched_lines = 0u;
    size_t file_offset = 0u;
    size_t line_number = 0u;
    size_t after_left = 0u;
    bool max_count_reached = false;
    bool saw_match_record = false;
    bool binary_detected = false;
    bool binary_diagnostic_reported = false;
    bool published_output = false;
    bool stop = false;

    while (!stop &&
           (len = bx_search_input_read_record_until_binary(
                f, record_stream, opts, NULL)) != -1) {
        unsigned char *raw = (unsigned char *)record_stream->record;
        struct bx_search_context_record current;
        struct bx_match bm = {0};
        size_t match_len;
        int match_rc;
        bool matched;
        bool selected;
        bool record_has_nul;
        int record_match_count = 0;

        line_number++;
        current.text = raw;
        current.len = (size_t)len;
        current.byte_offset = file_offset;
        current.line_number = line_number;
        file_offset += (size_t)len;
        if (stats)
            stats->bytes_searched += (size_t)len;

        record_has_nul = !opts->null_data && !opts->binary_as_text &&
            len > 0 && raw[len - 1] == '\0';
        if (record_has_nul && !binary_detected) {
            binary_detected = true;
            bx_search_dev_counters_note_binary_policy_check();
            if (emits_text)
                bx_search_context_roll_clear(&before);
        }

        if (binary_detected && opts->binary_without_match) {
            if (!published_output) {
                file_matches = 0;
                matched_lines = 0u;
            }
            stop = true;
            continue;
        }

        match_len = record_has_nul
            ? (size_t)len - 1u
            : bx_search_record_match_len(raw, (size_t)len, opts);
        match_rc = bx_search_matcher_find_with_opts(m, raw, match_len, 0, opts, &bm);
        if (match_rc < 0) {
            return bx_search_buffered_matcher_error(
                f, use_stdin, &before, display_name, progname, m, opts);
        }
        matched = match_rc == 0;
        if (opts->invert_match)
            matched = !matched;
        selected = matched &&
            !(opts->max_count > 0 && file_matches >= opts->max_count);

        if (selected && !opts->invert_match) {
            record_match_count = opts->count_matches
                ? bx_search_count_record_matches(m, raw, match_len, opts)
                : 1;
            if (bx_search_matcher_had_error(m)) {
                return bx_search_buffered_matcher_error(
                    f, use_stdin, &before, display_name, progname, m, opts);
            }
        }

        if (!selected && opts->stop_on_nonmatch && saw_match_record)
            break;
        if (with_context)
            bx_search_dev_counters_note_context_buffer_entry();

        if (selected) {
            file_matches += opts->count_matches ? record_match_count : 1;
            matched_lines++;
            saw_match_record = true;

            if (emits_text && binary_detected) {
                if (!binary_diagnostic_reported) {
                    bx_search_report_binary_match(progname, display_name);
                    binary_diagnostic_reported = true;
                }
                stop = true;
            } else if (emits_text) {
                bool emitted;

                if (with_context) {
                    emitted = bx_search_buffered_emit_before(
                            &before, display_name, m, opts, &output) &&
                        bx_search_buffered_emit_context_record(
                            &current, true, &bm, display_name, m, opts, &output);
                } else {
                    emitted = bx_search_buffered_emit_record(
                        &current, true, &bm, display_name, m, opts, &output);
                }
                if (!emitted) {
                    return bx_search_buffered_matcher_error(
                        f, use_stdin, &before, display_name, progname, m, opts);
                }
                published_output = true;
            }

            after_left = with_context ? (size_t)opts->after_context : 0u;
            if (opts->max_count > 0 && file_matches >= opts->max_count)
                max_count_reached = true;
            if ((opts->quiet ||
                 ((opts->files_with_matches || opts->files_without_match) &&
                  !opts->stats && !opts->binary_without_match)) ||
                (max_count_reached && after_left == 0u)) {
                stop = true;
            }
            continue;
        }

        if (binary_detected && emits_text) {
            if (!published_output) {
                int binary_match = bx_search_binary_scan_remaining(
                    f, m, opts, record_stream, stats);

                if (binary_match < 0) {
                    if (bx_search_matcher_had_error(m)) {
                        return bx_search_buffered_matcher_error(
                            f, use_stdin, &before, display_name, progname, m, opts);
                    }
                    return bx_search_buffered_path_error(
                        f, use_stdin, &before, display_name, progname, opts,
                        bx_record_stream_error(record_stream) != 0
                            ? bx_record_stream_error(record_stream)
                            : EIO);
                }
                if (binary_match > 0) {
                    file_matches++;
                    matched_lines++;
                    if (!binary_diagnostic_reported) {
                        bx_search_report_binary_match(progname, display_name);
                        binary_diagnostic_reported = true;
                    }
                }
            }
            stop = true;
            continue;
        }

        if (with_context && emits_text) {
            if (after_left > 0u) {
                if (!bx_search_buffered_emit_context_record(
                        &current, false, NULL, display_name, m, opts, &output)) {
                    return bx_search_buffered_matcher_error(
                        f, use_stdin, &before, display_name, progname, m, opts);
                }
                after_left--;
                if (max_count_reached && after_left == 0u)
                    stop = true;
            } else if (!bx_search_context_roll_push(
                           &before, raw, (size_t)len, current.byte_offset,
                           current.line_number, (size_t)opts->before_context)) {
                return bx_search_buffered_path_error(
                    f, use_stdin, &before, display_name, progname, opts,
                           errno != 0 ? errno : ENOMEM);
            }
        } else if (opts->passthru && emits_text) {
            if (!bx_search_buffered_emit_record(
                    &current, false, NULL, display_name, m, opts, &output)) {
                return bx_search_buffered_matcher_error(
                    f, use_stdin, &before, display_name, progname, m, opts);
            }
            published_output = true;
        }
    }

    bx_search_buffered_close_input(f, use_stdin);
    f = NULL;
    if (bx_record_stream_had_error(record_stream)) {
        int errnum = bx_record_stream_error(record_stream);

        bx_search_context_roll_dispose(&before);
        if (errnum == EOVERFLOW) {
            bx_search_report_record_too_large(
                progname, display_name ? display_name : "(standard input)", opts);
        } else {
            bx_search_report_path_error(
                progname, display_name ? display_name : "(standard input)",
                errnum != 0 ? errnum : EIO, opts);
        }
        return 2;
    }

    bx_search_context_roll_dispose(&before);
    if (opts->count_only || opts->files_with_matches || opts->files_without_match)
        bx_search_buffered_emit_summary(display_name, opts, file_matches);
    bx_search_buffered_add_stats(stats, file_matches, matched_lines);
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
    FILE *f = bx_search_input_open_stream(filename, progname, opts,
                                          record_stream, &use_stdin);

    if (!f)
        return 2;
    return bx_search_buffered_opened(f, use_stdin, display_name, progname, m, opts,
                                     match_count, record_stream, stats);
}
