#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include "lib/fd_ops.h"
#include "search_buffered.h"
#include "search_gzip.h"
#include "search_input.h"
#include "search_internal.h"
#include "search_plan.h"
#include "search_streaming.h"

enum bx_search_gzip_state {
    GZIP_PROBE,
    GZIP_PLAIN,
    GZIP_MEMBER,
    GZIP_BETWEEN_MEMBERS,
    GZIP_EOF,
};

struct bx_search_gzip_reader {
    z_stream z;
    unsigned char input[8192];
    int fd;
    int error;
    enum bx_search_gzip_state state;
    bool initialized;
};

static bool gzip_fill_input(struct bx_search_gzip_reader *reader) {
    z_stream *z = &reader->z;
    ssize_t n;

    if (z->avail_in != 0)
        memmove(reader->input, z->next_in, z->avail_in);
    z->next_in = reader->input;
    do {
        n = read(reader->fd, reader->input + z->avail_in,
                 sizeof(reader->input) - z->avail_in);
    } while (n < 0 && errno == EINTR);
    if (n > 0)
        z->avail_in += (unsigned)n;
    else if (n < 0)
        reader->error = errno;
    return n > 0;
}

static ssize_t gzip_read(void *cookie, char *buf, size_t size) {
    struct bx_search_gzip_reader *reader = cookie;
    z_stream *z = &reader->z;
    unsigned cap = size > INT_MAX ? INT_MAX : (unsigned)size;
    unsigned written = 0;

    while (written < cap && !reader->error && reader->state != GZIP_EOF) {
        if (reader->state == GZIP_PROBE || reader->state == GZIP_BETWEEN_MEMBERS) {
            /* Only a gzip signature starts a member. Otherwise the original
             * input is plain text, or this is ignorable trailing data. */
            while (z->avail_in == 0 || (z->avail_in == 1 && z->next_in[0] == 0x1f)) {
                if (written != 0)
                    return written;
                if (!gzip_fill_input(reader))
                    break;
            }
            if (reader->error)
                break;
            if (z->avail_in < 2 || z->next_in[0] != 0x1f || z->next_in[1] != 0x8b) {
                reader->state = reader->state == GZIP_PROBE && z->avail_in != 0
                    ? GZIP_PLAIN : GZIP_EOF;
                continue;
            }
            int rc = reader->initialized ? inflateReset(z) : inflateInit2(z, MAX_WBITS + 16);
            if (rc != Z_OK) {
                reader->error = rc == Z_MEM_ERROR ? ENOMEM : EIO;
                break;
            }
            reader->initialized = true;
            reader->state = GZIP_MEMBER;
        }

        if (reader->state == GZIP_PLAIN) {
            if (z->avail_in == 0 && !gzip_fill_input(reader)) {
                reader->state = GZIP_EOF;
                break;
            }
            unsigned take = z->avail_in < cap ? z->avail_in : cap;
            memcpy(buf, z->next_in, take);
            z->next_in += take;
            z->avail_in -= take;
            return take;
        }

        unsigned available = z->avail_in;
        z->next_out = (unsigned char *)buf + written;
        z->avail_out = cap - written;
        int rc = inflate(z, Z_NO_FLUSH);
        unsigned produced = cap - written - z->avail_out;
        written += produced;
        if (rc == Z_STREAM_END) {
            reader->state = GZIP_BETWEEN_MEMBERS;
        } else if (rc != Z_OK && rc != Z_BUF_ERROR) {
            reader->error = rc == Z_MEM_ERROR ? ENOMEM : EBADMSG;
        } else if (produced == 0 && z->avail_in == available) {
            if (z->avail_in != 0) {
                reader->error = EBADMSG;
                break;
            }
            /* A pipe may still be open. Publish decoded bytes before waiting
             * for more input, including when the gzip trailer is not here yet. */
            if (written != 0)
                return written;
            if (!gzip_fill_input(reader) && !reader->error)
                reader->error = EBADMSG;
        }
    }
    /* inflate can produce data and detect a bad trailer in the same call.
     * Return those bytes first, then report the error (even after -q or -m). */
    if (written != 0 || !reader->error)
        return written;
    errno = reader->error;
    return -1;
}

static int gzip_open_path(const char *name, const struct search_opts *opts, bool *skip) {
    if (opts->directory_mode == BX_GREP_DIR_SKIP || opts->device_mode == BX_GREP_DEVICE_SKIP) {
        struct stat st;

        if (stat(name, &st) == 0 &&
            ((S_ISDIR(st.st_mode) && opts->directory_mode == BX_GREP_DIR_SKIP) ||
             bx_search_should_skip_special_input_mode(st.st_mode, opts))) {
            *skip = true;
            return -1;
        }
    }
    /* -m0 checks that the operand opens, but never reads from it. */
    int flags = opts->max_count_set && opts->max_count == 0 ? O_NONBLOCK : 0;
    return bx_search_input_open_fd_flags(name, flags);
}

static int gzip_open_operand(const char *name, const struct search_opts *opts, bool *skip) {
    int fd = gzip_open_path(name, opts, skip);

    if (fd < 0 && !*skip && errno == ENOENT) {
        size_t len = strlen(name);
        if (len != 0 && (len < 3 || strcmp(name + len - 3, ".gz") != 0)) {
            char *path;
            if (asprintf(&path, "%s.gz", name) < 0)
                return -1;
            fd = gzip_open_path(path, opts, skip);
            int error = errno;
            free(path);
            errno = error;
        }
    }
    return fd;
}

static bool gzip_output_aliases_input(const struct stat *input, const struct search_opts *opts) {
    struct stat output;

    /* Only an unbounded record-printing search can feed itself indefinitely.
     * Compare opened objects so hard links, symlinks and stdin are covered. */
    return S_ISREG(input->st_mode) && !opts->max_count_set && !opts->quiet &&
        !opts->count_only && !opts->files_with_matches && !opts->files_without_match &&
        fstat(STDOUT_FILENO, &output) == 0 && S_ISREG(output.st_mode) &&
        input->st_dev == output.st_dev && input->st_ino == output.st_ino;
}

int bx_search_gzip_file(const char *filename, const char *progname,
                        struct bx_matcher *matcher, struct search_opts *opts,
                        struct bx_record_stream *stream, struct bx_search_stats *stats) {
    bool use_stdin = !filename || strcmp(filename, "-") == 0;
    const char *name = use_stdin ? (opts->label ? opts->label : "(standard input)") : filename;
    bool skip = false;
    int fd = use_stdin ? bx_fd_dup_cloexec(STDIN_FILENO)
                       : gzip_open_operand(filename, opts, &skip);
    if (skip)
        return 1;
    if (fd < 0) {
        bx_search_report_path_error(progname, name, errno, opts);
        return 2;
    }
    if (opts->max_count_set && opts->max_count == 0) {
        close(fd);
        if (opts->files_without_match && !opts->quiet)
            bx_search_print_path_record(name, opts);
        return 1;
    }

    struct stat input;
    if (fstat(fd, &input) < 0) {
        int error = errno;
        close(fd);
        bx_search_report_path_error(progname, name, error, opts);
        return 2;
    }
    if (gzip_output_aliases_input(&input, opts)) {
        close(fd);
        if (!opts->suppress_errors)
            fprintf(bx_search_error_output_stream(), "%s: %s: input file is also the output\n",
                    progname, name);
        return 2;
    }

    /* Keep ownership of the wrapped input and check its state after
     * the search kernel returns, including when the kernel stops early. */
    struct bx_search_gzip_reader reader = { .fd = fd };
    FILE *f = fopencookie(&reader, "r", (cookie_io_functions_t){ .read = gzip_read });
    if (!f) {
        int error = errno;
        close(fd);
        bx_search_report_path_error(progname, name, error, opts);
        return 2;
    }
    bx_record_stream_prepare_file(f, stream);
    /* On a live stream, fread would wait for a full chunk even after the
     * last record the search needs. Keep bulk reads for ordinary files. */
    stream->live_records = opts->line_buffered ||
        (!S_ISREG(input.st_mode) && (opts->quiet || opts->files_with_matches ||
         opts->files_without_match || opts->max_count_set || opts->binary_without_match));
    bx_search_output_set_offset_width(0);
    bool binary_sensitive = bx_search_plan_plain_output_needs_binary_sensitive_path(opts);
    bool buffered = bx_search_plan_needs_line_buffering(opts)
        || binary_sensitive;
    int status = 1;
    if (binary_sensitive) {
        /* Check an available decoded chunk before publishing any matches.
         * Unlike fread, the cookie read need not wait to fill the buffer. */
        unsigned char prefix[8192];
        ssize_t n = gzip_read(&reader, (char *)prefix, sizeof(prefix));
        if (n < 0)
            goto done;
        if (n > 0 && memchr(prefix, '\0', (size_t)n)) {
            if (opts->binary_without_match) {
                if (!reader.error)
                    status = bx_search_binary_without_match(name, opts, NULL, stats);
                goto done;
            }
            stream->binary_prefix = true;
        }
        if (!bx_record_stream_seed(stream, prefix, (size_t)n)) {
            bx_search_report_path_error(progname, name, ENOMEM, opts);
            status = 2;
            goto done;
        }
    }
    status = buffered
        ? bx_search_buffered_opened(f, use_stdin, name, progname, matcher, opts, NULL, stream, stats)
        : bx_search_streaming_opened(f, use_stdin, name, progname, matcher, opts, NULL, stream, stats);
    if (!use_stdin)
        f = NULL; /* The kernel closes named files. */
done:
    if (f)
        fclose(f);
    if (reader.initialized)
        inflateEnd(&reader.z);
    int close_error = close(fd) == 0 ? 0 : errno;
    if (status != 2 && (reader.error || close_error)) {
        bx_search_report_path_error(progname, name, reader.error ? reader.error : close_error, opts);
        status = 2;
    }
    return status;
}
