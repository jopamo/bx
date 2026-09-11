#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "overwrite_ops.h"
#include "bx/libbx.h"
#include "prompt_ops.h"
#include "path_ops.h"
#include "same_file.h"

struct bx_written_dest {
    struct stat parent;
    struct stat dest;
    char* name;
    struct bx_written_dest* next;
};

bool bx_overwrite_check_written(const struct bx_written_dest* written, const char* dest_path, const struct stat* dest_stat, const struct bx_backup_params* backup_params) {
    if (written == NULL || S_ISDIR(dest_stat->st_mode) || backup_params->mode == BX_BACKUP_NUMBERED) {
        return true;
    }
    for (const struct bx_written_dest* entry = written; entry != NULL; entry = entry->next) {
        if (!bx_same_file(&entry->dest, dest_stat)) {
            continue;
        }
        char* parent = bx_path_parent_dir_stripped_dup(dest_path);
        char* name = bx_path_basename_dup(dest_path);
        struct stat parent_stat;
        bool conflict = strcmp(name, entry->name) == 0 &&
                        (stat(parent, &parent_stat) != 0 || bx_same_file(&parent_stat, &entry->parent));
        free(parent);
        free(name);
        if (conflict) {
            return false;
        }
    }
    return true;
}

bool bx_overwrite_remember(struct bx_written_dest** written, const char* dest_path, const struct stat* expected, struct bx_diag_ctx* diag) {
    struct stat dest_stat;
    if (lstat(dest_path, &dest_stat) != 0) {
        bx_perror_path(diag, dest_path);
        return false;
    }
    /* An fd-authoritative copy may have finished after its path was replaced.
     * The replacement is not an entry that this invocation wrote. */
    if (S_ISDIR(dest_stat.st_mode) || (expected != NULL && !bx_same_file(expected, &dest_stat))) {
        return true;
    }
    char* parent = bx_path_parent_dir_stripped_dup(dest_path);
    struct stat parent_stat;
    int rc = stat(parent, &parent_stat);
    free(parent);
    if (rc != 0) {
        bx_perror_path(diag, dest_path);
        return false;
    }
    struct bx_written_dest* entry = xmalloc(sizeof(*entry));
    entry->parent = parent_stat;
    entry->dest = dest_stat;
    entry->name = bx_path_basename_dup(dest_path);
    entry->next = *written;
    *written = entry;
    return true;
}

void bx_overwrite_free_written(struct bx_written_dest** written) {
    while (*written != NULL) {
        struct bx_written_dest* entry = *written;
        *written = entry->next;
        free(entry->name);
        free(entry);
    }
}

bool bx_overwrite_should_skip(bool no_clobber,
                              bool interactive,
                              enum bx_update_mode update_mode,
                              const char* dest_path,
                              const struct stat* src_stat,
                              const struct stat* dest_stat,
                              bool* skip_out,
                              enum bx_overwrite_skip_reason* reason_out,
                              struct bx_diag_ctx* diag) {
    *skip_out = false;
    if (reason_out != NULL) {
        *reason_out = BX_OVERWRITE_SKIP_NONE;
    }

    if (no_clobber) {
        *skip_out = true;
        if (reason_out != NULL) {
            *reason_out = BX_OVERWRITE_SKIP_NO_CLOBBER;
        }
        return true;
    }

    bool error = false;
    if (!bx_update_should_skip(update_mode, src_stat, dest_stat, skip_out, &error)) {
        if (error) {
            bx_diag(diag, "will not overwrite '%s'", dest_path);
        }
        return false;
    }

    if (*skip_out && reason_out != NULL) {
        *reason_out = BX_OVERWRITE_SKIP_UPDATE;
    }

    if (interactive) {
        return true;
    }

    return true;
}

bool bx_overwrite_backup_existing(const char* source_path, const struct stat* source_stat, bool move_mode, const char* dest_path, const struct bx_backup_params* backup_params, struct bx_diag_ctx* diag, struct bx_dest_state* dest_state, char** backup_path_out) {
    char* backup_file = NULL;

    if (backup_path_out != NULL) {
        *backup_path_out = NULL;
    }

    enum bx_backup_create_result result = bx_backup_create(dest_path, backup_params, source_path, source_stat, diag, &backup_file);

    if (result == BX_BACKUP_CREATE_SOURCE_CONFLICT) {
        bx_diag(diag, "backing up '%s' might destroy source;  '%s' not %s", dest_path, source_path, move_mode ? "moved" : "copied");
        return false;
    }
    if (result == BX_BACKUP_CREATE_FAILED) {
        return false;
    }
    if (result == BX_BACKUP_CREATE_CREATED) {
        memset(dest_state, 0, sizeof(*dest_state));
        if (backup_path_out != NULL) {
            *backup_path_out = backup_file;
        }
        else {
            free(backup_file);
        }
    }
    return true;
}

bool bx_prompt_overwrite(const char* progname, const char* dest_path) {
    size_t prompt_len = strlen(progname) + strlen(dest_path) + sizeof(": overwrite ''? ");
    char* prompt = xmalloc(prompt_len);

    snprintf(prompt, prompt_len, "%s: overwrite '%s'? ", progname, dest_path);
    bool confirmed = bx_prompt_confirm(prompt);
    free(prompt);
    return confirmed;
}
