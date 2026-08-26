#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "bx/libbx.h"
#include "lib/path_ops.h"
#include "lib/preload_ops.h"

static bool bx_preload_path_exists(const char *path) {
    struct stat stat_buffer;
    return stat(path, &stat_buffer) == 0;
}

char *bx_preload_find_runtime_module(
    const char *module_name,
    const char *install_subdir,
    const char *installed_path
) {
    char *executable = bx_path_readlink_dup("/proc/self/exe");
    if (executable != NULL) {
        char *directory = bx_path_dirname_dup(executable);
        char *candidate = bx_path_join(directory, module_name);
        if (bx_preload_path_exists(candidate)) {
            free(directory);
            free(executable);
            return candidate;
        }
        free(candidate);

        if (install_subdir != NULL && install_subdir[0] != '\0') {
            char *prefix = bx_path_parent_dir_dup(directory);
            char *module_directory = bx_path_join(prefix, install_subdir);
            candidate = bx_path_join(module_directory, module_name);
            free(module_directory);
            free(prefix);
            if (bx_preload_path_exists(candidate)) {
                free(directory);
                free(executable);
                return candidate;
            }
            free(candidate);
        }
        free(directory);
        free(executable);
    }

    if (installed_path != NULL && installed_path[0] != '\0' &&
        bx_preload_path_exists(installed_path)) {
        return xstrdup(installed_path);
    }
    return NULL;
}

int bx_preload_append_environment(
    const char *environment_name,
    const char *module_path,
    char **value_out
) {
    const char *old_value = getenv(environment_name);
    size_t old_len = old_value != NULL ? strlen(old_value) : 0u;
    size_t module_len = strlen(module_path);
    size_t total = old_len + (old_value != NULL ? 1u : 0u) + module_len + 1u;
    char *value = xmalloc(total);

    size_t offset = 0u;
    if (old_value != NULL) {
        memcpy(value, old_value, old_len);
        offset = old_len;
        value[offset++] = ':';
    }
    memcpy(value + offset, module_path, module_len + 1u);

    if (setenv(environment_name, value, 1) != 0) {
        int error = errno;
        *value_out = value;
        return error;
    }

    *value_out = value;
    return 0;
}
