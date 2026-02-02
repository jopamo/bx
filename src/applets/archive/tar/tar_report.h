#ifndef BX_APPLETS_ARCHIVE_TAR_REPORT_H
#define BX_APPLETS_ARCHIVE_TAR_REPORT_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "bx/diag.h"

struct bx_tar_report_output {
    FILE* stream;
    bool close_stream;
};

bool bx_tar_report_output_init(struct bx_tar_report_output* output,
                               const char* index_file_path,
                               FILE* default_stream,
                               struct bx_diag_ctx* diag);
bool bx_tar_report_output_finish(struct bx_tar_report_output* output,
                                 struct bx_diag_ctx* diag);
void bx_tar_report_output_cleanup(struct bx_tar_report_output* output);

bool bx_tar_report_printf(FILE* stream,
                          struct bx_diag_ctx* diag,
                          const char* fmt,
                          ...);
bool bx_tar_report_member_line(FILE* stream,
                               const char* name,
                               bool is_directory,
                               struct bx_diag_ctx* diag);
bool bx_tar_report_member_line_with_block(FILE* stream,
                                          uint64_t block_index,
                                          const char* name,
                                          bool is_directory,
                                          struct bx_diag_ctx* diag);
bool bx_tar_report_archive_end(FILE* stream,
                               uint64_t block_index,
                               bool zero_block_terminated,
                               struct bx_diag_ctx* diag);

#endif /* BX_APPLETS_ARCHIVE_TAR_REPORT_H */
