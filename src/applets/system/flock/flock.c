#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>

#include "applets.h"
#include "lib/args_common.h"
#include "lib/cli_common.h"

static void print_help(FILE* out, const char* name) {
    fprintf(out,
            "Usage: %s [-x] [-n] FD\nLock an inherited file descriptor exclusively.\n"
            "  -n  fail instead of waiting for the lock\n",
            name);
}

int bx_flock_main(int argc, char** argv) {
    const int help = bx_cli_maybe_handle_help_or_version(argc, argv, "flock", NULL, NULL, print_help);
    if (help >= 0)
        return help;

    int operation = LOCK_EX;
    bx_args_getopt_reset();
    int opt;
    while ((opt = bx_args_getopt_long(argc, argv, "+xn", NULL, NULL)) != -1) {
        if (opt == 'n')
            operation |= LOCK_NB;
        else if (opt != 'x') {
            print_help(stderr, "flock");
            return 2;
        }
    }
    int fd;
    if (argc - optind != 1 || argv[optind][0] < '0' || argv[optind][0] > '9' || !bx_args_parse_int_range(argv[optind], 0, INT_MAX, &fd)) {
        print_help(stderr, "flock");
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
