#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "applets.h"
#include "lib/args_common.h"
#include "lib/cli_common.h"
#include "lib/mount_table.h"

static void print_help(FILE* out, const char* name) {
    fprintf(out, "Usage: %s [-q] PATH\nTest whether PATH is the root of a mount.\n", name);
}

int bx_mountpoint_main(int argc, char** argv) {
    int help = bx_cli_maybe_handle_help_or_version(argc, argv, "mountpoint", NULL, NULL, print_help);
    if (help >= 0)
        return help;
    bool quiet = false;
    bx_args_getopt_reset();
    int next_operand = 1;
    int opt;
    while ((opt = bx_args_getopt_long_interspersed(argc, argv, "-q", NULL, &next_operand)) != -1) {
        if (opt != 'q') {
            print_help(stderr, "mountpoint");
            return 1;
        }
        quiet = true;
    }
    if (argc - optind != 1) {
        print_help(stderr, "mountpoint");
        return 1;
    }
    const char* path = argv[optind];
    int mounted = bx_mount_is_mountpoint(path);
    if (mounted < 0) {
        if (!quiet)
            fprintf(stderr, "mountpoint: %s: %s\n", path, strerror(errno));
        return 1;
    }
    if (!quiet)
        printf("%s is %sa mountpoint\n", path, mounted ? "" : "not ");
    return mounted ? 0 : 32;
}
