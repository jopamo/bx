#include <limits.h>
#include <stdio.h>
#include <unistd.h>

#include "args_common.h"
#include "thread_count.h"

bool bx_thread_count_parse(const char *progname,
                           const char *optname,
                           const char *text,
                           int *out) {
    if (!text || !*text || !out) {
        fprintf(stderr, "%s: invalid argument for %s: %s\n",
                progname, optname, text ? text : "(null)");
        return false;
    }

    if (!bx_args_parse_int_range(text, 0, INT_MAX, out)) {
        fprintf(stderr, "%s: invalid argument for %s: %s\n",
                progname, optname, text);
        return false;
    }

    return true;
}

size_t bx_thread_count_resolve(int requested) {
    if (requested > 0)
        return (size_t)requested;

    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    if (cpus <= 0)
        return 1u;
    return (size_t)cpus;
}
