#define _GNU_SOURCE
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dev_counters.h"
#include "lib/color.h"
#include "lib/path_quote.h"
#include "literal.h"
#include "pcre2_matcher.h"
#include "rg_output.h"
#include "rg_text.h"
#include "search_internal.h"

static _Thread_local struct bx_search_output_ctx *current_output_ctx = NULL;
static _Thread_local int current_offset_width = 0;

char *bx_search_quote_path_metadata_for_terminal(const char *path,
                                                 const struct search_opts *opts) {
    if (!path || !opts || !opts->terminal_quote_paths)
        return NULL;

    const struct bx_path_control_quote_options quote_options = {
        .style = BX_PATH_CONTROL_QUOTE_QUESTION,
        .high_bit_printable = true,
    };
    return bx_path_quote_control_dup(path, &quote_options);
}

void bx_search_print_path_record(const char *display_name,
                                 struct search_opts *opts) {
    if (!display_name)
        return;

    char *quoted = bx_search_quote_path_metadata_for_terminal(display_name, opts);
    const char *output_name = quoted ? quoted : display_name;
    bx_search_printf_out("%s%c", output_name, opts && opts->null_output ? '\0' : '\n');
    free(quoted);
}

static FILE *bx_search_null_stream(void) {
    static FILE *stream = NULL;

    if (!stream)
        stream = fopen("/dev/null", "wb");
    return stream ? stream : stderr;
}

struct bx_search_output_ctx *bx_search_output_ctx_push(struct bx_search_output_ctx *ctx) {
    struct bx_search_output_ctx *previous = current_output_ctx;

    current_output_ctx = ctx;
    return previous;
}

void bx_search_output_ctx_pop(struct bx_search_output_ctx *previous) {
    current_output_ctx = previous;
}

int bx_search_compute_offset_width_from_stat(const struct stat *st,
                                             const struct search_opts *opts) {
    if (!opts || !opts->initial_tab || !st || st->st_size < 0)
        return 0;

    uintmax_t num = (uintmax_t)st->st_size;
    if (opts->show_line_number && num < UINTMAX_MAX)
        num++;

    int width = 1;
    while (num >= 10u) {
        width++;
        num /= 10u;
    }
    return width;
}

int bx_search_output_get_offset_width(void) {
    return current_offset_width;
}

void bx_search_output_set_offset_width(int width) {
    current_offset_width = width;
}

FILE *bx_search_output_stream(void) {
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

FILE *bx_search_error_output_stream(void) {
    return bx_search_error_stream();
}

static bool bx_search_stream_is_private(FILE *stream) {
    if (!current_output_ctx || !stream)
        return false;
    if (current_output_ctx->capture_out_buf && current_output_ctx->capture_out_len &&
        current_output_ctx->out == stream) {
        return true;
    }
    if (current_output_ctx->capture_err_buf && current_output_ctx->capture_err_len &&
        current_output_ctx->err == stream) {
        return true;
    }
    return false;
}

bool bx_search_stdout_is_captured(void) {
    return current_output_ctx
        && current_output_ctx->capture_out_buf
        && current_output_ctx->capture_out_len;
}

void bx_search_note_stdout_output(void) {
    if (current_output_ctx)
        current_output_ctx->emitted_stdout = true;
}

int bx_search_check_output_error(void) {
    FILE *out = bx_search_output_stream();
    int saved_errno = 0;

    if (!out)
        return 0;
    if (fflush(out) == EOF)
        saved_errno = errno;
    if (!ferror(out))
        return 0;
    return saved_errno != 0 ? saved_errno : (errno != 0 ? errno : EIO);
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
    if (bx_search_stream_is_private(stream))
        written = fwrite_unlocked(buf, 1u, len, stream);
    else
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
    rc = bx_search_stream_is_private(stream) ? putc_unlocked(ch, stream) : fputc(ch, stream);
    if (rc != EOF)
        bx_search_note_stdout_output();
    return rc;
}

static bool bx_search_only_matching_can_use_exact_literal_fast(
    const struct bx_literal_matcher *literal,
    const struct search_opts *opts,
    bool color
) {
    if (!literal || !opts || color)
        return false;
    if (opts->word_regexp || opts->line_regexp || opts->trim || opts->initial_tab)
        return false;
    if (opts->show_line_number || opts->show_column || opts->show_byte_offset)
        return false;
    return bx_literal_candidates_are_exact(literal);
}

static void bx_search_print_only_matches_exact_literal_fast(
    const unsigned char *line,
    size_t len,
    const char *display_name,
    struct bx_literal_matcher *literal,
    const struct bx_match *first_match,
    struct search_opts *opts,
    FILE *out
) {
    enum { BX_SEARCH_ONLY_MATCHING_STACK_BUF = 512 };
    size_t match_len = bx_search_record_match_len(line, len, opts);
    const char *sep = bx_search_match_field_separator(opts);
    char *quoted_display_name = bx_search_quote_path_metadata_for_terminal(display_name, opts);
    const char *output_display_name = quoted_display_name ? quoted_display_name : display_name;
    size_t display_name_len = output_display_name ? strlen(output_display_name) : 0u;
    size_t sep_len = opts->null_filename ? 1u : strlen(sep);
    size_t prefix_len = (opts->show_filename && output_display_name) ? display_name_len + sep_len : 0u;
    unsigned char delimiter = (unsigned char)bx_search_record_delimiter(opts);
    unsigned char stack_buf[BX_SEARCH_ONLY_MATCHING_STACK_BUF];
    unsigned char *prefix_buf = stack_buf;
    struct bx_match bm = {0};
    size_t start = 0u;
    bool have_match = false;

    if (bx_literal_len(literal) == 0u)
        return;
    if (prefix_len > sizeof(stack_buf)) {
        prefix_buf = malloc(prefix_len);
        if (!prefix_buf) {
            free(quoted_display_name);
            return;
        }
    }

    if (prefix_len > 0u) {
        memcpy(prefix_buf, output_display_name, display_name_len);
        if (opts->null_filename)
            prefix_buf[display_name_len] = '\0';
        else
            memcpy(prefix_buf + display_name_len, sep, sep_len);
    }

    if (first_match && first_match->start <= first_match->end && first_match->end <= match_len) {
        bm = *first_match;
        have_match = true;
    }

    while (have_match || start <= match_len) {
        size_t matched_len;

        if (!have_match && bx_literal_find(literal, line, match_len, start, &bm) != 0)
            break;
        matched_len = bm.end - bm.start;
        bx_search_fwrite_stream(out, prefix_buf, prefix_len);
        bx_search_fwrite_stream(out, line + bm.start, matched_len);
        bx_search_putc_stream(out, delimiter);
        bx_search_stats_count_bytes(prefix_len + matched_len + 1u);
        bx_search_dev_counters_note_output_line_emitted();
        start = bm.end > bm.start ? bm.end : bm.start + 1u;
        have_match = false;
    }

    if (prefix_buf != stack_buf)
        free(prefix_buf);
    free(quoted_display_name);
}

int bx_search_printf_out(const char *fmt, ...) {
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

static size_t printable_trim_prefix(const unsigned char *line, size_t len,
                                    const struct search_opts *opts) {
    size_t match_len = bx_search_record_match_len(line, len, opts);

    if (!opts || !opts->trim)
        return 0u;
    return bx_rg_trim_leading_ascii_space(line, match_len);
}

void bx_search_print_plain_record_contents(const unsigned char *line,
                                           size_t len,
                                           struct search_opts *opts) {
    FILE *out = bx_search_output_stream();
    size_t trim_prefix = printable_trim_prefix(line, len, opts);

    bx_search_fwrite_stream(out, line + trim_prefix, len - trim_prefix);
    bx_search_stats_count_bytes(len - trim_prefix);
}

void bx_search_print_match_colored_cached(const unsigned char *line,
                                          size_t len,
                                          size_t match_start,
                                          size_t match_end,
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
        bx_search_stats_count_bytes(match_start - trim_prefix);
        if (color)
            bx_rg_emit_color_style_start_file(out, &opts->rg_colors.match);
        bx_search_fwrite_stream(out, line + match_start, match_end - match_start);
        bx_search_stats_count_bytes(match_end - match_start);
        if (color)
            bx_rg_emit_color_reset_file(out);
        bx_search_fwrite_stream(out, line + match_end, len - match_end);
        bx_search_stats_count_bytes(len - match_end);
        if (!has_delim)
            bx_search_putc_stream(out, delimiter);
    } else {
        bx_search_fwrite_stream(out, line + match_start, match_end - match_start);
        bx_search_stats_count_bytes(match_end - match_start);
        bx_search_putc_stream(out, delimiter);
    }
    bx_search_dev_counters_note_output_line_emitted();
}

static void print_replacement_piece(const char *replace,
                                    const unsigned char *match,
                                    size_t match_len) {
    if (!replace) {
        bx_search_fwrite_out(match, match_len);
        bx_search_stats_count_bytes(match_len);
        return;
    }

    for (const char *p = replace; *p; ++p) {
        if (p[0] == '$' && p[1] == '0') {
            bx_search_fwrite_out(match, match_len);
            bx_search_stats_count_bytes(match_len);
            ++p;
            continue;
        }
        bx_search_putc_out((unsigned char)*p);
        bx_search_stats_count_bytes(1u);
    }
}

void bx_search_print_replaced_record(const unsigned char *line,
                                     size_t len,
                                     struct bx_matcher *m,
                                     struct search_opts *opts) {
    size_t match_len = bx_search_record_match_len(line, len, opts);
    size_t trim_prefix = printable_trim_prefix(line, len, opts);
    size_t start = trim_prefix;
    size_t cursor = trim_prefix;

    while (start <= match_len) {
        struct bx_match bm;

        if (bx_search_matcher_find_with_opts(m, line, match_len, start, opts, &bm) != 0)
            break;
        bx_search_fwrite_out(line + cursor, bm.start - cursor);
        bx_search_stats_count_bytes(bm.start - cursor);
        print_replacement_piece(opts->replace, line + bm.start, bm.end - bm.start);
        cursor = bm.end;
        start = bm.end > bm.start ? bm.end : bm.start + 1u;
    }

    bx_search_fwrite_out(line + cursor, match_len - cursor);
    bx_search_stats_count_bytes(match_len - cursor);
    bx_search_write_record_terminator(opts);
    bx_search_dev_counters_note_output_line_emitted();
}

bool bx_search_should_omit_long_match_line(const struct search_opts *opts, size_t record_len) {
    return opts->max_columns > 0 && !opts->only_matching && (int)record_len > opts->max_columns;
}

void bx_search_print_omitted_long_line(struct search_opts *opts) {
    FILE *out = bx_search_output_stream();

    bx_search_fputs_stream(out, "[Omitted long matching line]");
    bx_search_stats_count_bytes(strlen("[Omitted long matching line]"));
    bx_search_putc_stream(out, (unsigned char)bx_search_record_delimiter(opts));
    bx_search_dev_counters_note_output_line_emitted();
}

const char *bx_search_match_field_separator(struct search_opts *opts) {
    return opts->field_match_separator ? opts->field_match_separator : ":";
}

const char *bx_search_context_field_separator(struct search_opts *opts) {
    return opts->field_context_separator ? opts->field_context_separator : "-";
}

bool bx_search_print_result_prefix_cached(const char *display_name,
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
        char *quoted_display_name = bx_search_quote_path_metadata_for_terminal(display_name, opts);
        const char *output_display_name = quoted_display_name ? quoted_display_name : display_name;
        size_t output_display_name_len = quoted_display_name ? strlen(quoted_display_name) : display_name_len;

        if (!color && !opts->show_line_number && !opts->show_column
            && !opts->show_byte_offset && !opts->null_filename) {
            bx_search_fwrite_stream(out, output_display_name, output_display_name_len);
            bx_search_stats_count_bytes(output_display_name_len);
            bx_search_fwrite_stream(out, sep, sep_len);
            bx_search_stats_count_bytes(sep_len);
            free(quoted_display_name);
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
        bx_search_fwrite_stream(out, output_display_name, output_display_name_len);
        bx_search_stats_count_bytes(output_display_name_len);
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
        bx_search_stats_count_bytes(sep_len);
        free(quoted_display_name);
        printed = true;
    }
    if (opts->show_line_number) {
        if (color)
            bx_rg_emit_color_style_start_file(out, &opts->rg_colors.line);
        int n = current_offset_width > 0
            ? bx_search_printf_stream(out, "%*d", current_offset_width, line_num)
            : bx_search_printf_stream(out, "%d", line_num);
        if (n > 0)
            bx_search_stats_count_bytes((size_t)n);
        if (color)
            bx_rg_emit_color_reset_file(out);
        bx_search_fwrite_stream(out, sep, sep_len);
        bx_search_stats_count_bytes(sep_len);
        printed = true;
    }
    if (opts->show_column && has_column) {
        if (color)
            bx_rg_emit_color_style_start_file(out, &opts->rg_colors.column);
        int n = current_offset_width > 0
            ? bx_search_printf_stream(out, "%*zu", current_offset_width, column)
            : bx_search_printf_stream(out, "%zu", column);
        if (n > 0)
            bx_search_stats_count_bytes((size_t)n);
        if (color)
            bx_rg_emit_color_reset_file(out);
        bx_search_fwrite_stream(out, sep, sep_len);
        bx_search_stats_count_bytes(sep_len);
        printed = true;
    }
    if (opts->show_byte_offset) {
        if (color)
            bx_rg_emit_color_style_start_file(out, &opts->rg_colors.line);
        int n = current_offset_width > 0
            ? bx_search_printf_stream(out, "%*zu", current_offset_width, byte_offset)
            : bx_search_printf_stream(out, "%zu", byte_offset);
        if (n > 0)
            bx_search_stats_count_bytes((size_t)n);
        if (color)
            bx_rg_emit_color_reset_file(out);
        bx_search_fwrite_stream(out, sep, sep_len);
        bx_search_stats_count_bytes(sep_len);
        printed = true;
    }
    return printed;
}

bool bx_search_print_result_prefix(const char *display_name,
                                   struct search_opts *opts,
                                   int line_num,
                                   size_t column,
                                   bool has_column,
                                   size_t byte_offset,
                                   const char *sep) {
    FILE *out = bx_search_output_stream();
    bool color = bx_color_enabled();
    size_t display_name_len = display_name ? strlen(display_name) : 0u;
    size_t sep_len = (opts && opts->null_filename) ? 1u : strlen(sep);

    return bx_search_print_result_prefix_cached(display_name, display_name_len,
                                                opts, line_num, column, has_column,
                                                byte_offset, sep, sep_len, out, color);
}

void bx_search_maybe_emit_initial_tab(const struct search_opts *opts,
                                      bool prefix_printed) {
    if (!opts || !opts->initial_tab || !prefix_printed)
        return;
    bx_search_putc_out('\t');
    bx_search_stats_count_bytes(1u);
}

bool bx_search_maybe_emit_context_file_separator(const struct search_opts *opts) {
    if (!current_output_ctx || !current_output_ctx->context_output_started || !opts)
        return false;
    if (opts->suppress_group_separator)
        return false;
    bx_search_printf_out("%s\n", opts->group_separator ? opts->group_separator : "--");
    bx_search_dev_counters_note_output_line_emitted();
    return true;
}

void bx_search_note_context_output_started(void) {
    if (current_output_ctx)
        current_output_ctx->context_output_started = true;
}

bool bx_search_use_heading_output(const char *display_name,
                                  const struct search_opts *opts) {
    return opts->heading && opts->show_filename && display_name && display_name[0] != '\0';
}

void bx_search_maybe_print_heading(const char *display_name,
                                   struct search_opts *opts,
                                   bool *heading_printed_for_file) {
    struct bx_search_output_ctx *ctx = current_output_ctx;

    if (!bx_search_use_heading_output(display_name, opts) || *heading_printed_for_file)
        return;
    if (ctx && ctx->heading_output_started)
        bx_search_putc_out('\n');

    char *hyperlink = bx_rg_hyperlink_open_dup(opts->hyperlink_format,
                                               opts->hostname_bin,
                                               display_name, 1u, 1u, false, false);
    char *quoted_display_name = bx_search_quote_path_metadata_for_terminal(display_name, opts);
    const char *output_display_name = quoted_display_name ? quoted_display_name : display_name;
    if (hyperlink)
        bx_search_fputs_out(hyperlink);
    bx_rg_emit_color_style_start_file(bx_search_output_stream(), &opts->rg_colors.path);
    bx_search_printf_out("%s", output_display_name);
    bx_rg_emit_color_reset_file(bx_search_output_stream());
    if (hyperlink) {
        bx_search_fputs_out(bx_rg_hyperlink_close());
        free(hyperlink);
    }
    free(quoted_display_name);
    bx_search_putc_out('\n');
    bx_search_dev_counters_note_output_line_emitted();
    *heading_printed_for_file = true;
    if (ctx) {
        ctx->heading_output_started = true;
        ctx->used_heading = true;
    }
}

void bx_search_print_only_matches(const unsigned char *line,
                                  size_t len,
                                  const char *display_name,
                                  int line_num,
                                  size_t byte_offset,
                                  struct bx_matcher *m,
                                  struct search_opts *opts) {
    bx_search_print_only_matches_from_first(line, len, display_name, line_num,
                                            byte_offset, m, NULL, opts);
}

void bx_search_print_only_matches_from_first(const unsigned char *line,
                                             size_t len,
                                             const char *display_name,
                                             int line_num,
                                             size_t byte_offset,
                                             struct bx_matcher *m,
                                             const struct bx_match *first_match,
                                             struct search_opts *opts) {
    size_t match_len = bx_search_record_match_len(line, len, opts);
    size_t start = 0u;
    FILE *out = bx_search_output_stream();
    bool color = bx_color_enabled();
    size_t display_name_len = display_name ? strlen(display_name) : 0u;
    const char *sep = bx_search_match_field_separator(opts);
    size_t sep_len = (opts && opts->null_filename) ? 1u : strlen(sep);
    unsigned char delimiter = (unsigned char)bx_search_record_delimiter(opts);
    struct bx_match bm = {0};
    bool have_match = false;
    struct bx_literal_matcher *literal = bx_search_matcher_literal(m);

    if (first_match && first_match->start <= first_match->end &&
        first_match->end <= match_len) {
        bm = *first_match;
        have_match = true;
    }

    if (bx_search_only_matching_can_use_exact_literal_fast(literal, opts, color)) {
        bx_search_print_only_matches_exact_literal_fast(line, len, display_name,
                                                        literal, have_match ? &bm : NULL,
                                                        opts, out);
        return;
    }

    while (have_match || start <= match_len) {
        if (!have_match &&
            bx_search_matcher_find_with_opts(m, line, match_len, start, opts, &bm) != 0) {
            break;
        }
        if (bm.end == bm.start) {
            start = bm.start + 1u;
            have_match = false;
            continue;
        }
        bool prefix_printed = bx_search_print_result_prefix_cached(display_name, display_name_len,
                                                                   opts, line_num,
                                                                   bm.start + 1u, true,
                                                                   byte_offset + bm.start,
                                                                   sep, sep_len, out, color);
        bx_search_maybe_emit_initial_tab(opts, prefix_printed);
        bx_search_fwrite_stream(out, line + bm.start, bm.end - bm.start);
        bx_search_stats_count_bytes(bm.end - bm.start);
        bx_search_putc_stream(out, delimiter);
        bx_search_stats_count_bytes(1u);
        bx_search_dev_counters_note_output_line_emitted();
        if (bm.end > bm.start)
            start = bm.end;
        else
            start = bm.start + 1u;
        have_match = false;
    }
}

void bx_search_print_vimgrep_matches(const unsigned char *line,
                                     size_t len,
                                     const char *display_name,
                                     int line_num,
                                     size_t byte_offset,
                                     struct bx_matcher *m,
                                     struct search_opts *opts) {
    size_t match_len = bx_search_record_match_len(line, len, opts);
    size_t start = 0u;
    FILE *out = bx_search_output_stream();
    bool color = bx_color_enabled();
    unsigned char delimiter = (unsigned char)bx_search_record_delimiter(opts);
    bool has_delim = len > 0u && line[len - 1u] == delimiter;

    while (start <= match_len) {
        struct bx_match bm;

        if (bx_search_matcher_find_with_opts(m, line, match_len, start, opts, &bm) != 0)
            break;
        bool prefix_printed = bx_search_print_result_prefix(display_name, opts, line_num,
                                                            bm.start + 1u, true,
                                                            byte_offset + bm.start,
                                                            bx_search_match_field_separator(opts));
        bx_search_maybe_emit_initial_tab(opts, prefix_printed);
        if (bx_search_should_omit_long_match_line(opts, len))
            bx_search_print_omitted_long_line(opts);
        else if (opts->replace)
            bx_search_print_replaced_record(line, len, m, opts);
        else
            bx_search_print_match_colored_cached(line, len, bm.start, bm.end, has_delim,
                                                 opts, out, color, delimiter);
        if (bm.end > bm.start)
            start = bm.end;
        else
            start = bm.start + 1u;
    }
}

void bx_search_write_record_terminator(const struct search_opts *opts) {
    bx_search_putc_out((unsigned char)bx_search_record_delimiter(opts));
    bx_search_stats_count_bytes(1u);
}

void bx_search_print_count_result(const char *display_name,
                                  struct search_opts *opts,
                                  int file_matches) {
    if (opts->omit_zero_count_output && file_matches == 0)
        return;
    if (opts->show_filename && display_name) {
        char *quoted_display_name = bx_search_quote_path_metadata_for_terminal(display_name, opts);
        const char *output_display_name = quoted_display_name ? quoted_display_name : display_name;
        bx_search_printf_out("%s%c%d\n", output_display_name,
                             opts->null_filename ? '\0' : ':', file_matches);
        free(quoted_display_name);
    }
    else
        bx_search_printf_out("%d\n", file_matches);
    bx_search_dev_counters_note_output_line_emitted();
}

void bx_search_print_stats_summary(struct bx_search_stats *stats) {
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

void bx_search_stats_count_bytes(size_t count) {
    if (current_output_ctx && current_output_ctx->stats)
        current_output_ctx->stats->bytes_printed += count;
}
