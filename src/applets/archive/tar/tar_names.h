#ifndef BX_APPLETS_ARCHIVE_TAR_NAMES_H
#define BX_APPLETS_ARCHIVE_TAR_NAMES_H

#include <regex.h>
#include <stdbool.h>
#include <stddef.h>

#include "bx/diag.h"

struct bx_tar_transform_rule {
    bool active;
    bool global;
    regex_t regex;
    char* replacement;
};

struct bx_tar_name_policy {
    bool absolute_names;
    size_t strip_components;
    const char* one_top_level;
    const struct bx_tar_transform_rule* transform;
};

struct bx_tar_mapped_name {
    const char* text;
    char* owned;
};

bool bx_tar_transform_rule_init(struct bx_tar_transform_rule* rule,
                                const char* spec,
                                struct bx_diag_ctx* diag);
void bx_tar_transform_rule_cleanup(struct bx_tar_transform_rule* rule);

struct bx_tar_mapped_name bx_tar_map_member_name(const char* stored_name,
                                                 const struct bx_tar_name_policy* policy,
                                                 bool* stripped_absolute,
                                                 bool* stripped_dotdot);

#endif /* BX_APPLETS_ARCHIVE_TAR_NAMES_H */
