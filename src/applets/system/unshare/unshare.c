#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>

#include "applets.h"
#include "lib/child_runner.h"
#include "lib/cli_common.h"
#include "lib/option_scan.h"

static void usage(FILE* stream) {
    fputs(
        "Usage: unshare -m [--propagation private] COMMAND [ARG...]\n"
        "Run COMMAND in a recursively private mount namespace.\n",
        stream);
}

int bx_unshare_main(int argc, char** argv) {
    static const struct bx_option_spec options[] = {
        {'m', 'm', "mount", BX_OPTION_NO_VALUE},
        {'p', 0, "propagation", BX_OPTION_REQUIRED_VALUE},
        {'h', 0, "help", BX_OPTION_NO_VALUE},
        {'V', 0, "version", BX_OPTION_NO_VALUE},
    };
    bool mount_namespace = false;
    size_t first;
    struct bx_option_cursor cursor;
    bx_option_cursor_init(&cursor, argv, 1, (size_t)argc, BX_OPTION_PREFIX | BX_OPTION_ABBREVIATE,
                          options, sizeof(options) / sizeof(options[0]));
    for (;;) {
        struct bx_option_result result = bx_option_next(&cursor);
        if (result.event == BX_OPTION_END) {
            first = result.index;
            break;
        }
        if (result.event != BX_OPTION_MATCH) {
            usage(stderr);
            return 1;
        }
        switch (result.spec->id) {
            case 'm':
                mount_namespace = true;
                break;
            case 'p':
                if (!strcmp(result.value, "private"))
                    break;
                fprintf(stderr, "unshare: unsupported propagation: %s\n", result.value);
                return 1;
            case 'h':
                usage(stdout);
                return 0;
            case 'V':
                bx_cli_print_version("unshare");
                return 0;
        }
    }
    if (!mount_namespace || first == (size_t)argc || !argv[first][0]) {
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
    int error = bx_child_exec_argv(argv + first);
    fprintf(stderr, "unshare: %s: %s\n", argv[first], strerror(error));
    return error == ENOENT ? 127 : 126;
}
