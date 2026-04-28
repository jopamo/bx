#ifndef BX_SEARCH_SEARCH_RAW_PRESENCE_H
#define BX_SEARCH_SEARCH_RAW_PRESENCE_H

#include <stdbool.h>
#include <stdio.h>

struct bx_matcher;
struct bx_search_scanner;
struct bx_search_stats;
struct search_opts;

int bx_search_raw_presence_opened(FILE *f,
                                  bool use_stdin,
                                  const char *filename,
                                  const char *display_name,
                                  const char *progname,
                                  struct bx_matcher *m,
                                  struct search_opts *opts,
                                  int *match_count,
                                  struct bx_search_scanner *scanner,
                                  struct bx_search_stats *stats);

#endif
