#ifndef BX_SEARCH_SEARCH_MULTILINE_H
#define BX_SEARCH_SEARCH_MULTILINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

struct bx_matcher;
struct bx_search_stats;
struct search_opts;

int bx_search_multiline_buffer(unsigned char *buf,
                               size_t len,
                               const char *display_name,
                               struct bx_matcher *m,
                               struct search_opts *opts,
                               int *match_count,
                               struct bx_search_stats *stats);
int bx_search_multiline_opened(FILE *f,
                               bool use_stdin,
                               const char *display_name,
                               const char *progname,
                               struct bx_matcher *m,
                               struct search_opts *opts,
                               int *match_count,
                               struct bx_search_stats *stats);
int bx_search_multiline_path(const char *filename,
                             const char *display_name,
                             const char *progname,
                             struct bx_matcher *m,
                             struct search_opts *opts,
                             int *match_count,
                             struct bx_search_stats *stats);

#endif
