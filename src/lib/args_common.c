#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "args_common.h"
#include "copy_metadata.h"
#include "update_policy.h"
#include "bx/libbx.h"

void bx_args_getopt_reset_at(int first_option_index) {
    opterr = 0;
    optind = first_option_index;
}

void bx_args_getopt_reset(void) {
    bx_args_getopt_reset_at(1);
}

int bx_args_getopt_long(
    int argc,
    char* const argv[],
    const char* short_options,
    const struct option* long_options,
    int* long_index
) {
    return getopt_long(argc, argv, short_options, long_options, long_index);
}

bool bx_args_parse_int_range(const char* arg, int min_value, int max_value, int* value_out) {
    if (arg == NULL || arg[0] == '\0' || value_out == NULL || min_value > max_value) {
        return false;
    }

    errno = 0;
    char* end = NULL;
    intmax_t value = strtoimax(arg, &end, 10);
    if (errno == ERANGE || end == arg || end == NULL || end[0] != '\0') {
        return false;
    }
    if (value < (intmax_t)min_value || value > (intmax_t)max_value) {
        return false;
    }

    *value_out = (int)value;
    return true;
}

bool bx_args_parse_llong_range(const char* arg, long long min_value, long long max_value, long long* value_out) {
    if (arg == NULL || arg[0] == '\0' || value_out == NULL || min_value > max_value) {
        return false;
    }

    errno = 0;
    char* end = NULL;
    intmax_t value = strtoimax(arg, &end, 10);
    if (errno == ERANGE || end == arg || end == NULL || end[0] != '\0') {
        return false;
    }
    if (value < (intmax_t)min_value || value > (intmax_t)max_value) {
        return false;
    }

    *value_out = (long long)value;
    return true;
}

bool bx_args_parse_size_range(const char* arg, size_t min_value, size_t max_value, size_t* value_out) {
    if (arg == NULL || arg[0] == '\0' || arg[0] == '-' || value_out == NULL || min_value > max_value) {
        return false;
    }

    errno = 0;
    char* end = NULL;
    uintmax_t value = strtoumax(arg, &end, 10);
    if (errno == ERANGE || end == arg || end == NULL || end[0] != '\0') {
        return false;
    }
    if (value < (uintmax_t)min_value || value > (uintmax_t)max_value) {
        return false;
    }

    *value_out = (size_t)value;
    return true;
}

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
