#define _GNU_SOURCE

#include "publish.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lib/copy_data.h"
#include "lib/fd_ops.h"
#include "lib/path_ops.h"

char* zu_publish_make_temp_path(const char* temp_dir, const char* target_path) {
    const char* base = bx_path_basename_ptr(target_path);
    char* dir = temp_dir
        ? strdup(temp_dir)
        : bx_path_dirname_dup(target_path);
    if (!dir)
        return NULL;

    size_t base_len = strlen(base);
    if (base_len > SIZE_MAX - sizeof(".tmp")) {
        free(dir);
        return NULL;
    }
    char* temp_name = malloc(base_len + sizeof(".tmp"));
    if (!temp_name) {
        free(dir);
        return NULL;
    }
    memcpy(temp_name, base, base_len);
    memcpy(temp_name + base_len, ".tmp", sizeof(".tmp"));

    char* path = bx_path_join(dir, temp_name);
    free(temp_name);
    free(dir);
    return path;
}

int zu_publish_replace(const char* source_path, const char* target_path) {
    if (rename(source_path, target_path) == 0)
        return 0;
    if (errno != EXDEV)
        return -1;

    int source_fd = bx_fd_open_cloexec(source_path, O_RDONLY, 0);
    if (source_fd < 0)
        return -1;

    int target_fd = bx_fd_open_cloexec(
        target_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (target_fd < 0) {
        bx_fd_cleanup(&source_fd);
        return -1;
    }

    const struct bx_copy_data_options options = {
        .sparse_mode = BX_SPARSE_NEVER,
        .reflink_mode = BX_REFLINK_NEVER,
    };
    int rc = bx_copy_data(source_fd, target_fd, &options)
        == BX_COPY_DATA_SUCCESS
        ? 0
        : -1;
    if (!bx_fd_close(&source_fd, NULL, NULL)
        || !bx_fd_close(&target_fd, NULL, NULL)) {
        rc = -1;
    }
    if (rc == 0 && bx_fd_unlinkat(AT_FDCWD, source_path, 0) != 0)
        rc = -1;
    return rc;
}
