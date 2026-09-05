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

typedef struct {
    const char* path;
    const char* open_summary;
    const char* write_summary;
    bool append;
} MiraLogSpec;

static void mira_report_log_error(const struct bx_fetch_config* config, const MiraLogSpec* spec, const char* summary, int error_number) {
    char* quoted = bx_path_quote_dup(spec->path, BX_PATH_QUOTE_C);
    if (quoted) {
        fprintf(stderr, "mira: %s: %s\n", summary, quoted);
        free(quoted);
    }
    else {
        fprintf(stderr, "mira: %s\n", summary);
    }
    if (config->logging.structured_errors) {
        bx_fetch_error_emit_simple(stderr, BX_FETCH_ERROR_CLASS_FILESYSTEM, summary, NULL, spec->path, -1, error_number);
    }
}

static FILE* mira_log_open(const struct bx_fetch_config* config, const MiraLogSpec* spec) {
    int flags = O_WRONLY | O_CREAT | (spec->append ? O_APPEND : 0);
    int fd = bx_fd_open_nofollow_cloexec(spec->path, flags, S_IRUSR | S_IWUSR);
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
    if (!spec->append && bx_fd_ftruncate(fd, 0) != 0)
        goto close_fail;

    FILE* stream = fdopen(fd, spec->append ? "a" : "w");
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
    mira_report_log_error(config, spec, spec->open_summary, errno ? errno : EIO);
    return NULL;
}

static int mira_log_finish(const struct bx_fetch_config* config, const MiraLogSpec* spec, FILE* stream, int exit_code) {
    if (!config || !spec || !stream) {
        errno = EINVAL;
        return bx_fetch_exit_combine(exit_code, BX_FETCH_EXIT_FILE_IO);
    }

    bool write_failed = ferror(stream);
    errno = 0;
    if (fclose(stream) != 0)
        write_failed = true;
    if (!write_failed)
        return exit_code;

    int error_number = errno ? errno : EIO;
    mira_report_log_error(config, spec, spec->write_summary, error_number);
    return bx_fetch_exit_combine(exit_code, BX_FETCH_EXIT_FILE_IO);
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
            return NULL;
    }
    const MiraLogSpec spec = {
        .path = config->logging.log_file,
        .open_summary = "failed to open log file",
        .write_summary = "failed to write log file",
        .append = append,
    };
    return mira_log_open(config, &spec);
}

int bx_mira_diagnostics_finish(const struct bx_fetch_config* config, FILE* diagnostics, int exit_code) {
    if (!config || !diagnostics) {
        errno = EINVAL;
        return bx_fetch_exit_combine(exit_code, BX_FETCH_EXIT_FILE_IO);
    }
    if (diagnostics == stderr)
        return exit_code;
    const MiraLogSpec spec = {
        .path = config->logging.log_file,
        .open_summary = "failed to open log file",
        .write_summary = "failed to write log file",
        .append = config->logging.log_file_mode == BX_FETCH_LOG_FILE_APPEND,
    };
    return mira_log_finish(config, &spec, diagnostics, exit_code);
}

int bx_mira_rejected_log_open(const struct bx_fetch_config* config, FILE** rejected_log_out) {
    if (!config || !rejected_log_out) {
        errno = EINVAL;
        return -1;
    }
    *rejected_log_out = NULL;
    if (!config->logging.rejected_log || config->download.dry_run)
        return 0;

    const MiraLogSpec spec = {
        .path = config->logging.rejected_log,
        .open_summary = "failed to open rejected URL log",
        .write_summary = "failed to write rejected URL log",
        .append = true,
    };
    *rejected_log_out = mira_log_open(config, &spec);
    return *rejected_log_out ? 0 : -1;
}

int bx_mira_rejected_log_finish(const struct bx_fetch_config* config, FILE* rejected_log, int exit_code) {
    if (!rejected_log)
        return exit_code;
    const MiraLogSpec spec = {
        .path = config ? config->logging.rejected_log : NULL,
        .open_summary = "failed to open rejected URL log",
        .write_summary = "failed to write rejected URL log",
        .append = true,
    };
    return mira_log_finish(config, &spec, rejected_log, exit_code);
}
