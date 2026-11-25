#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>

#include "applets.h"
#include "lib/args_common.h"
#include "lib/cli_common.h"
#include "lib/option_scan.h"

static void print_help(FILE* out, const char* name) {
    fprintf(out,
            "Usage: %s [-x] [-n] FD\nLock an inherited file descriptor exclusively.\n"
            "  -n  fail instead of waiting for the lock\n",
            name);
}

int bx_flock_main(int argc, char** argv) {
    enum { OPT_HELP = 256, OPT_VERSION };
    static const struct bx_option_spec specs[] = {
        {'x', 'x', NULL, BX_OPTION_NO_VALUE},
        {'n', 'n', NULL, BX_OPTION_NO_VALUE},
        {OPT_HELP, 0, "help", BX_OPTION_NO_VALUE},
        {OPT_VERSION, 0, "version", BX_OPTION_NO_VALUE},
    };
    const char* name = bx_cli_progname(argc > 0 ? argv[0] : NULL, "flock");
    int operation = LOCK_EX;
    struct bx_option_cursor cursor;
    bx_option_cursor_init(&cursor, argv, 1, (size_t)argc, BX_OPTION_PREFIX,
                          specs, sizeof(specs) / sizeof(specs[0]));
    struct bx_option_result result;
    while ((result = bx_option_next(&cursor)).event != BX_OPTION_END) {
        if (result.event != BX_OPTION_MATCH) {
            print_help(stderr, name);
            return 2;
        }
        if (result.spec->id == 'n')
            operation |= LOCK_NB;
        else if (result.spec->id == OPT_HELP) {
            print_help(stdout, name);
            return 0;
        }
        else if (result.spec->id == OPT_VERSION) {
            bx_cli_print_version(name);
            return 0;
        }
    }
    int fd;
    if (result.index + 1 != (size_t)argc ||
        argv[result.index][0] < '0' || argv[result.index][0] > '9' ||
        !bx_args_parse_int_range(argv[result.index], 0, INT_MAX, &fd)) {
        print_help(stderr, name);
        return 2;
    }
    while (flock(fd, operation) < 0) {
        if (errno == EINTR)
            continue;
        if ((operation & LOCK_NB) && errno == EWOULDBLOCK)
            return 1;
        fprintf(stderr, "flock: fd %d: %s\n", fd, strerror(errno));
        return 2;
    }
    return 0;
}
