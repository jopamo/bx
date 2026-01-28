#ifndef BX_APPLETS_ARCHIVE_TAR_TAR_CREATE_H
#define BX_APPLETS_ARCHIVE_TAR_TAR_CREATE_H

#include <stdbool.h>

#include "applets/archive/archive_common.h"
#include "applets/archive/archive_fs.h"
#include "applets/archive/tar/tar_patterns.h"
#include "bx/diag.h"

enum bx_tar_create_directive_kind {
    BX_TAR_CREATE_DIRECTIVE_CHDIR = 0,
    BX_TAR_CREATE_DIRECTIVE_ADD_PATH,
    BX_TAR_CREATE_DIRECTIVE_EXCLUDE_PATTERN,
    BX_TAR_CREATE_DIRECTIVE_EXCLUDE_FROM,
    BX_TAR_CREATE_DIRECTIVE_RECURSE_ON,
    BX_TAR_CREATE_DIRECTIVE_RECURSE_OFF,
    BX_TAR_CREATE_DIRECTIVE_FILES_FROM,
    BX_TAR_CREATE_DIRECTIVE_FILES_FROM_NULL_ON,
    BX_TAR_CREATE_DIRECTIVE_FILES_FROM_NULL_OFF,
    BX_TAR_CREATE_DIRECTIVE_FILES_FROM_VERBATIM_ON,
    BX_TAR_CREATE_DIRECTIVE_FILES_FROM_VERBATIM_OFF,
    BX_TAR_CREATE_DIRECTIVE_FILES_FROM_UNQUOTE_ON,
    BX_TAR_CREATE_DIRECTIVE_FILES_FROM_UNQUOTE_OFF,
    BX_TAR_CREATE_DIRECTIVE_ANCHORED_ON,
    BX_TAR_CREATE_DIRECTIVE_ANCHORED_OFF,
    BX_TAR_CREATE_DIRECTIVE_IGNORE_CASE_ON,
    BX_TAR_CREATE_DIRECTIVE_IGNORE_CASE_OFF,
    BX_TAR_CREATE_DIRECTIVE_WILDCARDS_ON,
    BX_TAR_CREATE_DIRECTIVE_WILDCARDS_OFF,
    BX_TAR_CREATE_DIRECTIVE_WILDCARDS_MATCH_SLASH_ON,
    BX_TAR_CREATE_DIRECTIVE_WILDCARDS_MATCH_SLASH_OFF,
    BX_TAR_CREATE_DIRECTIVE_EXCLUDE_CACHES,
    BX_TAR_CREATE_DIRECTIVE_EXCLUDE_CACHES_ALL,
    BX_TAR_CREATE_DIRECTIVE_EXCLUDE_CACHES_UNDER,
    BX_TAR_CREATE_DIRECTIVE_EXCLUDE_IGNORE,
    BX_TAR_CREATE_DIRECTIVE_EXCLUDE_IGNORE_RECURSIVE,
    BX_TAR_CREATE_DIRECTIVE_EXCLUDE_TAG,
    BX_TAR_CREATE_DIRECTIVE_EXCLUDE_TAG_ALL,
    BX_TAR_CREATE_DIRECTIVE_EXCLUDE_TAG_UNDER,
    BX_TAR_CREATE_DIRECTIVE_EXCLUDE_VCS,
    BX_TAR_CREATE_DIRECTIVE_EXCLUDE_VCS_IGNORES,
};

struct bx_tar_create_directive {
    enum bx_tar_create_directive_kind kind;
    char* text;
};

struct bx_tar_create_directive_list {
    struct bx_tar_create_directive* items;
    size_t len;
    size_t cap;
};

struct bx_tar_create_options {
    bool remove_files;
    struct bx_tar_create_directive_list directives;
};

bool bx_tar_create_options_has_inputs(const struct bx_tar_create_options* options);
void bx_tar_create_options_cleanup(struct bx_tar_create_options* options);
bool bx_tar_create_options_add_exclude_pattern(struct bx_tar_create_options* options,
                                               const char* pattern);
bool bx_tar_create_options_add_exclude_from(struct bx_tar_create_options* options,
                                            const char* path);
bool bx_tar_create_options_add_add_file(struct bx_tar_create_options* options,
                                        const char* path);
bool bx_tar_create_options_add_chdir(struct bx_tar_create_options* options,
                                     const char* path);
bool bx_tar_create_options_set_recurse(struct bx_tar_create_options* options,
                                       bool enabled);
bool bx_tar_create_options_add_files_from(struct bx_tar_create_options* options,
                                          const char* path);
bool bx_tar_create_options_set_files_from_null(struct bx_tar_create_options* options,
                                               bool enabled);
bool bx_tar_create_options_set_files_from_verbatim(struct bx_tar_create_options* options,
                                                   bool enabled);
bool bx_tar_create_options_set_files_from_unquote(struct bx_tar_create_options* options,
                                                  bool enabled);
bool bx_tar_create_options_set_anchored(struct bx_tar_create_options* options,
                                        bool enabled);
bool bx_tar_create_options_set_ignore_case(struct bx_tar_create_options* options,
                                           bool enabled);
bool bx_tar_create_options_set_wildcards(struct bx_tar_create_options* options,
                                         bool enabled);
bool bx_tar_create_options_set_wildcards_match_slash(struct bx_tar_create_options* options,
                                                     bool enabled);
bool bx_tar_create_options_set_exclude_caches(struct bx_tar_create_options* options);
bool bx_tar_create_options_set_exclude_caches_all(struct bx_tar_create_options* options);
bool bx_tar_create_options_set_exclude_caches_under(struct bx_tar_create_options* options);
bool bx_tar_create_options_add_exclude_ignore(struct bx_tar_create_options* options,
                                              const char* path);
bool bx_tar_create_options_add_exclude_ignore_recursive(struct bx_tar_create_options* options,
                                                        const char* path);
bool bx_tar_create_options_add_exclude_tag(struct bx_tar_create_options* options,
                                           const char* path);
bool bx_tar_create_options_add_exclude_tag_all(struct bx_tar_create_options* options,
                                               const char* path);
bool bx_tar_create_options_add_exclude_tag_under(struct bx_tar_create_options* options,
                                                 const char* path);
bool bx_tar_create_options_set_exclude_vcs(struct bx_tar_create_options* options);
bool bx_tar_create_options_set_exclude_vcs_ignores(struct bx_tar_create_options* options);

bool bx_tar_create_collect_fs_entries(struct bx_archive_fs_list* list,
                                      const struct bx_tar_create_options* create_options,
                                      bool sort_children,
                                      bool* had_create_errors,
                                      struct bx_diag_ctx* diag);

bool bx_tar_create_remove_archived_sources(const struct bx_archive_fs_list* list,
                                           const struct bx_diag_ctx* diag);

#endif /* BX_APPLETS_ARCHIVE_TAR_TAR_CREATE_H */
