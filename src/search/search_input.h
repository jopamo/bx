#ifndef BX_SEARCH_SEARCH_INPUT_H
#define BX_SEARCH_SEARCH_INPUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>

struct bx_record_stream;
struct search_opts;

FILE *bx_search_input_open_stream(const char *filename,
                                  const char *progname,
                                  struct search_opts *opts,
                                  struct bx_record_stream *stream,
                                  bool *use_stdin_out);
ssize_t bx_search_input_read_record(FILE *f,
                                    struct bx_record_stream *stream,
                                    const struct search_opts *opts);
unsigned char *bx_search_input_read_stream_all(FILE *f, size_t *out_len);
bool bx_search_input_needs_early_transform_load(const char *filename,
                                                bool use_stdin,
                                                const struct search_opts *opts);
bool bx_search_input_opened_needs_auto_transform(FILE *f,
                                                 const struct search_opts *opts);
bool bx_search_input_is_binary_path(const char *path);

#endif
