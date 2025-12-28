#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "thread_count.h"

bool bx_thread_count_parse(const char *progname,
                           const char *optname,
                           const char *text,
                           int *out) {
    char *end = NULL;
    long value;

    if (!text || !*text || !out) {
        fprintf(stderr, "%s: invalid argument for %s: %s\n",
                progname, optname, text ? text : "(null)");
        return false;
    }

    value = strtol(text, &end, 10);
    if (!end || *end != '\0' || value < 0 || value > INT_MAX) {
        fprintf(stderr, "%s: invalid argument for %s: %s\n",
                progname, optname, text);
        return false;
    }

    *out = (int)value;
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
