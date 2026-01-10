#ifndef BX_APPLETS_ARCHIVE_TAR_TAR_FILES_FROM_H
#define BX_APPLETS_ARCHIVE_TAR_TAR_FILES_FROM_H

#include <stdbool.h>
#include <stddef.h>

#include "applets/archive/archive_common.h"

bool bx_tar_files_from_read_buffer(const char* path,
                                   struct bx_archive_buffer* buffer,
                                   struct bx_diag_ctx* diag);

char* bx_tar_files_from_unquote_text(const char* text);

char* bx_tar_files_from_decode_text(bool verbatim,
                                    bool unquote,
                                    const char* text);

const char* bx_tar_files_from_skip_inline_space(const char* text);

void bx_tar_files_from_report_option_error(const struct bx_diag_ctx* diag,
                                           const char* list_path,
                                           size_t record_no,
                                           const char* message);

#endif /* BX_APPLETS_ARCHIVE_TAR_TAR_FILES_FROM_H */
