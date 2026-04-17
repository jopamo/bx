#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "dev_counters.h"
#include "record_stream.h"
#include "rg_transform.h"
#include "search_input.h"
#include "search_internal.h"

static int bx_search_input_open_readonly(const char *filename,
                                         const struct search_opts *opts) {
    int flags = O_RDONLY;

#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOATIME
    /*
     * Metadata-sorted accessed-time searches must not perturb atime while
     * reading candidate files, or a second pass can collapse to path-order
     * ties after the first pass dirties every file to "now".
     */
    if (opts && opts->sort_key == BX_SEARCH_SORT_ACCESSED)
        flags |= O_NOATIME;
#endif

    int fd = open(filename, flags);

#ifdef O_NOATIME
    if (fd < 0 && (flags & O_NOATIME) != 0 &&
        (errno == EPERM || errno == EINVAL || errno == EOPNOTSUPP || errno == ENOTSUP)) {
        flags &= ~O_NOATIME;
        fd = open(filename, flags);
    }
#endif

    return fd;
}

FILE *bx_search_input_fopen(const char *filename,
                            const struct search_opts *opts) {
    if (!filename) {
        errno = EINVAL;
        return NULL;
    }

    int fd = bx_search_input_open_readonly(filename, opts);
    if (fd < 0)
        return NULL;

    FILE *f = fdopen(fd, "r");
    if (f)
        return f;

    int saved_errno = errno;
    close(fd);
    errno = saved_errno;
    return NULL;
}

FILE *bx_search_input_open_stream(const char *filename,
                                  const char *progname,
                                  struct search_opts *opts,
                                  struct bx_record_stream *stream,
                                  bool *use_stdin_out) {
    bool use_stdin = (!filename || strcmp(filename, "-") == 0);

    if (use_stdin) {
        if (use_stdin_out)
            *use_stdin_out = true;
        return stdin;
    }

    FILE *f = bx_search_input_fopen(filename, opts);

    if (!f) {
        bx_search_report_path_error(progname, filename, errno, opts);
        return NULL;
    }
    bx_search_dev_counters_note_file_opened();

    bx_record_stream_prepare_file(f, stream);
    if (use_stdin_out)
        *use_stdin_out = false;
    return f;
}

ssize_t bx_search_input_read_record(FILE *f,
                                    struct bx_record_stream *stream,
                                    const struct search_opts *opts) {
    return bx_record_stream_read(f, stream, opts->null_data ? '\0' : '\n');
}

unsigned char *bx_search_input_read_stream_all(FILE *f, size_t *out_len) {
    size_t cap = 4096u;
    size_t len = 0u;
    unsigned char *buf = malloc(cap + 1u);

    if (!buf)
        return NULL;

    for (;;) {
        if (len == cap) {
            size_t new_cap = cap * 2u;
            unsigned char *tmp = realloc(buf, new_cap + 1u);

            if (!tmp) {
                free(buf);
                return NULL;
            }
            buf = tmp;
            cap = new_cap;
        }

        size_t nread = fread(buf + len, 1u, cap - len, f);
        len += nread;
        bx_search_dev_counters_note_bytes_read(nread);
        if (nread == 0u)
            break;
    }

    if (ferror(f)) {
        free(buf);
        return NULL;
    }

    buf[len] = '\0';
    if (out_len)
        *out_len = len;
    return buf;
}

bool bx_search_input_needs_early_transform_load(const char *filename,
                                                bool use_stdin,
                                                const struct search_opts *opts) {
    if (!opts)
        return false;
    if (use_stdin)
        return bx_rg_transform_maybe_needed(opts, filename, true, fileno(stdin));
    return bx_rg_transform_needs_file_preload(opts, filename);
}

bool bx_search_input_opened_needs_auto_transform(FILE *f,
                                                 const struct search_opts *opts) {
    if (!f || !opts)
        return false;
    return bx_rg_transform_auto_encoding_needs_fd(opts, fileno(f));
}

bool bx_search_input_is_binary_path(const char *path,
                                    const struct search_opts *opts) {
    FILE *f = bx_search_input_fopen(path, opts);

    if (!f)
        return false;
    bx_search_dev_counters_note_file_opened();

    unsigned char buf[1024];
    size_t n = fread(buf, 1u, sizeof(buf), f);
    bx_search_dev_counters_note_bytes_read(n);
    fclose(f);
    if (n == 0u)
        return false;
    for (size_t i = 0; i < n; i++) {
        if (buf[i] == 0u)
            return true;
    }
    return false;
}
