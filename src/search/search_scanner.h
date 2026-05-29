#ifndef BX_SEARCH_SEARCH_SCANNER_H
#define BX_SEARCH_SEARCH_SCANNER_H

#include <stdbool.h>
#include <stdio.h>

struct bx_matcher;
struct bx_search_scanner;
struct bx_search_stats;
struct search_opts;

bool bx_search_scanner_stream_is_eligible(FILE *f);
bool bx_search_scanner_can_use(const struct bx_matcher *m,
                               const struct search_opts *opts,
                               bool use_stdin);
bool bx_search_scanner_can_raw_shortcut_file_presence(const struct bx_matcher *m,
                                                      const struct search_opts *opts);
bool bx_search_scanner_matcher_is_exact_literal_candidate(const struct bx_matcher *m,
                                                          const struct search_opts *opts);
int bx_search_scanner_opened(FILE *f,
                             bool use_stdin,
                             const char *display_name,
                             const char *progname,
                             struct bx_matcher *m,
                             struct search_opts *opts,
                             int *match_count,
                             struct bx_search_scanner *scanner,
                             struct bx_search_stats *stats,
                             bool candidate_triggered_scanner_entry);

#endif
