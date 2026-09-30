#ifndef BX_APPLETS_ARCHIVE_TAR_NAMES_H
#define BX_APPLETS_ARCHIVE_TAR_NAMES_H

#include <stdbool.h>
#include <stddef.h>

struct bx_tar_name_policy {
    bool absolute_names;
    size_t strip_components;
    const char* one_top_level;
};

struct bx_tar_mapped_name {
    const char* text;
    char* owned;
};

struct bx_tar_mapped_name bx_tar_map_member_name(const char* stored_name,
                                                 const struct bx_tar_name_policy* policy,
                                                 bool* stripped_absolute,
                                                 bool* stripped_dotdot);

#endif /* BX_APPLETS_ARCHIVE_TAR_NAMES_H */
