#ifndef BX_APPLETS_ARCHIVE_TAR_TAR_CREATE_H
#define BX_APPLETS_ARCHIVE_TAR_TAR_CREATE_H

#include <stdbool.h>

#include "applets/archive/archive_common.h"
#include "applets/archive/archive_fs.h"
#include "bx/diag.h"

struct bx_tar_name_source {
    char* path;
    char* cwd;
};

struct bx_tar_name_source_list {
    struct bx_tar_name_source* items;
    size_t len;
    size_t cap;
};

struct bx_tar_files_from_source {
    char* path;
    char* cwd;
    unsigned char separator;
};

struct bx_tar_files_from_source_list {
    struct bx_tar_files_from_source* items;
    size_t len;
    size_t cap;
};

struct bx_tar_create_options {
    bool recurse;
    bool remove_files;
    unsigned char files_from_separator;
    struct bx_archive_name_list exclude_patterns;
    struct bx_tar_name_source_list exclude_from_sources;
    struct bx_tar_files_from_source_list files_from_sources;
};

void bx_tar_create_options_cleanup(struct bx_tar_create_options* options);
bool bx_tar_create_options_add_exclude_pattern(struct bx_tar_create_options* options,
                                               const char* pattern);
bool bx_tar_create_options_add_exclude_from(struct bx_tar_create_options* options,
                                            const char* path,
                                            const char* cwd);
bool bx_tar_create_options_add_files_from(struct bx_tar_create_options* options,
                                          const char* path,
                                          const char* cwd);

bool bx_tar_create_collect_fs_entries(struct bx_archive_fs_list* list,
                                      const struct bx_tar_create_options* create_options,
                                      const char* create_cwd,
                                      int argc,
                                      char** argv,
                                      int operand_index,
                                      bool sort_children,
                                      bool* had_create_errors,
                                      struct bx_diag_ctx* diag);

bool bx_tar_create_remove_archived_sources(const struct bx_archive_fs_list* list,
                                           const struct bx_diag_ctx* diag);

#endif /* BX_APPLETS_ARCHIVE_TAR_TAR_CREATE_H */
