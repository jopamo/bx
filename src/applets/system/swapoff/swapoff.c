#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/swap.h>

#include "applets.h"
#include "lib/cli_common.h"
#include "lib/swap_table.h"

static void usage(FILE* out, const char* name) {
    fprintf(out, "Usage: %s -a\nDisable every active swap area listed in /proc/swaps.\n", name);
}

int bx_swapoff_main(int argc, char** argv) {
    if (argc != 2) {
        usage(stderr, "swapoff");
        return 1;
    }
    int help = bx_cli_maybe_handle_help_or_version(argc, argv, "swapoff", NULL, NULL, usage);
    if (help >= 0)
        return help;
    if (strcmp(argv[1], "-a")) {
        usage(stderr, "swapoff");
        return 1;
    }
    struct bx_swap_table table;
    if (!bx_swap_table_load(&table)) {
        perror("swapoff: /proc/swaps");
        return 1;
    }
    int status = 0;
    size_t count = table.len;
    while (count) {
        const char* path = table.paths[--count];
        if (swapoff(path) < 0) {
            fprintf(stderr, "swapoff: %s: %s\n", path, strerror(errno));
            status = 1;
        }
    }
    bx_swap_table_free(&table);
    return status;
}
