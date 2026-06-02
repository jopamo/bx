#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dev_counters.h"
#include "lib/color.h"
#include "pcre2_matcher.h"
#include "record_stream.h"
#include "search_buffered.h"
#include "search_input.h"
#include "search_internal.h"
#include "search_plan.h"

struct bx_search_buffered_line {
    char *text;
    size_t len;
    size_t byte_offset;
    bool match;
    bool print;
    int match_count;
};

static void bx_search_buffered_free_lines(struct bx_search_buffered_line *lines,
                                          int count) {
    for (int i = 0; i < count; i++)
        free(lines[i].text);
    free(lines);
}

static int bx_search_buffered_allocation_error(FILE *f,
                                               bool use_stdin,
                                               const char *display_name,
                                               const char *progname,
                                               struct search_opts *opts,
                                               struct bx_search_buffered_line *lines,
                                               int nlines) {
    if (!use_stdin)
        fclose(f);
    bx_search_buffered_free_lines(lines, nlines);
    bx_search_report_path_error(progname,
                                display_name ? display_name : "(standard input)",
                                ENOMEM,
                                opts);
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

int bx_search_buffered_opened(FILE *f,
                              bool use_stdin,
                              const char *display_name,
                              const char *progname,
                              struct bx_matcher *m,
                              struct search_opts *opts,
                              int *match_count,
                              struct bx_record_stream *record_stream,
                              struct bx_search_stats *stats) {
    int cap = 256;
    struct bx_search_buffered_line *lines = malloc((size_t)cap * sizeof(*lines));
    int nlines = 0;
    ssize_t len;
    int file_matches = 0;
    int after_left = -1;
    size_t file_offset = 0u;
    bool saw_binary = false;
    bool saw_match_record = false;
    bool heading_printed_for_file = false;
    bool count_context_buffer_entries = bx_search_plan_needs_line_buffering(opts);

    if (!lines)
        return bx_search_buffered_allocation_error(f, use_stdin, display_name, progname,
                                                   opts, NULL, 0);

    while ((len = bx_search_input_read_record(f, record_stream, opts)) != -1) {
        char *raw = record_stream->record;

        if (!opts->null_data && memchr(raw, '\0', (size_t)len) != NULL)
            saw_binary = true;
        if (nlines >= cap) {
            if (cap > INT_MAX / 2)
                return bx_search_buffered_allocation_error(f, use_stdin, display_name,
                                                           progname, opts, lines, nlines);
            int new_cap = cap * 2;
            struct bx_search_buffered_line *grown =
                realloc(lines, (size_t)new_cap * sizeof(*lines));
            if (!grown)
                return bx_search_buffered_allocation_error(f, use_stdin, display_name,
                                                           progname, opts, lines, nlines);
            lines = grown;
            cap = new_cap;
        }
        if ((size_t)len > SIZE_MAX - 1u)
            return bx_search_buffered_allocation_error(f, use_stdin, display_name,
                                                       progname, opts, lines, nlines);
        char *line_text = malloc((size_t)len + 1u);
        if (!line_text)
            return bx_search_buffered_allocation_error(f, use_stdin, display_name,
                                                       progname, opts, lines, nlines);
        memcpy(line_text, raw, (size_t)len + 1u);
        lines[nlines].text = line_text;
        lines[nlines].len = (size_t)len;
        lines[nlines].byte_offset = file_offset;
        lines[nlines].print = false;
        if (stats)
            stats->bytes_searched += (size_t)len;

        struct bx_match bm;
        size_t match_len = bx_search_record_match_len((unsigned char *)raw, (size_t)len, opts);
        bool matched =
            bx_search_matcher_find_with_opts(m, (unsigned char *)raw, match_len, 0, opts, &bm)
            == 0;

        file_offset += (size_t)len;
        if (opts->invert_match)
            matched = !matched;

        bool selected = matched;
        int record_match_count = 0;
        if (matched && !opts->invert_match) {
            record_match_count = opts->count_matches
                ? bx_search_count_record_matches(m, (unsigned char *)raw, match_len, opts)
                : 1;
        }
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
                if (count_context_buffer_entries)
                    bx_search_dev_counters_note_context_buffer_entry();
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
            if (count_context_buffer_entries)
                bx_search_dev_counters_note_context_buffer_entry();
            nlines++;
            if (after_left == 0)
                break;
            continue;
        }
        if (count_context_buffer_entries)
            bx_search_dev_counters_note_context_buffer_entry();
        nlines++;
    }
    if (!use_stdin)
        fclose(f);
    if (bx_record_stream_had_error(record_stream)) {
        int errnum = bx_record_stream_error(record_stream);
        bx_search_buffered_free_lines(lines, nlines);
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
        if (opts->binary_without_match) {
            bx_search_buffered_free_lines(lines, nlines);
            return bx_search_binary_without_match(display_name, opts, match_count, stats);
        }

        if (opts->quiet && file_matches > 0) {
            if (stats && file_matches > 0)
                stats->files_with_matches++;
            bx_search_buffered_free_lines(lines, nlines);
            if (match_count)
                *match_count += file_matches;
            return 0;
        }

        if (opts->count_only || opts->files_with_matches || opts->files_without_match) {
            int rc = bx_search_buffered_emit_summary(display_name, opts, match_count, stats,
                                                     file_matches);
            bx_search_buffered_free_lines(lines, nlines);
            return rc;
        }

        if (file_matches > 0) {
            bx_search_report_binary_match(progname, display_name);
            if (stats)
                stats->files_with_matches++;
            if (match_count)
                *match_count += file_matches;
            bx_search_buffered_free_lines(lines, nlines);
            return 0;
        }

        bx_search_buffered_free_lines(lines, nlines);
        return 1;
    }

    if (opts->quiet && file_matches > 0) {
        if (stats)
            stats->files_with_matches++;
        bx_search_buffered_free_lines(lines, nlines);
        if (match_count)
            *match_count += file_matches;
        return 0;
    }

    if (opts->count_only || opts->files_with_matches || opts->files_without_match) {
        int rc = bx_search_buffered_emit_summary(display_name, opts, match_count, stats,
                                                 file_matches);
        bx_search_buffered_free_lines(lines, nlines);
        return rc;
    }

    for (int i = 0; i < nlines; i++) {
        if (!lines[i].match)
            continue;
        int start = i - opts->before_context;
        if (start < 0)
            start = 0;
        int end = i + opts->after_context + 1;
        if (end > nlines)
            end = nlines;
        for (int j = start; j < end; j++)
            lines[j].print = true;
    }

    bool want_group_separator = bx_search_plan_needs_line_buffering(opts);
    bool in_group = false;
    bool emitted_file_separator = false;
    bool printed_any_line = false;
    int last_printed = -1;
    for (int i = 0; i < nlines; i++) {
        if (!lines[i].print) {
            in_group = false;
            continue;
        }
        if (want_group_separator && !emitted_file_separator) {
            (void)bx_search_maybe_emit_context_file_separator(opts);
            emitted_file_separator = true;
        }
        if (!in_group && last_printed >= 0 && i > last_printed + 1) {
            if (want_group_separator && !opts->suppress_group_separator) {
                bx_search_printf_out("%s\n",
                                     opts->group_separator ? opts->group_separator : "--");
                bx_search_dev_counters_note_output_line_emitted();
            }
        }
        if (lines[i].match) {
            struct bx_match bm;
            size_t match_len = bx_search_record_match_len((unsigned char *)lines[i].text,
                                                          lines[i].len, opts);

            bx_search_matcher_find_with_opts(
                m, (unsigned char *)lines[i].text, match_len, 0, opts, &bm
            );
            if (opts->vimgrep && !opts->invert_match) {
                bx_search_maybe_print_heading(display_name, opts, &heading_printed_for_file);
                bx_search_print_vimgrep_matches(
                    (unsigned char *)lines[i].text, lines[i].len,
                    heading_printed_for_file ? NULL : display_name,
                    i + 1, lines[i].byte_offset, m, opts);
            } else if (opts->only_matching && !opts->invert_match) {
                bx_search_maybe_print_heading(display_name, opts, &heading_printed_for_file);
                bx_search_print_only_matches_from_first(
                    (unsigned char *)lines[i].text, lines[i].len,
                    heading_printed_for_file ? NULL : display_name,
                    i + 1, lines[i].byte_offset, m, &bm, opts);
            } else {
                bx_search_maybe_print_heading(display_name, opts, &heading_printed_for_file);
                bool prefix_printed = bx_search_print_result_prefix(
                    heading_printed_for_file ? NULL : display_name,
                    opts, i + 1, bm.start + 1u, true, lines[i].byte_offset,
                    bx_search_match_field_separator(opts));
                if (opts->only_matching && opts->invert_match)
                    continue;
                bx_search_maybe_emit_initial_tab(opts, prefix_printed);
                if (opts->invert_match) {
                    bx_search_print_plain_record_contents((unsigned char *)lines[i].text,
                                                          lines[i].len, opts);
                    if (lines[i].len == 0
                        || lines[i].text[lines[i].len - 1]
                               != bx_search_record_delimiter(opts)) {
                        bx_search_write_record_terminator(opts);
                    }
                    bx_search_dev_counters_note_output_line_emitted();
                    continue;
                }
                if (bx_search_should_omit_long_match_line(opts, lines[i].len)) {
                    bx_search_print_omitted_long_line(opts);
                } else if (opts->replace) {
                    bx_search_print_replaced_record((unsigned char *)lines[i].text,
                                                    lines[i].len, m, opts);
                } else {
                    FILE *out = bx_search_output_stream();
                    bool color = bx_color_enabled();
                    unsigned char delimiter =
                        (unsigned char)bx_search_record_delimiter(opts);
                    bool has_delim = lines[i].len > 0u
                        && ((unsigned char)lines[i].text[lines[i].len - 1u] == delimiter);

                    bx_search_print_match_colored_cached((unsigned char *)lines[i].text,
                                                         lines[i].len,
                                                         bm.start,
                                                         bm.end,
                                                         has_delim,
                                                         opts,
                                                         out,
                                                         color,
                                                         delimiter);
                }
            }
        } else {
            bx_search_maybe_print_heading(display_name, opts, &heading_printed_for_file);
            bool prefix_printed = bx_search_print_result_prefix(
                heading_printed_for_file ? NULL : display_name,
                opts, i + 1, 0u, false, lines[i].byte_offset,
                bx_search_context_field_separator(opts));
            bx_search_maybe_emit_initial_tab(opts, prefix_printed);
            bx_search_print_plain_record_contents((unsigned char *)lines[i].text,
                                                  lines[i].len, opts);
            if (lines[i].len == 0
                || lines[i].text[lines[i].len - 1] != bx_search_record_delimiter(opts)) {
                bx_search_write_record_terminator(opts);
            }
            bx_search_dev_counters_note_output_line_emitted();
        }
        in_group = true;
        last_printed = i;
        printed_any_line = true;
    }
    if (want_group_separator && printed_any_line)
        bx_search_note_context_output_started();
    if (stats && file_matches > 0)
        stats->files_with_matches++;
    if (match_count)
        *match_count += file_matches;
    bx_search_buffered_free_lines(lines, nlines);
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
