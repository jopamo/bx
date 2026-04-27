#define _GNU_SOURCE
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "dev_counters.h"
#include "pcre2_matcher.h"
#include "rg_output.h"
#include "rg_transform.h"
#include "search_buffered.h"
#include "search_input.h"
#include "search_internal.h"
#include "search_multiline.h"
#include "search_plan.h"
#include "search_raw_presence.h"
#include "search_scanner.h"
#include "search_streaming.h"

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

    if (operand_st_loaded && (S_ISCHR(operand_st.st_mode) || S_ISBLK(operand_st.st_mode)
                              || S_ISFIFO(operand_st.st_mode) || S_ISSOCK(operand_st.st_mode))) {
        FILE *f;

        if (bx_search_should_skip_special_input_mode(operand_st.st_mode, opts)) {
            result = 1;
            goto out;
        }

        f = bx_search_input_open_stream(filename, progname, opts, record_stream, NULL);
        if (!f)
            goto out_error;

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

        result = search_file_run_path_kernel(stdin_kernel,
                                             filename, display_name, progname, m, opts,
                                             match_count, scanner, record_stream, stats);
        goto out;
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
                                                   m, opts, match_count, scanner,
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

                if (opts->quiet || opts->files_with_matches ||
                    opts->files_without_match || opts->count_only) {
                    result = search_file_run_opened_kernel(
                        exec_plan ? exec_plan->binary_search_kernel
                                  : BX_SEARCH_FILE_KERNEL_STREAMING,
                        f, false, filename, display_name, progname, m, opts,
                        match_count, scanner, record_stream, stats);
                    goto out;
                }

                bool matched = binary_file_matches_opened(f, m, opts, record_stream);
                fclose(f);
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

            enum bx_search_file_kernel_kind nonbinary_kernel =
                exec_plan ? exec_plan->opened_nonbinary_kernel
                          : BX_SEARCH_FILE_KERNEL_STREAMING;

            result = search_file_run_opened_kernel(
                search_file_resolve_opened_kernel(f, nonbinary_kernel),
                f, false, filename, display_name, progname, m, opts,
                match_count, scanner, record_stream, stats);
            goto out;
        }

        {
            struct stat opened_st;
            if (fstat(fileno(f), &opened_st) == 0 && !S_ISREG(opened_st.st_mode)) {
                result = search_file_opened_without_reopen(f, false, display_name, progname,
                                                           m, exec_plan, opts, match_count,
                                                           scanner, record_stream, stats);
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

        if (opts->quiet || opts->files_with_matches ||
            opts->files_without_match || opts->count_only) {
            result = search_file_run_path_kernel(exec_plan
                                                     ? exec_plan->binary_search_kernel
                                                     : BX_SEARCH_FILE_KERNEL_STREAMING,
                                                 filename, display_name, progname, m, opts,
                                                 match_count, scanner, record_stream, stats);
            goto out;
        }

        if (binary_file_matches(filename, m, opts, record_stream)) {
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
