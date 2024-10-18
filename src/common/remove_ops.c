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

bool bx_remove_recursive(const char *path, struct bx_diag_ctx *diag) {
    struct stat st;
    if (lstat(path, &st) != 0) {
        if (errno == ENOENT) {
            return true;
        }
        bx_perror_path(diag, path);
        return false;
    }

    if (!S_ISDIR(st.st_mode)) {
        if (unlink(path) != 0) {
            bx_perror_path(diag, path);
            return false;
        }
        return true;
    }

    DIR *dir = opendir(path);
    if (dir == NULL) {
        bx_perror_path(diag, path);
        return false;
    }

    bool ok = true;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(dir);
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

        char *child_path = bx_path_join(path, entry->d_name);
        if (!bx_remove_recursive(child_path, diag)) {
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
