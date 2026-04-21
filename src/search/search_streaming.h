#ifndef BX_SEARCH_SEARCH_STREAMING_H
#define BX_SEARCH_SEARCH_STREAMING_H

#include <stdbool.h>
#include <stdio.h>

struct bx_matcher;
struct bx_record_stream;
struct bx_search_stats;
struct search_opts;

bool bx_search_streaming_uses_line_buffered_stdin(const struct search_opts *opts,
                                                  bool use_stdin);
int bx_search_streaming_opened(FILE *f,
                               bool use_stdin,
                               const char *display_name,
                               const char *progname,
                               struct bx_matcher *m,
                               struct search_opts *opts,
                               int *match_count,
                               struct bx_record_stream *record_stream,
                               struct bx_search_stats *stats);
int bx_search_streaming_path(const char *filename,
                             const char *display_name,
                             const char *progname,
                             struct bx_matcher *m,
                             struct search_opts *opts,
                             int *match_count,
                             struct bx_record_stream *record_stream,
                             struct bx_search_stats *stats);

#endif
