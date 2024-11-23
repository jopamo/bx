#ifndef BX_DIAG_H
#define BX_DIAG_H

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>

struct bx_diag_ctx {
    const char* progname;
    int exit_status;
    bool verbose;
    bool debug;
};

static inline void bx_vdiag(const struct bx_diag_ctx* ctx, const char* fmt, va_list ap) {
    fprintf(stderr, "%s: ", ctx->progname);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
}

static inline void bx_diag(struct bx_diag_ctx* ctx, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    bx_vdiag(ctx, fmt, ap);
    va_end(ap);
    ctx->exit_status = 1;
}

static inline void bx_perror_path(struct bx_diag_ctx* ctx, const char* path) {
    fprintf(stderr, "%s: %s: %s\n", ctx->progname, path, strerror(errno));
    ctx->exit_status = 1;
}

static inline void bx_vinfo(const struct bx_diag_ctx* ctx, const char* fmt, va_list ap) {
    if (ctx->verbose) {
        vprintf(fmt, ap);
        fputc('\n', stdout);
    }
}

static inline void bx_info(const struct bx_diag_ctx* ctx, const char* fmt, ...) {
    if (ctx->verbose) {
        va_list ap;
        va_start(ap, fmt);
        bx_vinfo(ctx, fmt, ap);
        va_end(ap);
    }
}

static inline void bx_vdebug(const struct bx_diag_ctx* ctx, const char* fmt, va_list ap) {
    if (ctx->debug) {
        vprintf(fmt, ap);
        fputc('\n', stdout);
    }
}

static inline void bx_debug(const struct bx_diag_ctx* ctx, const char* fmt, ...) {
    if (ctx->debug) {
        va_list ap;
        va_start(ap, fmt);
        bx_vdebug(ctx, fmt, ap);
        va_end(ap);
    }
}

/* Legacy helpers for now */
static inline void bx_err(const char* fmt, ...) {
    va_list ap;
    fprintf(stderr, "bx: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

static inline void bx_fatal(int code, const char* fmt, ...) {
    va_list ap;
    fprintf(stderr, "bx: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    exit(code);
}

#define bx_perror(msg) fprintf(stderr, "bx: %s: %s\n", msg, strerror(errno))

#define bx_pfatal(code, msg) \
    do {                     \
        bx_perror(msg);      \
        exit(code);          \
    } while (0)

#endif /* BX_DIAG_H */
