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
    bool used_heading;
    bool emitted_stdout;
    bool capture_failed;
};

bool bx_search_progname_uses_os_error_style(const char *progname);
void bx_search_report_path_error(const char *progname,
                                 const char *path,
                                 int errnum,
                                 const struct search_opts *opts);
bool bx_search_path_exceeds_max_filesize(const char *path,
                                         const struct search_opts *opts);
bool bx_search_entry_exceeds_max_filesize(struct bx_walk_entry *entry,
                                          const struct search_opts *opts);
bool bx_search_should_skip_special_input_mode(mode_t mode,
                                              const struct search_opts *opts);
bool bx_search_entry_should_skip_special_input(struct bx_walk_entry *entry,
                                               const struct search_opts *opts);
char *bx_search_display_path_for_output(const char *path,
                                        bool strip_dot_prefix,
                                        const struct search_opts *opts);
struct bx_search_output_ctx *bx_search_output_ctx_push(struct bx_search_output_ctx *ctx);
void bx_search_output_ctx_pop(struct bx_search_output_ctx *previous);
struct bx_matcher *bx_search_compile_matcher(const char *pattern,
                                             enum bx_search_personality personality,
                                             struct search_opts *opts,
                                             char **errmsg);
void bx_search_matcher_free(struct bx_matcher *m);
int bx_search_search_file(const char *filename,
                          const char *display_name_override,
                          const char *progname,
                          struct bx_matcher *m,
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
