#ifndef BX_APPLETS_ARCHIVE_TAR_TAR_PATTERNS_H
#define BX_APPLETS_ARCHIVE_TAR_TAR_PATTERNS_H

#include <stdbool.h>

#include "applets/archive/archive_common.h"

bool bx_tar_match_exclude_pattern(const char* pattern,
                                  const char* archive_path);

bool bx_tar_path_excluded(const struct bx_archive_name_list* patterns,
                          const char* archive_path);

#endif /* BX_APPLETS_ARCHIVE_TAR_TAR_PATTERNS_H */
