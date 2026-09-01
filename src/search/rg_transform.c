#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "dev_counters.h"
#include "lib/child_runner.h"
#include "lib/fd_ops.h"
#include "lib/path_ops.h"
#include "options.h"
#include "rg_text.h"
#include "rg_transform.h"
#include "search_input.h"

static bool bx_rg_pre_glob_matches(const struct search_opts *opts, const char *filename) {
    bool selected = true;
    const char *basename;

    if (!opts || !filename || opts->num_pre_globs <= 0)
        return true;
    basename = bx_path_basename_ptr(filename);

    for (int i = 0; i < opts->num_pre_globs; i++) {
        if (opts->pre_globs[i] && opts->pre_globs[i][0] != '!') {
            selected = false;
            break;
        }
    }
    for (int i = 0; i < opts->num_pre_globs; i++) {
        const char *glob = opts->pre_globs[i];
        int flags = opts->glob_case_insensitive ? FNM_CASEFOLD : 0;
        bool negated;
        const char *pattern;
        bool matched;

        if (!glob || glob[0] == '\0')
            continue;
        negated = glob[0] == '!';
        pattern = negated ? glob + 1 : glob;
        if (pattern[0] == '\0')
            continue;
        matched = fnmatch(pattern, filename, flags) == 0;
        if (!matched && basename != filename)
            matched = fnmatch(pattern, basename, flags) == 0;
        if (matched)
            selected = !negated;
    }
    return selected;
}

bool bx_rg_trace_enabled(const struct search_opts *opts) {
    return opts && opts->trace;
}

void bx_rg_tracef(const struct search_opts *opts, const char *fmt, ...) {
    va_list ap;
    if (!bx_rg_trace_enabled(opts) || !fmt)
        return;
    fputs("rg: TRACE|bx::rg|", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static const char *bx_rg_search_zip_program(const char *filename, const char *const **argv_out) {
    static const char *gzip_argv[] = {"gzip", "-cd", NULL, NULL};
    static const char *bzip_argv[] = {"bzip2", "-cd", NULL, NULL};
    static const char *xz_argv[] = {"xz", "-cd", NULL, NULL};
    static const char *lz4_argv[] = {"lz4", "-cd", NULL, NULL};
    static const char *brotli_argv[] = {"brotli", "-cd", NULL, NULL};
    static const char *zstd_argv[] = {"zstd", "-cdq", NULL, NULL};
    const char *ext = bx_path_extension_ptr(filename);

    if (!ext || !argv_out)
        return NULL;
    if (strcmp(ext, ".gz") == 0) {
        gzip_argv[2] = filename;
        *argv_out = gzip_argv;
        return "gzip";
    }
    if (strcmp(ext, ".bz2") == 0) {
        bzip_argv[2] = filename;
        *argv_out = bzip_argv;
        return "bzip2";
    }
    if (strcmp(ext, ".xz") == 0 || strcmp(ext, ".lzma") == 0) {
        xz_argv[2] = filename;
        *argv_out = xz_argv;
        return "xz";
    }
    if (strcmp(ext, ".lz4") == 0) {
        lz4_argv[2] = filename;
        *argv_out = lz4_argv;
        return "lz4";
    }
    if (strcmp(ext, ".br") == 0) {
        brotli_argv[2] = filename;
        *argv_out = brotli_argv;
        return "brotli";
    }
    if (strcmp(ext, ".zst") == 0 || strcmp(ext, ".zstd") == 0) {
        zstd_argv[2] = filename;
        *argv_out = zstd_argv;
        return "zstd";
    }
    return NULL;
}

bool bx_rg_transform_prefix_has_utf8_bom(const unsigned char *prefix,
                                         size_t nread) {
    if (!prefix || nread < 2u)
        return false;
    return nread >= 3u &&
           prefix[0] == 0xEFu && prefix[1] == 0xBBu && prefix[2] == 0xBFu;
}

static bool bx_rg_transform_prefix_has_utf16_bom(const unsigned char *prefix,
                                                 size_t nread) {
    if (!prefix || nread < 2u)
        return false;
    return (prefix[0] == 0xFFu && prefix[1] == 0xFEu) ||
           (prefix[0] == 0xFEu && prefix[1] == 0xFFu);
}

bool bx_rg_transform_prefix_needs_decode(const unsigned char *prefix,
                                         size_t nread) {
    bx_search_dev_counters_note_transform_prefix_check();
    return bx_rg_transform_prefix_has_utf8_bom(prefix, nread) ||
           bx_rg_transform_prefix_has_utf16_bom(prefix, nread);
}

bool bx_rg_transform_needs_file_preload(const struct search_opts *opts,
                                        const char *filename) {
    if (!opts)
        return false;
    if (filename && opts->pre_command && *opts->pre_command &&
        bx_rg_pre_glob_matches(opts, filename)) {
        return true;
    }
    if (filename && opts->search_zip) {
        const char *const *argv = NULL;
        if (bx_rg_search_zip_program(filename, &argv) != NULL)
            return true;
    }
    return opts->encoding_mode == BX_RG_ENCODING_EXPLICIT;
}

bool bx_rg_transform_uses_external_source(const struct search_opts *opts,
                                          const char *filename) {
    const char *const *argv = NULL;

    if (!opts || !filename || strcmp(filename, "-") == 0)
        return false;
    if (opts->pre_command && *opts->pre_command &&
        bx_rg_pre_glob_matches(opts, filename)) {
        return true;
    }
    return opts->search_zip &&
           bx_rg_search_zip_program(filename, &argv) != NULL;
}

bool bx_rg_transform_auto_encoding_needs_prefix(const struct search_opts *opts,
                                                const unsigned char *prefix,
                                                size_t nread) {
    if (!opts || opts->encoding_mode != BX_RG_ENCODING_AUTO)
        return false;
    return bx_rg_transform_prefix_needs_decode(prefix, nread);
}

enum bx_rg_auto_encoding_probe bx_rg_transform_auto_encoding_probe_fd(
    const struct search_opts *opts,
    int fd_hint) {
    unsigned char prefix[3] = {0};
    ssize_t nread;

    if (!opts || opts->encoding_mode != BX_RG_ENCODING_AUTO || fd_hint < 0)
        return BX_RG_AUTO_ENCODING_PASSTHROUGH;

    nread = pread(fd_hint, prefix, sizeof(prefix), 0);
    if (nread < 0)
        return BX_RG_AUTO_ENCODING_STREAM;
    bx_search_dev_counters_note_content_pread((size_t)nread);
    bx_search_dev_counters_note_prefix_pread((size_t)nread);
    bx_search_dev_counters_note_prefix_bytes_rescanned((size_t)nread);
    return bx_rg_transform_auto_encoding_needs_prefix(opts, prefix, (size_t)nread)
        ? BX_RG_AUTO_ENCODING_DECODE
        : BX_RG_AUTO_ENCODING_PASSTHROUGH;
}

struct bx_rg_capture_reap {
    bool called;
    int status;
    bool exec_failed;
    int exec_errno;
};

static void bx_rg_capture_reap_cb(pid_t pid, int status, bool exec_failed, int exec_errno, void *user) {
    (void)pid;
    struct bx_rg_capture_reap *state = user;
    state->called = true;
    state->status = status;
    state->exec_failed = exec_failed;
    state->exec_errno = exec_errno;
}

static bool bx_rg_run_capture(const char *const *argv,
                              bool capture_stderr,
                              int stdin_fd,
                              unsigned char **stdout_buf,
                              size_t *stdout_len,
                              char **stderr_buf,
                              int *status_out,
                              bool *exec_failed,
                              int *exec_errno) {
    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    struct bx_child child[1] = {0};
    int running = 0;
    struct bx_child_runner_opts runner_opts = bx_child_runner_opts_default();
    struct bx_rg_capture_reap reap = {0};
    FILE *out_stream = NULL;
    FILE *err_stream = NULL;
    unsigned char *raw_stdout = NULL;
    size_t raw_stdout_len = 0u;
    unsigned char *raw_stderr = NULL;
    size_t raw_stderr_len = 0u;

    if (stdout_buf)
        *stdout_buf = NULL;
    if (stdout_len)
        *stdout_len = 0u;
    if (stderr_buf)
        *stderr_buf = NULL;
    if (status_out)
        *status_out = 0;
    if (exec_failed)
        *exec_failed = false;
    if (exec_errno)
        *exec_errno = 0;

    if (bx_fd_pipe_cloexec(out_pipe) != 0)
        return false;
    if (capture_stderr && bx_fd_pipe_cloexec(err_pipe) != 0) {
        close(out_pipe[0]);
        close(out_pipe[1]);
        return false;
    }

    if (stdin_fd >= 0) {
        runner_opts.use_stdin_fd = true;
        runner_opts.stdin_fd = stdin_fd;
    }
    runner_opts.use_stdout_fd = true;
    runner_opts.stdout_fd = out_pipe[1];
    if (capture_stderr) {
        runner_opts.use_stderr_fd = true;
        runner_opts.stderr_fd = err_pipe[1];
    }

    if (bx_child_spawn_const_argv("rg", argv, &runner_opts, 0,
                                  child, &running, NULL, NULL) != 0) {
        close(out_pipe[0]);
        close(out_pipe[1]);
        if (capture_stderr) {
            close(err_pipe[0]);
            close(err_pipe[1]);
        }
        if (stdin_fd >= 0)
            close(stdin_fd);
        return false;
    }

    close(out_pipe[1]);
    out_pipe[1] = -1;
    if (capture_stderr) {
        close(err_pipe[1]);
        err_pipe[1] = -1;
    }
    if (stdin_fd >= 0) {
        close(stdin_fd);
        stdin_fd = -1;
    }

    out_stream = fdopen(out_pipe[0], "r");
    if (!out_stream)
        goto fail;
    out_pipe[0] = -1;
    raw_stdout = bx_search_input_read_stream_all(out_stream, &raw_stdout_len);
    if (!raw_stdout)
        goto fail;
    fclose(out_stream);
    out_stream = NULL;

    if (capture_stderr) {
        err_stream = fdopen(err_pipe[0], "r");
        if (!err_stream)
            goto fail;
        err_pipe[0] = -1;
        raw_stderr = bx_search_input_read_stream_all(err_stream, &raw_stderr_len);
        if (!raw_stderr)
            goto fail;
        fclose(err_stream);
        err_stream = NULL;
    }

    if (bx_child_reap(child, &running, true, true, bx_rg_capture_reap_cb, &reap) != 0 || !reap.called)
        goto fail;

    if (exec_failed)
        *exec_failed = reap.exec_failed;
    if (exec_errno)
        *exec_errno = reap.exec_errno;

    char *stderr_text = NULL;
    if (stderr_buf) {
        stderr_text = malloc(raw_stderr_len + 1u);
        if (!stderr_text)
            goto fail;
        if (raw_stderr_len > 0u && raw_stderr)
            memcpy(stderr_text, raw_stderr, raw_stderr_len);
        stderr_text[raw_stderr_len] = '\0';
    }
    if (stdout_buf)
        *stdout_buf = raw_stdout;
    else
        free(raw_stdout);
    if (stdout_len)
        *stdout_len = raw_stdout_len;
    if (stderr_buf)
        *stderr_buf = stderr_text;
    free(raw_stderr);
    if (status_out)
        *status_out = reap.status;
    return true;

fail:
    if (out_stream)
        fclose(out_stream);
    else if (out_pipe[0] >= 0)
        close(out_pipe[0]);
    if (out_pipe[1] >= 0)
        close(out_pipe[1]);
    if (err_stream)
        fclose(err_stream);
    else if (capture_stderr && err_pipe[0] >= 0)
        close(err_pipe[0]);
    if (capture_stderr && err_pipe[1] >= 0)
        close(err_pipe[1]);
    if (stdin_fd >= 0)
        close(stdin_fd);
    free(raw_stdout);
    free(raw_stderr);
    if (running > 0)
        bx_child_reap(child, &running, true, true, NULL, NULL);
    return false;
}

static enum bx_rg_transform_result bx_rg_load_decoded_stream(
    FILE *input,
    const char *filename,
    const char *progname,
    const struct search_opts *opts,
    FILE *err_stream,
    bool close_input,
    unsigned char **output,
    size_t *output_len
) {
    struct stat st;
    int fd = fileno(input);
    bool preflight_ok = true;

    if (fd >= 0) {
        bx_search_dev_counters_note_content_fstat_call();
        if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
            (st.st_size < 0 ||
             (uintmax_t)st.st_size > (uintmax_t)BX_SEARCH_MATERIALIZED_INPUT_LIMIT)) {
            errno = EFBIG;
            preflight_ok = false;
        }
    }
    bool decoded = preflight_ok && bx_rg_decode_stream_limited(
        input, opts->encoding_mode, opts->encoding_name,
        BX_SEARCH_MATERIALIZED_INPUT_LIMIT,
        BX_SEARCH_MATERIALIZED_INPUT_LIMIT,
        output, output_len);
    int decode_errno = errno != 0 ? errno : EIO;

    if (close_input && fclose(input) != 0 && decoded) {
        decoded = false;
        decode_errno = errno != 0 ? errno : EIO;
        free(*output);
        *output = NULL;
        *output_len = 0u;
    }
    if (decoded)
        return BX_RG_TRANSFORM_OK;
    if (!opts->suppress_errors) {
        fprintf(err_stream ? err_stream : stderr, "%s: %s: %s (os error %d)\n",
                progname, filename ? filename : "(standard input)",
                strerror(decode_errno), decode_errno);
    }
    errno = decode_errno;
    return BX_RG_TRANSFORM_ERROR;
}

static enum bx_rg_transform_result bx_rg_load_decoded_file(
    const char *filename,
    const char *progname,
    const struct search_opts *opts,
    FILE *err_stream,
    unsigned char **output,
    size_t *output_len
) {
    FILE *input = fopen(filename, "r");

    if (!input) {
        int open_errno = errno != 0 ? errno : EIO;
        if (!opts->suppress_errors) {
            fprintf(err_stream ? err_stream : stderr, "%s: %s: %s (os error %d)\n",
                    progname, filename, strerror(open_errno), open_errno);
        }
        return BX_RG_TRANSFORM_ERROR;
    }
    return bx_rg_load_decoded_stream(input, filename, progname, opts, err_stream,
                                     true, output, output_len);
}

bool bx_rg_transform_maybe_needed(const struct search_opts *opts,
                                  const char *filename,
                                  bool use_stdin,
                                  int fd_hint) {
    unsigned char prefix[3] = {0};
    FILE *f = NULL;
    size_t nread = 0u;

    if (!opts)
        return false;
    if (use_stdin) {
        /*
         * Pipes cannot be pread without consuming bytes. Route auto-encoded
         * stdin through the incremental reader as well; it passes through a
         * non-BOM prefix without materializing the input.
         */
        (void)fd_hint;
        return opts->encoding_mode != BX_RG_ENCODING_NONE;
    }
    if (bx_rg_transform_needs_file_preload(opts, filename))
        return true;
    if (opts->encoding_mode == BX_RG_ENCODING_NONE)
        return false;
    if (opts->encoding_mode != BX_RG_ENCODING_AUTO)
        return false;

    f = fopen(filename, "r");
    if (!f)
        return false;
    nread = fread(prefix, 1u, sizeof(prefix), f);
    fclose(f);
    return bx_rg_transform_prefix_needs_decode(prefix, nread);
}

static void bx_rg_report_pre_exec_error(FILE *err_stream,
                                        const char *progname,
                                        const char *filename,
                                        const char *command,
                                        int errnum) {
    fprintf(err_stream ? err_stream : stderr,
            "%s: %s: preprocessor command could not start: '\"%s\" \"%s\"': %s (os error %d)\n",
            progname, filename, command, filename, strerror(errnum), errnum);
}

static void bx_rg_report_pre_failure(FILE *err_stream,
                                     const char *progname,
                                     const char *filename,
                                     const char *command,
                                     const char *stderr_text) {
    FILE *stream = err_stream ? err_stream : stderr;

    fprintf(stream, "%s: %s: preprocessor command failed: '\"%s\" \"%s\"': ",
            progname, filename, command, filename);
    if (!stderr_text || *stderr_text == '\0') {
        fputs("<stderr is empty>\n", stream);
        return;
    }
    fputc('\n', stream);
    fputs("-------------------------------------------------------------------------------\n", stream);
    fputs(stderr_text, stream);
    if (stderr_text[strlen(stderr_text) - 1] != '\n')
        fputc('\n', stream);
    fputs("-------------------------------------------------------------------------------\n", stream);
}

static enum bx_rg_transform_result bx_rg_run_preprocessor(const char *filename,
                                                          const char *progname,
                                                          const struct search_opts *opts,
                                                          FILE *err_stream,
                                                          unsigned char **output,
                                                          size_t *output_len) {
    const char *argv[3];
    int stdin_fd = -1;
    int status = 0;
    bool exec_failed = false;
    int exec_errno = 0;
    char *stderr_text = NULL;

    argv[0] = opts->pre_command;
    argv[1] = filename;
    argv[2] = NULL;

    stdin_fd = bx_fd_open_cloexec(filename, O_RDONLY, 0);
    if (stdin_fd < 0) {
        if (!opts->suppress_errors) {
            fprintf(err_stream ? err_stream : stderr, "%s: %s: %s (os error %d)\n",
                    progname, filename, strerror(errno), errno);
        }
        return BX_RG_TRANSFORM_ERROR;
    }

    bx_rg_tracef(opts, "pre: running %s %s", opts->pre_command, filename);
    if (!bx_rg_run_capture(argv, true, stdin_fd, output, output_len, &stderr_text,
                           &status, &exec_failed, &exec_errno)) {
        free(stderr_text);
        return BX_RG_TRANSFORM_ERROR;
    }
    if (exec_failed) {
        bx_rg_report_pre_exec_error(err_stream, progname, filename,
                                    opts->pre_command, exec_errno);
        free(stderr_text);
        return BX_RG_TRANSFORM_ERROR;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        bx_rg_report_pre_failure(err_stream, progname, filename,
                                 opts->pre_command, stderr_text);
        free(stderr_text);
        free(*output);
        *output = NULL;
        *output_len = 0u;
        return BX_RG_TRANSFORM_ERROR;
    }
    free(stderr_text);
    return BX_RG_TRANSFORM_OK;
}

static enum bx_rg_transform_result bx_rg_run_search_zip(const char *filename,
                                                        const struct search_opts *opts,
                                                        unsigned char **output,
                                                        size_t *output_len) {
    const char *const *argv = NULL;
    const char *tool = bx_rg_search_zip_program(filename, &argv);
    int status = 0;
    bool exec_failed = false;
    int exec_errno = 0;

    if (!tool)
        return BX_RG_TRANSFORM_NO_MATCH;

    bx_rg_tracef(opts, "search-zip: running %s for %s", tool, filename);
    if (!bx_rg_run_capture(argv, false, -1, output, output_len, NULL,
                           &status, &exec_failed, &exec_errno)) {
        free(*output);
        *output = NULL;
        *output_len = 0u;
        return BX_RG_TRANSFORM_NO_MATCH;
    }
    if (exec_failed || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        bx_rg_tracef(opts, "search-zip: %s unavailable or failed for %s", tool, filename);
        free(*output);
        *output = NULL;
        *output_len = 0u;
        return BX_RG_TRANSFORM_NO_MATCH;
    }
    return BX_RG_TRANSFORM_OK;
}

enum bx_rg_transform_result bx_rg_load_transformed_input(
    const char *filename,
    const char *progname,
    const struct search_opts *opts,
    FILE *err_stream,
    unsigned char **output,
    size_t *output_len) {
    enum bx_rg_transform_result rc = BX_RG_TRANSFORM_ERROR;
    unsigned char *raw = NULL;
    size_t raw_len = 0u;
    unsigned char *decoded = NULL;
    size_t decoded_len = 0u;
    bool use_stdin = !filename || strcmp(filename, "-") == 0;

    if (!output || !output_len || !opts)
        return BX_RG_TRANSFORM_ERROR;
    *output = NULL;
    *output_len = 0u;

    if (!use_stdin && filename && opts->pre_command && *opts->pre_command &&
        bx_rg_pre_glob_matches(opts, filename)) {
        rc = bx_rg_run_preprocessor(filename, progname, opts, err_stream, &raw, &raw_len);
    } else if (!use_stdin && filename && opts->search_zip) {
        const char *const *zip_argv = NULL;
        if (bx_rg_search_zip_program(filename, &zip_argv) != NULL) {
            rc = bx_rg_run_search_zip(filename, opts, &raw, &raw_len);
            if (rc != BX_RG_TRANSFORM_OK)
                return rc;
        } else {
            return bx_rg_load_decoded_file(filename, progname, opts, err_stream,
                                           output, output_len);
        }
    } else if (use_stdin) {
        return bx_rg_load_decoded_stream(stdin, NULL, progname, opts, err_stream,
                                         false, output, output_len);
    } else {
        return bx_rg_load_decoded_file(filename, progname, opts, err_stream,
                                       output, output_len);
    }

    if (rc != BX_RG_TRANSFORM_OK)
        return rc;
    if (!bx_rg_decode_buffer_limited(opts->encoding_mode, opts->encoding_name,
                                     raw, raw_len,
                                     BX_SEARCH_MATERIALIZED_INPUT_LIMIT,
                                     &decoded, &decoded_len)) {
        int decode_errno = errno != 0 ? errno : EIO;
        free(raw);
        if (!opts->suppress_errors) {
            fprintf(err_stream ? err_stream : stderr, "%s: %s: %s (os error %d)\n",
                    progname, filename ? filename : "(standard input)",
                    strerror(decode_errno), decode_errno);
        }
        errno = decode_errno;
        return BX_RG_TRANSFORM_ERROR;
    }
    free(raw);
    *output = decoded;
    *output_len = decoded_len;
    return BX_RG_TRANSFORM_OK;
}
