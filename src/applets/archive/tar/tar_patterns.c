#include <fnmatch.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "applets/archive/tar/tar_patterns.h"
#include "lib/path_ops.h"

bool bx_tar_match_exclude_pattern(const char* pattern,
                                  const char* archive_path) {
    int flags = FNM_PATHNAME;
    const char* cursor;

    if (strchr(pattern, '/') == NULL) {
        return fnmatch(pattern, bx_path_basename_ptr(archive_path), 0) == 0;
    }

    if (fnmatch(pattern, archive_path, flags) == 0) {
        return true;
    }

    for (cursor = archive_path; *cursor != '\0'; cursor++) {
        if (*cursor == '/' && fnmatch(pattern, cursor + 1, flags) == 0) {
            return true;
        }
    }

    return false;
}

bool bx_tar_path_excluded(const struct bx_archive_name_list* patterns,
                          const char* archive_path) {
    size_t i;

    for (i = 0u; i < patterns->len; i++) {
        if (bx_tar_match_exclude_pattern(patterns->items[i], archive_path)) {
            return true;
        }
    }
    return false;
}
