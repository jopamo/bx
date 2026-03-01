#ifndef BX_SEARCH_RG_TRANSFORM_H
#define BX_SEARCH_RG_TRANSFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

struct search_opts;

enum bx_rg_transform_result {
    BX_RG_TRANSFORM_OK = 0,
    BX_RG_TRANSFORM_NO_MATCH = 1,
    BX_RG_TRANSFORM_ERROR = 2,
};

bool bx_rg_trace_enabled(const struct search_opts *opts);
void bx_rg_tracef(const struct search_opts *opts, const char *fmt, ...);

bool bx_rg_transform_needs_file_preload(const struct search_opts *opts,
                                        const char *filename);

bool bx_rg_transform_auto_encoding_needs_prefix(const struct search_opts *opts,
                                                const unsigned char *prefix,
                                                size_t nread);

bool bx_rg_transform_auto_encoding_needs_fd(const struct search_opts *opts,
                                            int fd_hint);

bool bx_rg_transform_maybe_needed(const struct search_opts *opts,
                                  const char *filename,
                                  bool use_stdin,
                                  int fd_hint);

enum bx_rg_transform_result bx_rg_load_transformed_input(
    const char *filename,
    const char *progname,
    const struct search_opts *opts,
    FILE *err_stream,
    unsigned char **output,
    size_t *output_len);

#endif
