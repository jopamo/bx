#ifndef BX_APPLETS_BASE_XARGS_INPUT_H
#define BX_APPLETS_BASE_XARGS_INPUT_H

#include <stdbool.h>
#include <stdio.h>

#include "xargs_parse.h"

struct xargs_items {
    char **v;
    int *line_groups;
    int count;
    int cap;
};

typedef bool (*xargs_item_sink_fn)(const char *text, int line_group, void *user);

bool xargs_read_items(FILE *input, const char *progname, struct xargs_opts *opts,
                      struct xargs_items *items);
bool xargs_read_stream(FILE *input, const char *progname, struct xargs_opts *opts,
                       xargs_item_sink_fn sink, void *user);
void xargs_items_free(struct xargs_items *items);

#endif
