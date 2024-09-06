#ifndef BX_DIAG_H
#define BX_DIAG_H

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <stdarg.h>

/*
 * bx_err: Print a formatted error message to stderr.
 * Prefixes with "bx: ".
 */
static inline void bx_err(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "bx: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

/*
 * bx_fatal: Print a formatted error message and exit with given code.
 * Prefixes with "bx: ".
 */
static inline void bx_fatal(int code, const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "bx: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    exit(code);
}

/*
 * bx_perror: Print a message and the error string for errno.
 * Prefixes with "bx: ".
 */
#define bx_perror(msg) fprintf(stderr, "bx: %s: %s\n", msg, strerror(errno))

/*
 * bx_pfatal: bx_perror followed by exit.
 */
#define bx_pfatal(code, msg) do { \
    bx_perror(msg); \
    exit(code); \
} while(0)

#endif /* BX_DIAG_H */
