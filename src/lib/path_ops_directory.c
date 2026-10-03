#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "lib/path_ops.h"

static bool bx_path_check_directory(const char* path) {
    struct stat metadata;
    if (stat(path, &metadata) != 0) {
        return false;
    }
    if (!S_ISDIR(metadata.st_mode)) {
        errno = ENOTDIR;
        return false;
    }
    return true;
}

char* bx_path_normalize_directory_dup(const char* path) {
    if (path == NULL || path[0] != '/') {
        errno = EINVAL;
        return NULL;
    }
    char* result = strdup(path);
    if (result == NULL) {
        return NULL;
    }
    size_t root = path[1] == '/' && path[2] != '/' ? 2u : 1u;
    size_t written = root;
    const char* cursor = path + root;
    bool checked = false;
    while (*cursor != '\0') {
        while (*cursor == '/') {
            cursor++;
        }
        const char* component = cursor;
        while (*cursor != '\0' && *cursor != '/') {
            cursor++;
        }
        size_t length = (size_t)(cursor - component);
        if (length == 0u || (length == 1u && component[0] == '.')) {
            continue;
        }
        if (length == 2u && component[0] == '.' && component[1] == '.') {
            while (written > root && result[written - 1u] != '/') {
                written--;
            }
            if (written > root) {
                written--;
            }
        }
        else {
            if (written > root) {
                result[written++] = '/';
            }
            memcpy(result + written, component, length);
            written += length;
        }
        result[written] = '\0';
        if (!bx_path_check_directory(result)) {
            int error = errno;
            free(result);
            errno = error;
            return NULL;
        }
        checked = true;
    }
    result[written] = '\0';
    if (!checked && !bx_path_check_directory(result)) {
        int error = errno;
        free(result);
        errno = error;
        return NULL;
    }
    return result;
}
