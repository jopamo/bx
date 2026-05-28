#ifndef BX_SEARCH_SEARCH_RAW_PRESENCE_H
#define BX_SEARCH_SEARCH_RAW_PRESENCE_H

#include <stdbool.h>
#include <stdio.h>

struct bx_lit_plan;
struct bx_search_scanner;
struct bx_search_stats;
struct search_opts;

#define BX_SEARCH_RAW_PRESENCE_CHUNK_CAP 65536u

int bx_search_raw_presence_opened(FILE *f,
                                  bool use_stdin,
                                  const char *filename,
                                  const char *display_name,
                                  const char *progname,
                                  const struct bx_lit_plan *absence_plan,
                                  struct search_opts *opts,
                                  int *match_count,
                                  struct bx_search_scanner *scanner,
                                  struct bx_search_stats *stats);

#endif
