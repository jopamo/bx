#ifndef BX_SEARCH_SEARCH_INTERNAL_H
#define BX_SEARCH_SEARCH_INTERNAL_H

#include <stdbool.h>
#include <stdio.h>
#include <sys/stat.h>

#include "fswalk/walk.h"
#include "options.h"
#include "record_stream.h"
#include "scanner.h"

struct bx_matcher;
struct bx_match;
struct bx_lit_plan;
struct bx_literal_matcher;
struct bx_search_exec_plan;

struct bx_search_stats {
    int matches;
    int matched_lines;
    int files_with_matches;
    int files_searched;
    size_t bytes_printed;
    size_t bytes_searched;
};

struct bx_search_output_ctx {
    FILE *out;
    FILE *err;
    char **capture_out_buf;
    size_t *capture_out_len;
    char **capture_err_buf;
    size_t *capture_err_len;
    struct bx_search_stats *stats;
    bool heading_output_started;
    bool context_output_started;
    bool used_heading;
    bool emitted_stdout;
    bool capture_failed;
};

bool bx_search_progname_uses_os_error_style(const char *progname);
int bx_search_fprintf_path_error(FILE *stream,
                                 const char *progname,
                                 const char *path,
                                 int errnum);
int bx_search_fprintf_path_io_error(FILE *stream,
                                    const char *progname,
                                    const char *path,
                                    int errnum);
int bx_search_snprintf_path_error(char *buf,
                                  size_t cap,
                                  const char *progname,
                                  const char *path,
                                  int errnum);
void bx_search_report_path_error(const char *progname,
                                 const char *path,
                                 int errnum,
                                 const struct search_opts *opts);
void bx_search_report_write_error(const char *progname, int errnum);
void bx_search_report_binary_match(const char *progname, const char *path);
void bx_search_report_record_too_large(const char *progname,
                                       const char *path,
                                       const struct search_opts *opts);
bool bx_search_path_exceeds_max_filesize(const char *path,
                                         const struct search_opts *opts);
bool bx_search_entry_exceeds_max_filesize(struct bx_walk_entry *entry,
                                          const struct search_opts *opts);
bool bx_search_entry_can_skip_max_filesize_zero_literal(
    const struct bx_walk_entry *entry,
    const struct bx_search_exec_plan *exec_plan,
    const struct search_opts *opts);
bool bx_search_should_skip_special_input_mode(mode_t mode,
                                              const struct search_opts *opts);
bool bx_search_entry_should_skip_special_input(struct bx_walk_entry *entry,
                                               const struct search_opts *opts);
bool bx_search_entry_should_skip_recursive_special_input(struct bx_walk_entry *entry,
                                                         const struct search_opts *opts);
char *bx_search_display_path_for_output(const char *path,
                                        bool strip_dot_prefix,
                                        const struct search_opts *opts);
struct bx_search_output_ctx *bx_search_output_ctx_push(struct bx_search_output_ctx *ctx);
void bx_search_output_ctx_pop(struct bx_search_output_ctx *previous);
int bx_search_compute_offset_width_from_stat(const struct stat *st,
                                             const struct search_opts *opts);
int bx_search_output_get_offset_width(void);
void bx_search_output_set_offset_width(int width);
FILE *bx_search_output_stream(void);
FILE *bx_search_error_output_stream(void);
bool bx_search_stdout_is_captured(void);
void bx_search_note_stdout_output(void);
int bx_search_check_output_error(void);
int bx_search_printf_out(const char *fmt, ...);
void bx_search_stats_count_bytes(size_t count);
char bx_search_record_delimiter(const struct search_opts *opts);
size_t bx_search_record_match_len(const unsigned char *buf,
                                  size_t len,
                                  const struct search_opts *opts);
void bx_search_write_record_terminator(const struct search_opts *opts);
void bx_search_print_plain_record_contents(const unsigned char *line,
                                           size_t len,
                                           struct search_opts *opts);
bool bx_search_matcher_is_scanner_literal_eligible(const struct bx_matcher *m,
                                                   const struct search_opts *opts);
struct bx_matcher *bx_search_compile_matcher(const char *pattern,
                                             enum bx_search_personality personality,
                                             struct search_opts *opts,
                                             char **errmsg,
                                             char **warningmsg);
void bx_search_matcher_free(struct bx_matcher *m);
struct bx_literal_matcher *bx_search_matcher_literal(const struct bx_matcher *m);
const struct bx_lit_plan *bx_search_matcher_absence_plan(const struct bx_matcher *m);
int bx_search_matcher_find_with_opts(struct bx_matcher *m,
                                     const unsigned char *buf,
                                     size_t len,
                                     size_t start,
                                     struct search_opts *opts,
                                     struct bx_match *out);
bool bx_search_matcher_verify_literal_candidate_with_opts(
    struct bx_matcher *m,
    const unsigned char *buf,
    size_t len,
    size_t candidate_start,
    struct search_opts *opts,
    struct bx_match *out);
int bx_search_count_record_matches(struct bx_matcher *m,
                                   const unsigned char *buf,
                                   size_t len,
                                   struct search_opts *opts);
const char *bx_search_match_field_separator(struct search_opts *opts);
const char *bx_search_context_field_separator(struct search_opts *opts);
bool bx_search_print_result_prefix(const char *display_name,
                                   struct search_opts *opts,
                                   int line_num,
                                   size_t column,
                                   bool has_column,
                                   size_t byte_offset,
                                   const char *sep);
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
                                          bool color);
void bx_search_maybe_emit_initial_tab(const struct search_opts *opts,
                                      bool prefix_printed);
bool bx_search_maybe_emit_context_file_separator(const struct search_opts *opts);
void bx_search_note_context_output_started(void);
void bx_search_print_match_colored_cached(const unsigned char *line,
                                          size_t len,
                                          size_t match_start,
                                          size_t match_end,
                                          bool has_delim,
                                          struct search_opts *opts,
                                          FILE *out,
                                          bool color,
                                          unsigned char delimiter);
bool bx_search_use_heading_output(const char *display_name,
                                  const struct search_opts *opts);
void bx_search_maybe_print_heading(const char *display_name,
                                   struct search_opts *opts,
                                   bool *heading_printed_for_file);
void bx_search_print_only_matches(const unsigned char *line,
                                  size_t len,
                                  const char *display_name,
                                  int line_num,
                                  size_t byte_offset,
                                  struct bx_matcher *m,
                                  struct search_opts *opts);
void bx_search_print_only_matches_from_first(const unsigned char *line,
                                             size_t len,
                                             const char *display_name,
                                             int line_num,
                                             size_t byte_offset,
                                             struct bx_matcher *m,
                                             const struct bx_match *first_match,
                                             struct search_opts *opts);
void bx_search_print_vimgrep_matches(const unsigned char *line,
                                     size_t len,
                                     const char *display_name,
                                     int line_num,
                                     size_t byte_offset,
                                     struct bx_matcher *m,
                                     struct search_opts *opts);
void bx_search_print_replaced_record(const unsigned char *line,
                                     size_t len,
                                     struct bx_matcher *m,
                                     struct search_opts *opts);
bool bx_search_should_omit_long_match_line(const struct search_opts *opts,
                                           size_t record_len);
void bx_search_print_omitted_long_line(struct search_opts *opts);
void bx_search_print_count_result(const char *display_name,
                                  struct search_opts *opts,
                                  int file_matches);
void bx_search_print_stats_summary(struct bx_search_stats *stats);
int bx_search_binary_without_match(const char *display_name,
                                   struct search_opts *opts,
                                   int *match_count,
                                   struct bx_search_stats *stats);
int bx_search_search_transformed_buffer(unsigned char *buf,
                                        size_t len,
                                        const char *display_name,
                                        const char *progname,
                                        struct bx_matcher *m,
                                        const struct bx_search_exec_plan *exec_plan,
                                        struct search_opts *opts,
                                        int *match_count,
                                        struct bx_record_stream *record_stream,
                                        struct bx_search_stats *stats);
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
                          struct bx_search_stats *stats);
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
                                struct bx_search_stats *stats);
struct bx_walk_opts bx_search_make_walk_opts(const char *progname,
                                             enum bx_search_personality personality,
                                             const struct search_opts *opts,
                                             bool *stop);
struct bx_walk_filter_opts bx_search_make_filter_opts(const struct search_opts *opts);
struct bx_walk_ignore_opts bx_search_make_ignore_opts(const char *progname,
                                                      const struct search_opts *opts);
bool bx_search_explicit_entry_selected(const struct search_opts *opts,
                                       const char *path);

#endif
