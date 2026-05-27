#define _GNU_SOURCE
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "dev_counters.h"
#include "lib/color.h"
#include "literal.h"
#include "search_internal.h"
#include "search_plan.h"
#include "search_scanner.h"

#define BX_SEARCH_SCANNER_MIN_FILE_SIZE 1u

static bool bx_search_scanner_can_shortcut_file_presence(const struct search_opts *opts) {
    if (!opts || opts->count_matches)
        return false;
    if (opts->quiet)
        return true;
    if (opts->count_only)
        return false;
    return (opts->files_with_matches || opts->files_without_match) && !opts->stats;
}

static bool bx_search_scanner_matcher_is_exact_literal_candidate(
    const struct bx_matcher *m,
    const struct search_opts *opts
) {
    struct bx_literal_matcher *literal;

    if (!m || !opts)
        return false;
    literal = bx_search_matcher_literal(m);
    if (!literal)
        return false;
    return !opts->word_regexp
        && !opts->line_regexp
        && bx_literal_candidates_are_exact(literal);
}

bool bx_search_scanner_stream_is_eligible(FILE *f) {
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

bool bx_search_scanner_can_use(const struct bx_matcher *m,
                               const struct search_opts *opts,
                               bool use_stdin) {
    if (!m || !opts || use_stdin)
        return false;
    if (opts->multiline || opts->invert_match)
        return false;
    if (bx_search_plan_needs_line_buffering(opts) || opts->replace
        || opts->passthru || opts->vimgrep) {
        return false;
    }
    if (opts->stop_on_nonmatch)
        return false;
    if (bx_color_enabled())
        return false;
    return bx_search_matcher_is_scanner_literal_eligible(m, opts);
}

bool bx_search_scanner_can_raw_shortcut_file_presence(const struct bx_matcher *m,
                                                      const struct search_opts *opts) {
    struct bx_literal_matcher *literal;

    if (!bx_search_scanner_can_shortcut_file_presence(opts))
        return false;
    if (opts->binary_without_match && !opts->quiet)
        return false;
    literal = bx_search_matcher_literal(m);
    return literal != NULL
        && !opts->line_regexp
        && !opts->word_regexp
        && bx_literal_candidates_are_exact(literal);
}

static char *bx_search_scanner_prepare_fast_plain_prefix(const char *display_name,
                                                         size_t display_name_len,
                                                         const char *match_sep,
                                                         size_t match_sep_len,
                                                         const struct search_opts *opts,
                                                         size_t *prefix_len_out) {
    size_t prefix_len = 0u;
    char *prefix;

    if (!display_name || !opts || !prefix_len_out)
        return NULL;

    prefix_len = display_name_len + (opts->null_filename ? 1u : match_sep_len);
    *prefix_len_out = prefix_len;
    if (prefix_len == 0u)
        return NULL;

    prefix = malloc(prefix_len);
    if (!prefix) {
        *prefix_len_out = 0u;
        return NULL;
    }

    if (display_name_len > 0u)
        memcpy(prefix, display_name, display_name_len);
    if (opts->null_filename) {
        prefix[display_name_len] = '\0';
    } else if (match_sep_len > 0u) {
        memcpy(prefix + display_name_len, match_sep, match_sep_len);
    }
    bx_search_dev_counters_note_scanner_plain_prefix_alloc();
    return prefix;
}

int bx_search_scanner_opened(FILE *f,
                             bool use_stdin,
                             const char *display_name,
                             const char *progname,
                             struct bx_matcher *m,
                             struct search_opts *opts,
                             int *match_count,
                             struct bx_search_scanner *scanner,
                             struct bx_search_stats *stats) {
    struct bx_literal_matcher *literal = bx_search_matcher_literal(m);
    int file_matches = 0;
    int status = 1;
    bool heading_printed_for_file = false;
    bool stop = false;
    bool shortcut_file_presence = bx_search_scanner_can_shortcut_file_presence(opts);
    bool need_line_numbers = opts->show_line_number;
    bool exact_literal_candidates =
        bx_search_scanner_matcher_is_exact_literal_candidate(m, opts);
    bool heading_enabled = bx_search_use_heading_output(display_name, opts);
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
    const char *match_sep = bx_search_match_field_separator(opts);
    size_t match_sep_len = opts->null_filename ? 1u : strlen(match_sep);
    size_t display_name_len = 0u;
    bool display_name_len_ready = false;
    unsigned char delimiter = (unsigned char)bx_search_record_delimiter(opts);
    FILE *out = NULL;
    bool output_is_captured = bx_search_stdout_is_captured();
    bool prefer_memmem_candidates = opts->quiet ||
        (!heading_enabled &&
         !opts->show_filename &&
         !opts->files_with_matches &&
         !opts->files_without_match);
    char *fast_plain_prefix = NULL;
    size_t fast_plain_prefix_len = 0u;

    if (!f || !m || !opts || !scanner || !literal)
        return 2;

    if (stats)
        stats->files_searched++;

    bx_search_scanner_begin_file(scanner, (char)delimiter, need_line_numbers);
    while (!stop && bx_search_scanner_read_chunk(scanner, f)) {
        size_t next_line_num = scanner->records_before_buf + 1u;
        size_t numbered_until = 0u;

        if (stats)
            stats->bytes_searched += scanner->scan_len;

        size_t cursor = 0u;
        while (!stop) {
            struct bx_search_candidate candidate;
            struct bx_match bm;

            if (!bx_search_scanner_next_literal_candidate(scanner, literal,
                                                          prefer_memmem_candidates,
                                                          &cursor, &candidate))
                break;

            if (shortcut_file_presence) {
                if (!exact_literal_candidates) {
                    if (!bx_search_matcher_verify_literal_candidate_with_opts(
                            m, scanner->buf, scanner->scan_len, candidate.chunk_off, opts, &bm)) {
                        continue;
                    }
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
            /*
             * Candidate detection owns the no-match fast path. Only recover a
             * record after a literal candidate survived the presence-only
             * shortcut and may need line-oriented verification or output.
             */
            if (!bx_search_scanner_expand_record(scanner, &candidate, &record))
                continue;

            size_t candidate_record_off = candidate.chunk_off - record.chunk_off;
            size_t match_len = 0u;
            if (exact_literal_candidates) {
                bm.start = candidate_record_off;
                bm.end = candidate_record_off + candidate.anchor_len;
            } else {
                match_len = bx_search_record_match_len(record.data, record.len, opts);
                if (!bx_search_matcher_verify_literal_candidate_with_opts(
                        m, record.data, match_len, candidate_record_off, opts, &bm)) {
                    if (bx_search_matcher_find_with_opts(m, record.data, match_len, 0, opts,
                                                         &bm) != 0) {
                        continue;
                    }
                }
            }

            cursor = record.chunk_off + record.len;
            size_t line_num = 0u;
            /*
             * Keep delimiter counting cold until a literal candidate already
             * forced record recovery for a potential match.
             */
            if (need_line_numbers) {
                if (record.chunk_off > numbered_until) {
                    next_line_num += bx_search_scanner_count_delimiters_range(
                        scanner, numbered_until, record.chunk_off);
                }
                line_num = next_line_num;
                numbered_until = record.chunk_off + record.len;
                next_line_num++;
            }

            int record_match_count = 1;
            if (opts->count_matches) {
                if (match_len == 0u)
                    match_len = bx_search_record_match_len(record.data, record.len, opts);
                record_match_count = bx_search_count_record_matches(m, record.data, match_len,
                                                                    opts);
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
                bx_search_maybe_print_heading(display_name, opts, &heading_printed_for_file);
            if (!out)
                out = bx_search_output_stream();

            if (opts->only_matching) {
                bx_search_print_only_matches_from_first(
                    record.data, record.len,
                    heading_printed_for_file ? NULL : display_name,
                    (int)line_num, (size_t)record.file_off, m, &bm, opts
                );
            } else if (can_omit_long_line && (int)record.len > opts->max_columns) {
                bx_search_print_omitted_long_line(opts);
            } else if (fast_plain_line_output) {
                const char *prefix_name = heading_printed_for_file ? NULL : display_name;
                size_t prefix_name_len = 0u;
                size_t printed_bytes = 0u;

                if (!heading_printed_for_file && display_name && !display_name_len_ready) {
                    display_name_len = strlen(display_name);
                    display_name_len_ready = true;
                }
                prefix_name_len = heading_printed_for_file ? 0u : display_name_len;
                if (!fast_plain_prefix
                    && !heading_printed_for_file
                    && opts->show_filename
                    && display_name) {
                    fast_plain_prefix = bx_search_scanner_prepare_fast_plain_prefix(
                        display_name, display_name_len, match_sep, match_sep_len,
                        opts, &fast_plain_prefix_len
                    );
                }

                if (!output_is_captured)
                    flockfile(out);
                if (fast_plain_prefix && !heading_printed_for_file) {
                    printed_bytes += fwrite_unlocked(fast_plain_prefix, 1u,
                                                     fast_plain_prefix_len, out);
                } else if (opts->show_filename && prefix_name) {
                    if (prefix_name_len > 0u)
                        printed_bytes += fwrite_unlocked(prefix_name, 1u, prefix_name_len, out);
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
                bx_search_stats_count_bytes(printed_bytes);
                bx_search_dev_counters_note_output_line_emitted();
            } else {
                bool prefix_printed = bx_search_print_result_prefix_cached(
                    heading_printed_for_file ? NULL : display_name,
                    heading_printed_for_file ? 0u : display_name_len,
                    opts, (int)line_num, bm.start + 1u, true,
                    (size_t)record.file_off,
                    match_sep, match_sep_len, out, color);
                if (need_initial_tab && prefix_printed) {
                    if (putc('\t', out) != EOF)
                        bx_search_stats_count_bytes(1u);
                }
                bx_search_print_match_colored_cached(record.data, record.len, bm.start, bm.end,
                                                     record.has_delim, opts, out, color,
                                                     delimiter);
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
        bx_search_print_count_result(display_name, opts, file_matches);
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
