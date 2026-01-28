#ifndef BX_APPLETS_ARCHIVE_TAR_TAR_SELECT_H
#define BX_APPLETS_ARCHIVE_TAR_TAR_SELECT_H

#include <stdbool.h>
#include <stddef.h>

#include "applets/archive/archive_common.h"
#include "applets/archive/tar/tar_create.h"
#include "applets/archive/tar/tar_patterns.h"

struct bx_tar_select_member {
    char* name;
    char* extract_dir;
    struct bx_tar_match_policy policy;
    bool recurse;
};

struct bx_tar_select_plan {
    struct bx_tar_select_member* members;
    size_t len;
    size_t cap;
    struct bx_tar_match_pattern_list exclude_patterns;
    char* default_extract_dir;
};

void bx_tar_select_plan_cleanup(struct bx_tar_select_plan* plan);

bool bx_tar_select_plan_build(struct bx_tar_select_plan* plan,
                              const struct bx_tar_create_options* directives,
                              bool* had_errors,
                              struct bx_diag_ctx* diag);

bool bx_tar_select_member_matches_name(const struct bx_tar_select_member* member,
                                       const char* name);

bool bx_tar_select_plan_match(const struct bx_tar_select_plan* plan,
                              const char* name,
                              bool default_select_all,
                              bool* matched_members,
                              const char** extract_dir_out);

bool bx_tar_select_plan_report_unmatched(const struct bx_tar_select_plan* plan,
                                         const bool* matched_members,
                                         const struct bx_diag_ctx* diag);

#endif /* BX_APPLETS_ARCHIVE_TAR_TAR_SELECT_H */
