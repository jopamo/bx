#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "applets.h"
#include "lib/cli_common.h"
#include "lib/mount_table.h"
#include "lib/option_scan.h"

static void print_help(FILE* out, const char* name) {
    fprintf(out, "Usage: %s [-q] PATH\nTest whether PATH is the root of a mount.\n", name);
}

int bx_mountpoint_main(int argc, char** argv) {
    int help = bx_cli_maybe_handle_help_or_version(argc, argv, "mountpoint", NULL, NULL, print_help);
    if (help >= 0)
        return help;
    static const struct bx_option_spec quiet_option = {'q', 'q', NULL, BX_OPTION_NO_VALUE};
    struct bx_option_cursor cursor;
    bx_option_cursor_init(&cursor, argv, 1, (size_t)argc, BX_OPTION_ORDERED, &quiet_option, 1);
    bool quiet = false;
    const char* path = NULL;
    for (;;) {
        struct bx_option_result result = bx_option_next(&cursor);
        if (result.event == BX_OPTION_END)
            break;
        if (result.event == BX_OPTION_MATCH)
            quiet = true;
        else if (result.event == BX_OPTION_OPERAND && path == NULL)
            path = result.token;
        else {
            print_help(stderr, "mountpoint");
            return 1;
        }
    }
    if (path == NULL) {
        print_help(stderr, "mountpoint");
        return 1;
    }
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
