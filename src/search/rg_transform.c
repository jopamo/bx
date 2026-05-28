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
#include "lib/path_ops.h"
#include "options.h"
#include "rg_text.h"
#include "rg_transform.h"

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

static bool bx_rg_transform_prefix_needs_decode(const unsigned char *prefix,
                                                size_t nread) {
    if (!prefix || nread < 2u)
        return false;
    return (nread >= 3u &&
            prefix[0] == 0xEFu && prefix[1] == 0xBBu && prefix[2] == 0xBFu) ||
           (prefix[0] == 0xFFu && prefix[1] == 0xFEu) ||
           (prefix[0] == 0xFEu && prefix[1] == 0xFFu);
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

bool bx_rg_transform_auto_encoding_needs_prefix(const struct search_opts *opts,
                                                const unsigned char *prefix,
                                                size_t nread) {
    if (!opts || opts->encoding_mode != BX_RG_ENCODING_AUTO)
        return false;
    return bx_rg_transform_prefix_needs_decode(prefix, nread);
}

bool bx_rg_transform_auto_encoding_needs_fd(const struct search_opts *opts,
                                            int fd_hint) {
    unsigned char prefix[3] = {0};
    ssize_t nread;

    if (!opts || fd_hint < 0)
        return false;

    nread = pread(fd_hint, prefix, sizeof(prefix), 0);
    if (nread < 0)
        return false;
    bx_search_dev_counters_note_content_pread((size_t)nread);
    bx_search_dev_counters_note_prefix_pread((size_t)nread);
    bx_search_dev_counters_note_prefix_bytes_rescanned((size_t)nread);
    return bx_rg_transform_auto_encoding_needs_prefix(opts, prefix, (size_t)nread);
}

static bool bx_rg_slurp_stream(FILE *f, unsigned char **output, size_t *output_len) {
    size_t cap = 0u;
    size_t len = 0u;
    unsigned char *buf = NULL;
    unsigned char chunk[4096];

    if (!output || !output_len)
        return false;
    *output = NULL;
    *output_len = 0u;

    while (!feof(f)) {
        size_t nread = fread(chunk, 1u, sizeof(chunk), f);
        if (nread == 0u)
            break;
        if (len + nread + 1u > cap) {
            size_t new_cap = cap == 0u ? 8192u : cap * 2u;
            while (new_cap < len + nread + 1u)
                new_cap *= 2u;
            unsigned char *grown = realloc(buf, new_cap);
            if (!grown) {
                free(buf);
                return false;
            }
            buf = grown;
            cap = new_cap;
        }
        memcpy(buf + len, chunk, nread);
        len += nread;
    }
    if (ferror(f)) {
        free(buf);
        return false;
    }
    if (!buf) {
        buf = malloc(1u);
        if (!buf)
            return false;
    }
    buf[len] = '\0';
    *output = buf;
    *output_len = len;
    return true;
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
    int exec_pipe[2] = {-1, -1};
    pid_t pid;
    FILE *out_stream = NULL;
    FILE *err_stream = NULL;
    unsigned char *raw_stdout = NULL;
    size_t raw_stdout_len = 0u;
    unsigned char *raw_stderr = NULL;
    size_t raw_stderr_len = 0u;
    int child_status = 0;

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

    if (pipe(out_pipe) != 0)
        return false;
    if (capture_stderr && pipe(err_pipe) != 0) {
        close(out_pipe[0]);
        close(out_pipe[1]);
        return false;
    }
    if (pipe(exec_pipe) != 0) {
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

    pid = fork();
    if (pid < 0) {
        close(out_pipe[0]); close(out_pipe[1]);
        if (capture_stderr) { close(err_pipe[0]); close(err_pipe[1]); }
        close(exec_pipe[0]); close(exec_pipe[1]);
        if (stdin_fd >= 0)
            close(stdin_fd);
        return false;
    }
    if (pid == 0) {
        size_t argc = 0u;
        char **exec_argv = NULL;
        int errnum = 0;

        while (argv && argv[argc])
            argc++;
        exec_argv = calloc(argc + 1u, sizeof(*exec_argv));
        if (!exec_argv)
            _exit(127);
        for (size_t i = 0; i < argc; i++) {
            exec_argv[i] = strdup(argv[i]);
            if (!exec_argv[i])
                _exit(127);
        }

        close(out_pipe[0]);
        close(exec_pipe[0]);
        if (capture_stderr)
            close(err_pipe[0]);
        if (stdin_fd >= 0) {
            if (dup2(stdin_fd, STDIN_FILENO) < 0)
                goto child_fail;
            close(stdin_fd);
        }
        if (dup2(out_pipe[1], STDOUT_FILENO) < 0)
            goto child_fail;
        if (capture_stderr) {
            if (dup2(err_pipe[1], STDERR_FILENO) < 0)
                goto child_fail;
        }
        close(out_pipe[1]);
        if (capture_stderr)
            close(err_pipe[1]);
        execvp(exec_argv[0], exec_argv);
child_fail:
        errnum = errno;
        (void)!write(exec_pipe[1], &errnum, sizeof(errnum));
        _exit(127);
    }

    close(out_pipe[1]);
    close(exec_pipe[1]);
    if (capture_stderr)
        close(err_pipe[1]);
    if (stdin_fd >= 0)
        close(stdin_fd);

    out_stream = fdopen(out_pipe[0], "r");
    if (!out_stream)
        goto fail;
    if (!bx_rg_slurp_stream(out_stream, &raw_stdout, &raw_stdout_len))
        goto fail;
    fclose(out_stream);
    out_stream = NULL;

    if (capture_stderr) {
        err_stream = fdopen(err_pipe[0], "r");
        if (!err_stream)
            goto fail;
        if (!bx_rg_slurp_stream(err_stream, &raw_stderr, &raw_stderr_len))
            goto fail;
        fclose(err_stream);
        err_stream = NULL;
    }

    int child_exec_errno = 0;
    ssize_t exec_read = read(exec_pipe[0], &child_exec_errno, sizeof(child_exec_errno));
    close(exec_pipe[0]);
    exec_pipe[0] = -1;
    if (waitpid(pid, &child_status, 0) < 0)
        goto fail;

    if (exec_read == (ssize_t)sizeof(child_exec_errno)) {
        if (exec_failed)
            *exec_failed = true;
        if (exec_errno)
            *exec_errno = child_exec_errno;
    }

    if (stdout_buf)
        *stdout_buf = raw_stdout;
    else
        free(raw_stdout);
    if (stdout_len)
        *stdout_len = raw_stdout_len;
    if (stderr_buf) {
        char *tmp = malloc(raw_stderr_len + 1u);
        if (!tmp)
            goto fail;
        if (raw_stderr_len > 0u && raw_stderr)
            memcpy(tmp, raw_stderr, raw_stderr_len);
        tmp[raw_stderr_len] = '\0';
        *stderr_buf = tmp;
    }
    free(raw_stderr);
    if (status_out)
        *status_out = child_status;
    return true;

fail:
    if (out_stream)
        fclose(out_stream);
    else if (out_pipe[0] >= 0)
        close(out_pipe[0]);
    if (err_stream)
        fclose(err_stream);
    else if (capture_stderr && err_pipe[0] >= 0)
        close(err_pipe[0]);
    if (exec_pipe[0] >= 0)
        close(exec_pipe[0]);
    free(raw_stdout);
    free(raw_stderr);
    waitpid(pid, &child_status, 0);
    return false;
}

static enum bx_rg_transform_result bx_rg_load_file_bytes(const char *filename,
                                                         const char *progname,
                                                         const struct search_opts *opts,
                                                         FILE *err_stream,
                                                         unsigned char **output,
                                                         size_t *output_len) {
    FILE *f;
    f = fopen(filename, "r");
    if (!f) {
        if (!opts || !opts->suppress_errors) {
            fprintf(err_stream ? err_stream : stderr, "%s: %s: %s (os error %d)\n",
                    progname, filename, strerror(errno), errno);
        }
        return BX_RG_TRANSFORM_ERROR;
    }
    if (!bx_rg_slurp_stream(f, output, output_len)) {
        fclose(f);
        return BX_RG_TRANSFORM_ERROR;
    }
    fclose(f);
    return BX_RG_TRANSFORM_OK;
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
        if (opts->encoding_mode == BX_RG_ENCODING_EXPLICIT)
            return true;
        return bx_rg_transform_auto_encoding_needs_fd(opts, fd_hint);
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

    stdin_fd = open(filename, O_RDONLY);
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
            rc = bx_rg_load_file_bytes(filename, progname, opts, err_stream, &raw, &raw_len);
        }
    } else if (use_stdin) {
        if (!bx_rg_slurp_stream(stdin, &raw, &raw_len))
            return BX_RG_TRANSFORM_ERROR;
        rc = BX_RG_TRANSFORM_OK;
    } else {
        rc = bx_rg_load_file_bytes(filename, progname, opts, err_stream, &raw, &raw_len);
    }

    if (rc != BX_RG_TRANSFORM_OK)
        return rc;
    if (!bx_rg_decode_buffer(opts->encoding_mode, opts->encoding_name, raw, raw_len,
                             &decoded, &decoded_len)) {
        free(raw);
        return BX_RG_TRANSFORM_ERROR;
    }
    free(raw);
    *output = decoded;
    *output_len = decoded_len;
    return BX_RG_TRANSFORM_OK;
}
