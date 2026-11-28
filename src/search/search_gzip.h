#ifndef BX_SEARCH_GZIP_H
#define BX_SEARCH_GZIP_H

struct bx_matcher;
struct bx_record_stream;
struct bx_search_stats;
struct search_opts;

int bx_search_gzip_file(const char *filename, const char *progname,
                        struct bx_matcher *matcher, struct search_opts *opts,
                        struct bx_record_stream *stream, struct bx_search_stats *stats);

#endif
