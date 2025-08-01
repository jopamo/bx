#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#include "args_common.h"
#include "copy_metadata.h"
#include "update_policy.h"
#include "libbx.h"

bool bx_args_parse_preserve_list(const char* arg, unsigned* mask, bool set_bits, bool* mode_mentioned_out, char** invalid_token_out) {
    char* copy = xstrdup(arg);
    char* saveptr = NULL;
    bool mode_mentioned = false;

    if (invalid_token_out) {
        *invalid_token_out = NULL;
    }

    for (char* token = strtok_r(copy, ",", &saveptr); token != NULL; token = strtok_r(NULL, ",", &saveptr)) {
        unsigned bits = 0;

        if (strcmp(token, "mode") == 0) {
            bits = BX_PRESERVE_MODE;
            mode_mentioned = true;
        }
        else if (strcmp(token, "ownership") == 0) {
            bits = BX_PRESERVE_OWNERSHIP;
        }
        else if (strcmp(token, "timestamps") == 0) {
            bits = BX_PRESERVE_TIMESTAMPS;
        }
        else if (strcmp(token, "links") == 0) {
            bits = BX_PRESERVE_LINKS;
        }
        else if (strcmp(token, "all") == 0) {
            bits = BX_PRESERVE_ALL;
            mode_mentioned = true;
        }
        else if (strcmp(token, "xattr") == 0) {
            bits = BX_PRESERVE_XATTR;
        }
        else {
            if (invalid_token_out) {
                *invalid_token_out = xstrdup(token);
            }
            free(copy);
            return false;
        }

        if (set_bits) {
            *mask |= bits;
        }
        else {
            *mask &= ~bits;
        }
    }

    free(copy);
    if (mode_mentioned_out != NULL) {
        *mode_mentioned_out = mode_mentioned;
    }
    return true;
}

bool bx_args_parse_update_mode(const char* arg, enum bx_update_mode* mode_out) {
    if (arg == NULL || strcmp(arg, "older") == 0) {
        *mode_out = BX_UPDATE_OLDER;
        return true;
    }
    if (strcmp(arg, "all") == 0) {
        *mode_out = BX_UPDATE_ALL;
        return true;
    }
    if (strcmp(arg, "none") == 0) {
        *mode_out = BX_UPDATE_NONE;
        return true;
    }
    if (strcmp(arg, "none-fail") == 0) {
        *mode_out = BX_UPDATE_NONE_FAIL;
        return true;
    }

    return false;
}

bool bx_args_parse_backup_mode(const char* arg, enum bx_backup_mode* mode_out) {
    if (arg == NULL) {
        return false;
    }
    if (strcmp(arg, "none") == 0 || strcmp(arg, "off") == 0) {
        *mode_out = BX_BACKUP_OFF;
        return true;
    }
    if (strcmp(arg, "numbered") == 0 || strcmp(arg, "t") == 0) {
        *mode_out = BX_BACKUP_NUMBERED;
        return true;
    }
    if (strcmp(arg, "existing") == 0 || strcmp(arg, "nil") == 0) {
        *mode_out = BX_BACKUP_EXISTING;
        return true;
    }
    if (strcmp(arg, "simple") == 0 || strcmp(arg, "never") == 0) {
        *mode_out = BX_BACKUP_SIMPLE;
        return true;
    }
    return false;
}

void bx_args_enable_backup_mode(enum bx_backup_mode* mode) {
    if (*mode == BX_BACKUP_NONE) {
        *mode = BX_BACKUP_UNSPECIFIED;
    }
}

bool bx_args_backup_mode_requested(enum bx_backup_mode mode) {
    return mode != BX_BACKUP_NONE;
}

bool bx_args_backup_mode_enabled(enum bx_backup_mode mode) {
    return mode != BX_BACKUP_NONE && mode != BX_BACKUP_OFF;
}
