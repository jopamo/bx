#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>
#include <limits.h>
#include <libgen.h>
#include <fcntl.h>

#include "backup_ops.h"
#include "args_common.h"
#include "diag.h"
#include "libbx.h"
#include "path_ops.h"
#include "copy_data.h"

void bx_backup_get_params(enum bx_backup_mode cmd_mode,
                          const char *cmd_suffix,
                          struct bx_backup_params *out) {
    if (cmd_mode == BX_BACKUP_UNSPECIFIED) {
        const char *vc = getenv("VERSION_CONTROL");
        if (vc) {
            if (!bx_args_parse_backup_mode(vc, &cmd_mode)) {
                cmd_mode = BX_BACKUP_EXISTING;
            }
        } else {
            cmd_mode = BX_BACKUP_EXISTING;
        }
    }

    out->mode = cmd_mode;

    if (cmd_suffix) {
        out->suffix = cmd_suffix;
    } else {
        const char *sbs = getenv("SIMPLE_BACKUP_SUFFIX");
        out->suffix = sbs ? sbs : "~";
    }
}

static int get_max_backup_version(const char *dir, const char *base) {
    DIR *d = opendir(dir);
    if (!d) return 0;

    int max_v = 0;
    size_t base_len = strlen(base);
    struct dirent *de;

    while ((de = readdir(d)) != NULL) {
        if (strncmp(de->d_name, base, base_len) == 0 &&
            de->d_name[base_len] == '.' &&
            de->d_name[base_len + 1] == '~') {
            
            char *endptr;
            long v = strtol(de->d_name + base_len + 2, &endptr, 10);
            if (*endptr == '~' && endptr[1] == '\0') {
                if (v > max_v) max_v = (int)v;
            }
        }
    }
    closedir(d);
    return max_v;
}

static char *get_backup_path(const char *path, const struct bx_backup_params *params, enum bx_backup_mode *effective_mode_out) {
    enum bx_backup_mode effective_mode = params->mode;
    if (effective_mode == BX_BACKUP_EXISTING) {
        char *dir_copy = xstrdup(path);
        char *dname = dirname(dir_copy);
        char *base = bx_path_basename_dup(path);
        if (get_max_backup_version(dname, base) > 0) {
            effective_mode = BX_BACKUP_NUMBERED;
        } else {
            effective_mode = BX_BACKUP_SIMPLE;
        }
        free(base);
        free(dir_copy);
    }

    if (effective_mode_out) *effective_mode_out = effective_mode;

    if (effective_mode == BX_BACKUP_SIMPLE) {
        char *res = xmalloc(strlen(path) + strlen(params->suffix) + 1);
        sprintf(res, "%s%s", path, params->suffix);
        return res;
    } else if (effective_mode == BX_BACKUP_NUMBERED) {
        char *dir_copy = xstrdup(path);
        char *dname = dirname(dir_copy);
        char *base = bx_path_basename_dup(path);
        int next_v = get_max_backup_version(dname, base) + 1;
        char *res = xmalloc(strlen(path) + 16);
        sprintf(res, "%s.~%d~", path, next_v);
        free(base);
        free(dir_copy);
        return res;
    }
    return NULL;
}

static void bx_backup_diag_errno(struct bx_diag_ctx *diag, const char *path, int err) {
    bx_diag(diag, "cannot backup '%s': %s", path, strerror(err));
}

enum bx_backup_create_result bx_backup_create(const char *path,
                                              const struct bx_backup_params *params,
                                              struct bx_diag_ctx *diag,
                                              char **backup_path_out) {
    if (backup_path_out) {
        *backup_path_out = NULL;
    }

    if (params->mode == BX_BACKUP_NONE || params->mode == BX_BACKUP_OFF) {
        return BX_BACKUP_CREATE_SKIPPED;
    }

    struct stat st;
    if (lstat(path, &st) != 0) {
        if (errno == ENOENT || errno == ENOTDIR) {
            return BX_BACKUP_CREATE_SKIPPED;
        }
        bx_backup_diag_errno(diag, path, errno);
        return BX_BACKUP_CREATE_FAILED;
    }
    (void)st;

    char *backup_path = get_backup_path(path, params, NULL);
    if (backup_path == NULL) {
        bx_diag(diag, "cannot backup '%s': unsupported backup mode", path);
        return BX_BACKUP_CREATE_FAILED;
    }

    if (rename(path, backup_path) != 0) {
        int err = errno;
        free(backup_path);
        bx_backup_diag_errno(diag, path, err);
        return BX_BACKUP_CREATE_FAILED;
    }

    if (backup_path_out) {
        *backup_path_out = backup_path;
    } else {
        free(backup_path);
    }
    return BX_BACKUP_CREATE_CREATED;
}

enum bx_backup_create_result bx_backup_create_copy(const char *path,
                                                   const struct bx_backup_params *params,
                                                   struct bx_diag_ctx *diag,
                                                   char **backup_path_out) {
    if (backup_path_out) {
        *backup_path_out = NULL;
    }

    if (params->mode == BX_BACKUP_NONE || params->mode == BX_BACKUP_OFF) {
        return BX_BACKUP_CREATE_SKIPPED;
    }

    struct stat st;
    if (lstat(path, &st) != 0) {
        if (errno == ENOENT || errno == ENOTDIR) {
            return BX_BACKUP_CREATE_SKIPPED;
        }
        bx_backup_diag_errno(diag, path, errno);
        return BX_BACKUP_CREATE_FAILED;
    }

    char *backup_path = get_backup_path(path, params, NULL);
    if (backup_path == NULL) {
        bx_diag(diag, "cannot backup '%s': unsupported backup mode", path);
        return BX_BACKUP_CREATE_FAILED;
    }

    int src_fd = open(path, O_RDONLY);
    if (src_fd < 0) {
        int err = errno;
        free(backup_path);
        bx_backup_diag_errno(diag, path, err);
        return BX_BACKUP_CREATE_FAILED;
    }

    int dest_fd = open(backup_path, O_WRONLY | O_CREAT | O_EXCL, st.st_mode & 0777u);
    if (dest_fd < 0) {
        int err = errno;
        close(src_fd);
        free(backup_path);
        bx_backup_diag_errno(diag, path, err);
        return BX_BACKUP_CREATE_FAILED;
    }

    struct bx_copy_data_options data_opts = {BX_SPARSE_NEVER, BX_REFLINK_NEVER};
    if (bx_copy_data(src_fd, dest_fd, &data_opts) != BX_COPY_DATA_SUCCESS) {
        int err = errno != 0 ? errno : EIO;
        close(src_fd);
        close(dest_fd);
        unlink(backup_path);
        free(backup_path);
        bx_backup_diag_errno(diag, path, err);
        return BX_BACKUP_CREATE_FAILED;
    }

    if (close(src_fd) != 0) {
        int err = errno;
        close(dest_fd);
        unlink(backup_path);
        free(backup_path);
        bx_backup_diag_errno(diag, path, err);
        return BX_BACKUP_CREATE_FAILED;
    }
    if (close(dest_fd) != 0) {
        int err = errno;
        unlink(backup_path);
        free(backup_path);
        bx_backup_diag_errno(diag, path, err);
        return BX_BACKUP_CREATE_FAILED;
    }

    if (backup_path_out) {
        *backup_path_out = backup_path;
    } else {
        free(backup_path);
    }
    return BX_BACKUP_CREATE_CREATED;
}
