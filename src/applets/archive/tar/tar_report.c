#include <errno.h>
#include <stdarg.h>
#include <string.h>

#include "applets/archive/tar/tar_report.h"

bool bx_tar_report_output_init(struct bx_tar_report_output* output,
                               const char* index_file_path,
                               FILE* default_stream,
                               struct bx_diag_ctx* diag) {
    memset(output, 0, sizeof(*output));
    if (index_file_path != NULL) {
        output->stream = fopen(index_file_path, "wb");
        if (output->stream == NULL) {
            bx_diag(diag, "%s: %s", index_file_path, strerror(errno));
            return false;
        }
        output->close_stream = true;
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

    if (output->close_stream) {
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
    return ok;
}

void bx_tar_report_output_cleanup(struct bx_tar_report_output* output) {
    if (output->close_stream && output->stream != NULL) {
        fclose(output->stream);
    }
    output->stream = NULL;
    output->close_stream = false;
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

bool bx_tar_report_member_line(FILE* stream,
                               const char* name,
                               bool is_directory,
                               struct bx_diag_ctx* diag) {
    return bx_tar_report_printf(stream, diag, is_directory ? "%s/\n" : "%s\n", name);
}
