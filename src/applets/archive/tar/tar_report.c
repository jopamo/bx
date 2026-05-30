#include <inttypes.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "applets/archive/tar/tar_report.h"
#include "lib/fd_ops.h"

bool bx_tar_report_output_init(struct bx_tar_report_output* output,
                               const char* index_file_path,
                               FILE* default_stream,
                               struct bx_diag_ctx* diag) {
    memset(output, 0, sizeof(*output));
    if (index_file_path != NULL) {
        int fd = bx_fd_open_cloexec(index_file_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);

        if (fd < 0) {
            bx_diag(diag, "%s: %s", index_file_path, strerror(errno));
            return false;
        }
        if (!bx_line_writer_file_open(&output->line_file, fd, true)) {
            bx_diag(diag, "%s: %s", index_file_path, strerror(errno));
            return false;
        }
        output->stream = bx_line_writer_file_stream(&output->line_file);
        output->line_file_active = true;
        return true;
    }

    if (default_stream == stdout) {
        if (!bx_line_writer_file_open(&output->line_file, STDOUT_FILENO, false)) {
            bx_diag(diag, "write error: %s", strerror(errno));
            return false;
        }
        output->stream = bx_line_writer_file_stream(&output->line_file);
        output->line_file_active = true;
        return true;
    }

    output->stream = default_stream;
    return true;
}

bool bx_tar_report_output_finish(struct bx_tar_report_output* output,
                                 struct bx_diag_ctx* diag) {
    FILE* stream = output->stream;
    bool ok = true;

    if (stream == NULL) {
        return true;
    }

    if (output->line_file_active) {
        if (!bx_line_writer_file_finish(&output->line_file)) {
            int errnum = bx_line_writer_file_error(&output->line_file);

            bx_diag(diag, "write error: %s", strerror(errnum));
            ok = false;
        }
    }
    else if (output->close_stream) {
        if (fclose(stream) != 0) {
            bx_diag(diag, "write error: %s", strerror(errno));
            ok = false;
        }
    }
    else if (fflush(stream) != 0) {
        bx_diag(diag, "write error: %s", strerror(errno));
        ok = false;
    }

    output->stream = NULL;
    output->close_stream = false;
    output->line_file_active = false;
    return ok;
}

void bx_tar_report_output_cleanup(struct bx_tar_report_output* output) {
    if (output->line_file_active) {
        bx_line_writer_file_cleanup(&output->line_file);
    }
    else if (output->close_stream && output->stream != NULL) {
        fclose(output->stream);
    }
    output->stream = NULL;
    output->close_stream = false;
    output->line_file_active = false;
}

bool bx_tar_report_printf(FILE* stream,
                          struct bx_diag_ctx* diag,
                          const char* fmt,
                          ...) {
    va_list ap;
    int rc;

    va_start(ap, fmt);
    rc = vfprintf(stream, fmt, ap);
    va_end(ap);
    if (rc < 0) {
        bx_diag(diag, "write error: %s", strerror(errno));
        return false;
    }
    return true;
}

static bool bx_tar_report_write(FILE* stream,
                                const void* data,
                                size_t len,
                                struct bx_diag_ctx* diag) {
    if (len == 0u) {
        return true;
    }
    if (fwrite(data, 1u, len, stream) == len) {
        return true;
    }
    bx_diag(diag, "write error: %s", strerror(errno));
    return false;
}

bool bx_tar_report_member_line(FILE* stream,
                               const char* name,
                               bool is_directory,
                               struct bx_diag_ctx* diag) {
    size_t name_len = strlen(name);

    return bx_tar_report_write(stream, name, name_len, diag)
        && bx_tar_report_write(stream, is_directory ? "/\n" : "\n", is_directory ? 2u : 1u, diag);
}

bool bx_tar_report_member_line_with_block(FILE* stream,
                                          uint64_t block_index,
                                          const char* name,
                                          bool is_directory,
                                          struct bx_diag_ctx* diag) {
    char prefix[64];
    int prefix_len = snprintf(prefix, sizeof(prefix), "block %" PRIu64 ": ", block_index);

    if (prefix_len < 0 || (size_t)prefix_len >= sizeof(prefix)) {
        return bx_tar_report_printf(stream,
                                    diag,
                                    is_directory
                                        ? "block %" PRIu64 ": %s/\n"
                                        : "block %" PRIu64 ": %s\n",
                                    block_index,
                                    name);
    }
    return bx_tar_report_write(stream, prefix, (size_t)prefix_len, diag)
        && bx_tar_report_member_line(stream, name, is_directory, diag);
}

bool bx_tar_report_archive_end(FILE* stream,
                               uint64_t block_index,
                               bool zero_block_terminated,
                               struct bx_diag_ctx* diag) {
    return bx_tar_report_printf(stream,
                                diag,
                                "block %" PRIu64 ": %s\n",
                                block_index,
                                zero_block_terminated
                                    ? "** Block of NULs **"
                                    : "** End of File **");
}
