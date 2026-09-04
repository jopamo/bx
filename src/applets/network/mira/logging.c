#define _GNU_SOURCE
#include "logging.h"
#include "lib/fetch/error.h"
#include "lib/fetch/exit_code.h"
#include "lib/fd_ops.h"
#include "lib/path_quote.h"
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>

static void mira_report_log_error(const struct bx_fetch_config* config, const char* summary, int error_number) {
    char* quoted = bx_path_quote_dup(config->logging.log_file, BX_PATH_QUOTE_C);
    if (quoted) {
        fprintf(stderr, "mira: %s: %s\n", summary, quoted);
        free(quoted);
    }
    else {
        fprintf(stderr, "mira: %s\n", summary);
    }
    if (config->logging.structured_errors) {
        bx_fetch_error_emit_simple(stderr, BX_FETCH_ERROR_CLASS_FILESYSTEM, summary, NULL, config->logging.log_file, -1, error_number);
    }
}

FILE* bx_mira_diagnostics_open(const struct bx_fetch_config* config) {
    if (!config) {
        errno = EINVAL;
        return NULL;
    }
    if (!config->logging.log_file || config->download.dry_run)
        return stderr;

    bool append = false;
    switch (config->logging.log_file_mode) {
        case BX_FETCH_LOG_FILE_TRUNCATE:
            break;
        case BX_FETCH_LOG_FILE_APPEND:
            append = true;
            break;
        case BX_FETCH_LOG_FILE_NONE:
        default:
            errno = EINVAL;
            goto fail;
    }
    int flags = O_WRONLY | O_CREAT | (append ? O_APPEND : 0);
    int fd = bx_fd_open_nofollow_cloexec(config->logging.log_file, flags, S_IRUSR | S_IWUSR);
    if (fd < 0)
        goto fail;

    struct stat status;
    if (fstat(fd, &status) != 0)
        goto close_fail;
    if (!S_ISREG(status.st_mode)) {
        errno = EINVAL;
        goto close_fail;
    }
    if (status.st_nlink != 1) {
        errno = EMLINK;
        goto close_fail;
    }
    if (!append && bx_fd_ftruncate(fd, 0) != 0)
        goto close_fail;

    FILE* stream = fdopen(fd, append ? "a" : "w");
    if (!stream)
        goto close_fail;
    if (setvbuf(stream, NULL, _IONBF, 0) != 0) {
        int error_number = errno ? errno : EIO;
        fclose(stream);
        errno = error_number;
        goto fail;
    }
    return stream;

close_fail: {
    int error_number = errno ? errno : EIO;
    bx_fd_cleanup(&fd);
    errno = error_number;
}
fail:
    mira_report_log_error(config, "failed to open log file", errno ? errno : EIO);
    return NULL;
}

int bx_mira_diagnostics_finish(const struct bx_fetch_config* config, FILE* diagnostics, int exit_code) {
    if (!config || !diagnostics) {
        errno = EINVAL;
        return bx_fetch_exit_combine(exit_code, BX_FETCH_EXIT_FILE_IO);
    }
    if (diagnostics == stderr)
        return exit_code;

    bool write_failed = ferror(diagnostics);
    errno = 0;
    if (fclose(diagnostics) != 0)
        write_failed = true;
    if (!write_failed)
        return exit_code;

    int error_number = errno ? errno : EIO;
    mira_report_log_error(config, "failed to write log file", error_number);
    return bx_fetch_exit_combine(exit_code, BX_FETCH_EXIT_FILE_IO);
}
