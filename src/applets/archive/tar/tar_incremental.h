#ifndef BX_APPLETS_ARCHIVE_TAR_TAR_INCREMENTAL_H
#define BX_APPLETS_ARCHIVE_TAR_TAR_INCREMENTAL_H

#include <stdbool.h>
#include <stddef.h>

#include "applets/archive/archive_fs.h"
#include "bx/diag.h"

struct bx_tar_incremental_state;

struct bx_tar_incremental_plan {
    struct bx_tar_incremental_state* state;
};

bool bx_tar_incremental_plan_init(struct bx_tar_incremental_plan* plan, const char* snapshot_path, struct bx_diag_ctx* diag);
bool bx_tar_incremental_plan_prepare(struct bx_tar_incremental_plan* plan, const struct bx_archive_fs_list* files, struct bx_diag_ctx* diag);
void bx_tar_incremental_plan_filter_files(struct bx_tar_incremental_plan* plan, struct bx_archive_fs_list* files);
void bx_tar_incremental_plan_order_files(struct bx_tar_incremental_plan* plan, struct bx_archive_fs_list* files);
bool bx_tar_incremental_directory_data(const char* archive_path, const unsigned char** data_out, size_t* data_len_out, void* user_data, struct bx_diag_ctx* diag);
bool bx_tar_incremental_plan_publish(const struct bx_tar_incremental_plan* plan, struct bx_diag_ctx* diag);
void bx_tar_incremental_plan_cleanup(struct bx_tar_incremental_plan* plan);

#endif /* BX_APPLETS_ARCHIVE_TAR_TAR_INCREMENTAL_H */
