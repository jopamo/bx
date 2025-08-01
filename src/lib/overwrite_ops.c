#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "overwrite_ops.h"
#include "libbx.h"
#include "prompt_ops.h"

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

bool bx_overwrite_backup_existing(const char* dest_path, const struct bx_backup_params* backup_params, struct bx_diag_ctx* diag, struct bx_dest_state* dest_state, char** backup_path_out) {
    char* backup_file = NULL;

    if (backup_path_out != NULL) {
        *backup_path_out = NULL;
    }

    enum bx_backup_create_result result = bx_backup_create(dest_path, backup_params, diag, &backup_file);

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
