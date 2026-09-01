#ifndef BX_SEARCH_BINARY_SCAN_H
#define BX_SEARCH_BINARY_SCAN_H

#include <stdio.h>

struct bx_matcher;
struct bx_record_stream;
struct bx_search_stats;
struct search_opts;

/*
 * Search the unread binary tail as newline/NUL-delimited segments.
 *
 * Returns 1 for a selected segment, 0 for no selected segment, and -1 on a
 * matcher, input, record-limit, or allocation error. Error details remain in
 * the matcher or record stream for applet policy to report.
 */
int bx_search_binary_scan_remaining(FILE *f,
                                    struct bx_matcher *m,
                                    struct search_opts *opts,
                                    struct bx_record_stream *record_stream,
                                    struct bx_search_stats *stats);

#endif
