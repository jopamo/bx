#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>

#include "applets.h"
#include "lib/args_common.h"
#include "lib/child_runner.h"
#include "lib/cli_common.h"

static void usage(FILE* stream) {
    fputs(
        "Usage: unshare -m [--propagation private] COMMAND [ARG...]\n"
        "Run COMMAND in a recursively private mount namespace.\n",
        stream);
}

int bx_unshare_main(int argc, char** argv) {
    static const struct option options[] = {
        {"mount", no_argument, NULL, 'm'}, {"propagation", required_argument, NULL, 'p'}, {"help", no_argument, NULL, 'h'}, {"version", no_argument, NULL, 'V'}, {0}};
    bool mount_namespace = false;
    int opt;
    bx_args_getopt_reset();
    while ((opt = bx_args_getopt_long(argc, argv, "+m", options, NULL)) != -1) {
        switch (opt) {
            case 'm':
                mount_namespace = true;
                break;
            case 'p':
                if (!strcmp(optarg, "private"))
                    break;
                fprintf(stderr, "unshare: unsupported propagation: %s\n", optarg);
                return 1;
            case 'h':
                usage(stdout);
                return 0;
            case 'V':
                bx_cli_print_version("unshare");
                return 0;
            default:
                usage(stderr);
                return 1;
        }
    }
    if (!mount_namespace || optind == argc) {
        usage(stderr);
        return 1;
    }
    if (unshare(CLONE_NEWNS) < 0) {
        perror("unshare: mount namespace");
        return 1;
    }
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0) {
        perror("unshare: make / recursively private");
        return 1;
    }
    int error = bx_child_exec_argv(argv + optind);
    fprintf(stderr, "unshare: %s: %s\n", argv[optind], strerror(error));
    return error == ENOENT ? 127 : 126;
}
