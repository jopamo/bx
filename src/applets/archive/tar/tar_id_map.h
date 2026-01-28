#ifndef BX_APPLETS_ARCHIVE_TAR_TAR_ID_MAP_H
#define BX_APPLETS_ARCHIVE_TAR_TAR_ID_MAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "bx/diag.h"

struct bx_tar_id_map_rule {
    char* source_text;
    bool source_numeric;
    uintmax_t source_id;
    char* dest_text;
    uintmax_t dest_id;
};

struct bx_tar_id_map {
    struct bx_tar_id_map_rule* rules;
    size_t len;
    size_t cap;
};

void bx_tar_id_map_cleanup(struct bx_tar_id_map* map);

bool bx_tar_id_map_load_owner(struct bx_tar_id_map* map,
                              const char* path,
                              struct bx_diag_ctx* diag);
bool bx_tar_id_map_load_group(struct bx_tar_id_map* map,
                              const char* path,
                              struct bx_diag_ctx* diag);

bool bx_tar_id_map_apply_owner(const struct bx_tar_id_map* map,
                               uid_t source_uid,
                               const char* source_name,
                               uid_t* mapped_uid_out,
                               const char** mapped_name_out);
bool bx_tar_id_map_apply_group(const struct bx_tar_id_map* map,
                               gid_t source_gid,
                               const char* source_name,
                               gid_t* mapped_gid_out,
                               const char** mapped_name_out);

#endif
