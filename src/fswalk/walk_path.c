#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "walk_internal.h"

char *bx_walk_path_join(const char *dirpath, const char *name, int *err_out) {
    if (err_out)
        *err_out = 0;

    if (!dirpath || !name) {
        if (err_out)
            *err_out = EINVAL;
        return NULL;
    }

    size_t dir_len = strlen(dirpath);
    size_t name_len = strlen(name);
    if (dir_len > SIZE_MAX - name_len - 2u) {
        if (err_out)
            *err_out = ENAMETOOLONG;
        return NULL;
    }

    size_t full_len = dir_len + 1u + name_len + 1u;
    char *full = malloc(full_len);
    if (!full) {
        if (err_out)
            *err_out = ENOMEM;
        return NULL;
    }

    memcpy(full, dirpath, dir_len);
    full[dir_len] = '/';
    memcpy(full + dir_len + 1u, name, name_len);
    full[full_len - 1u] = '\0';
    return full;
}
