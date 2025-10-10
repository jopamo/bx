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

bool xargs_read_items(FILE *input, const char *progname, struct xargs_opts *opts,
                      struct xargs_items *items);
void xargs_items_free(struct xargs_items *items);

#endif
