#define _GNU_SOURCE

#include "extract_path.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "lib/fd_ops.h"
#include "lib/path_ops.h"
#include "ziputils.h"

bool zu_extract_path_is_safe(const char* name) {
    return name != NULL
        && !bx_path_is_absolute(name)
        && !bx_path_has_parent_reference(name);
}

int zu_extract_ensure_dir(const char* path) {
    struct stat st;
    if (bx_fd_fstatat_nofollow(AT_FDCWD, path, &st) == 0)
        return S_ISDIR(st.st_mode) ? ZU_STATUS_OK : ZU_STATUS_USAGE;
    if (bx_fd_mkdirat(AT_FDCWD, path, 0755) == 0)
        return ZU_STATUS_OK;
    if (errno == EEXIST
        && bx_fd_fstatat_nofollow(AT_FDCWD, path, &st) == 0
        && S_ISDIR(st.st_mode)) {
        return ZU_STATUS_OK;
    }
    return ZU_STATUS_IO;
}

int zu_extract_ensure_parent_dirs(const char* path) {
    char* parent = bx_path_parent_dir_dup(path);
    if (!parent)
        return ZU_STATUS_OOM;
    if (strcmp(parent, ".") == 0 || strcmp(parent, "/") == 0) {
        free(parent);
        return ZU_STATUS_OK;
    }

    for (char* cursor = parent + (parent[0] == '/' ? 1 : 0);
         *cursor != '\0';
         cursor++) {
        if (*cursor != '/')
            continue;
        *cursor = '\0';
        int rc = zu_extract_ensure_dir(parent);
        *cursor = '/';
        if (rc != ZU_STATUS_OK) {
            free(parent);
            return rc == ZU_STATUS_OOM ? rc : ZU_STATUS_IO;
        }
    }

    int rc = zu_extract_ensure_dir(parent);
    free(parent);
    return rc;
}

char* zu_extract_build_output_path(const ZContext* ctx, const char* name) {
    if (!ctx || !name)
        return NULL;
    const char* name_part =
        ctx->store_paths ? name : bx_path_basename_ptr(name);
    if (!ctx->target_dir || ctx->target_dir[0] == '\0')
        return strdup(name_part);
    return bx_path_join(ctx->target_dir, name_part);
}
