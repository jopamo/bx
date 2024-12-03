#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "remove_ops.h"
#include "path_ops.h"
#include "diag.h"

static bool bx_remove_recursive_impl(const char* path, bool one_file_system, dev_t root_dev, bool top_level, struct bx_diag_ctx* diag) {
    struct stat st;
    if (lstat(path, &st) != 0) {
        if (errno == ENOENT) {
            return true;
        }
        bx_perror_path(diag, path);
        return false;
    }

    if (!top_level && one_file_system && S_ISDIR(st.st_mode) && st.st_dev != root_dev) {
        return true;
    }

    if (!S_ISDIR(st.st_mode)) {
        if (unlink(path) != 0) {
            bx_perror_path(diag, path);
            return false;
        }
        return true;
    }

    DIR* dir = opendir(path);
    if (dir == NULL) {
        bx_perror_path(diag, path);
        return false;
    }

    bool ok = true;
    for (;;) {
        errno = 0;
        struct dirent* entry = readdir(dir);
        if (entry == NULL) {
            if (errno != 0) {
                bx_perror_path(diag, path);
                ok = false;
            }
            break;
        }
        if (bx_path_is_dot_or_dotdot(entry->d_name)) {
            continue;
        }

        char* child_path = bx_path_join(path, entry->d_name);
        if (!bx_remove_recursive_impl(child_path, one_file_system, root_dev, false, diag)) {
            ok = false;
        }
        free(child_path);
    }

    if (closedir(dir) != 0) {
        bx_perror_path(diag, path);
        ok = false;
    }

    if (ok) {
        if (rmdir(path) != 0) {
            bx_perror_path(diag, path);
            ok = false;
        }
    }

    return ok;
}

bool bx_remove_recursive(const char* path, struct bx_diag_ctx* diag) {
    return bx_remove_recursive_impl(path, false, 0, true, diag);
}

bool bx_remove_recursive_one_file_system(const char* path, dev_t root_dev, struct bx_diag_ctx* diag) {
    return bx_remove_recursive_impl(path, true, root_dev, true, diag);
}
