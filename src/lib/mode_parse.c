#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>

#include "lib/mode_parse.h"

enum {
    BX_MODE_WHO_U = 1u << 0,
    BX_MODE_WHO_G = 1u << 1,
    BX_MODE_WHO_O = 1u << 2,
    BX_MODE_WHO_ALL = BX_MODE_WHO_U | BX_MODE_WHO_G | BX_MODE_WHO_O,
};

static bool bx_mode_is_octal_string(const char* text) {
    if (text == NULL || text[0] == '\0') {
        return false;
    }

    for (const char* p = text; *p != '\0'; p++) {
        if (*p < '0' || *p > '7') {
            return false;
        }
    }

    return true;
}

static mode_t bx_mode_rwx_mask_from_who(unsigned int who_flags) {
    mode_t mask = 0u;

    if ((who_flags & BX_MODE_WHO_U) != 0u) {
        mask |= S_IRWXU;
    }
    if ((who_flags & BX_MODE_WHO_G) != 0u) {
        mask |= S_IRWXG;
    }
    if ((who_flags & BX_MODE_WHO_O) != 0u) {
        mask |= S_IRWXO;
    }

    return mask;
}

static mode_t bx_mode_special_mask_from_who(const struct bx_mode_parse_params* params, unsigned int who_flags) {
    mode_t mask = 0u;

    if (params->allow_setuid && (who_flags & BX_MODE_WHO_U) != 0u) {
        mask |= S_ISUID;
    }
    if (params->allow_setgid && (who_flags & BX_MODE_WHO_G) != 0u) {
        mask |= S_ISGID;
    }
    if (params->allow_sticky && (who_flags & BX_MODE_WHO_O) != 0u) {
        mask |= params->sticky_bit;
    }

    return mask;
}

static mode_t bx_mode_full_special_mask(const struct bx_mode_parse_params* params) {
    return bx_mode_special_mask_from_who(params, BX_MODE_WHO_ALL);
}

static mode_t bx_mode_perm_bits_for_who(unsigned int who_flags, char perm) {
    mode_t bits = 0u;

    if (perm == 'r') {
        if ((who_flags & BX_MODE_WHO_U) != 0u) {
            bits |= S_IRUSR;
        }
        if ((who_flags & BX_MODE_WHO_G) != 0u) {
            bits |= S_IRGRP;
        }
        if ((who_flags & BX_MODE_WHO_O) != 0u) {
            bits |= S_IROTH;
        }
    }
    else if (perm == 'w') {
        if ((who_flags & BX_MODE_WHO_U) != 0u) {
            bits |= S_IWUSR;
        }
        if ((who_flags & BX_MODE_WHO_G) != 0u) {
            bits |= S_IWGRP;
        }
        if ((who_flags & BX_MODE_WHO_O) != 0u) {
            bits |= S_IWOTH;
        }
    }
    else if (perm == 'x') {
        if ((who_flags & BX_MODE_WHO_U) != 0u) {
            bits |= S_IXUSR;
        }
        if ((who_flags & BX_MODE_WHO_G) != 0u) {
            bits |= S_IXGRP;
        }
        if ((who_flags & BX_MODE_WHO_O) != 0u) {
            bits |= S_IXOTH;
        }
    }

    return bits;
}

static mode_t bx_mode_copy_perm_bits(mode_t mode, unsigned int who_flags, char source_class) {
    mode_t source = 0u;

    switch (source_class) {
        case 'u':
            source = (mode & S_IRWXU) >> 6;
            break;
        case 'g':
            source = (mode & S_IRWXG) >> 3;
            break;
        case 'o':
            source = mode & S_IRWXO;
            break;
        default:
            return 0u;
    }

    mode_t bits = 0u;
    if ((who_flags & BX_MODE_WHO_U) != 0u) {
        bits |= source << 6;
    }
    if ((who_flags & BX_MODE_WHO_G) != 0u) {
        bits |= source << 3;
    }
    if ((who_flags & BX_MODE_WHO_O) != 0u) {
        bits |= source;
    }

    return bits;
}

static bool bx_mode_should_apply_x(const struct bx_mode_parse_params* params, mode_t source_mode) {
    switch (params->x_policy) {
        case BX_MODE_X_DISABLED:
            return false;
        case BX_MODE_X_ALWAYS:
            return true;
        case BX_MODE_X_IF_ANY_EXEC:
            return (source_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0u;
        case BX_MODE_X_IF_DIRECTORY_OR_ANY_EXEC:
            return params->is_directory || (source_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0u;
    }

    return false;
}

mode_t bx_mode_current_umask(void) {
    mode_t current_umask = umask(0u);
    umask(current_umask);
    return current_umask;
}

bool bx_mode_parse_numeric(const char* text, mode_t max_mode, mode_t* mode_out) {
    errno = 0;
    char* end = NULL;
    unsigned long value = strtoul(text, &end, 8);
    if (errno == ERANGE || end == text || end == NULL || end[0] != '\0' || value > (unsigned long)max_mode) {
        return false;
    }

    *mode_out = (mode_t)value;
    return true;
}

bool bx_mode_parse_symbolic(const char* text, const struct bx_mode_parse_params* params, mode_t* mode_out) {
    if (text == NULL || text[0] == '\0') {
        return false;
    }

    mode_t mode = params->initial_mode & params->result_mask;
    const char* p = text;

    while (*p != '\0') {
        unsigned int who_flags = 0u;
        bool who_specified = false;

        while (*p == 'u' || *p == 'g' || *p == 'o' || *p == 'a') {
            who_specified = true;
            if (*p == 'u') {
                who_flags |= BX_MODE_WHO_U;
            }
            else if (*p == 'g') {
                who_flags |= BX_MODE_WHO_G;
            }
            else if (*p == 'o') {
                who_flags |= BX_MODE_WHO_O;
            }
            else {
                who_flags |= BX_MODE_WHO_ALL;
            }
            p++;
        }

        if (!who_specified) {
            who_flags = BX_MODE_WHO_ALL;
        }

        char op = *p;
        if (op != '+' && op != '-' && op != '=') {
            return false;
        }
        p++;

        if ((op != '=' && (*p == '\0' || *p == ',')) || *p == '+' || *p == '-' || *p == '=') {
            return false;
        }

        mode_t clause_rwx_bits = 0u;
        mode_t clause_special_bits = 0u;
        mode_t source_mode = mode;
        bool saw_token = false;

        while (*p != '\0' && *p != ',') {
            switch (*p) {
                case 'r':
                case 'w':
                case 'x':
                    clause_rwx_bits |= bx_mode_perm_bits_for_who(who_flags, *p);
                    saw_token = true;
                    break;
                case 'X':
                    if (params->x_policy == BX_MODE_X_DISABLED) {
                        return false;
                    }
                    if (bx_mode_should_apply_x(params, source_mode)) {
                        clause_rwx_bits |= bx_mode_perm_bits_for_who(who_flags, 'x');
                    }
                    saw_token = true;
                    break;
                case 's':
                    if (!params->allow_setuid && !params->allow_setgid) {
                        return false;
                    }
                    if (params->allow_setuid && (who_flags & BX_MODE_WHO_U) != 0u) {
                        clause_special_bits |= S_ISUID;
                    }
                    if (params->allow_setgid && (who_flags & BX_MODE_WHO_G) != 0u) {
                        clause_special_bits |= S_ISGID;
                    }
                    saw_token = true;
                    break;
                case 't':
                    if (!params->allow_sticky) {
                        return false;
                    }
                    if ((who_flags & BX_MODE_WHO_O) != 0u) {
                        clause_special_bits |= params->sticky_bit;
                    }
                    saw_token = true;
                    break;
                case 'u':
                case 'g':
                case 'o':
                    clause_rwx_bits |= bx_mode_copy_perm_bits(source_mode, who_flags, *p);
                    saw_token = true;
                    break;
                default:
                    return false;
            }
            p++;
        }

        if (!saw_token && op != '=') {
            return false;
        }

        mode_t affected_rwx_mask = bx_mode_rwx_mask_from_who(who_flags);
        if (!who_specified && params->apply_umask_when_who_omitted) {
            affected_rwx_mask &= (mode_t)(~params->umask_value) & 0777u;
        }

        mode_t clear_rwx_mask = affected_rwx_mask;
        if (op == '=' && !who_specified) {
            clear_rwx_mask = S_IRWXU | S_IRWXG | S_IRWXO;
        }

        mode_t affected_special_mask =
            who_specified ? bx_mode_special_mask_from_who(params, who_flags) : bx_mode_full_special_mask(params);
        mode_t applied_rwx_bits = clause_rwx_bits & affected_rwx_mask;

        if (op == '+') {
            mode |= applied_rwx_bits;
            mode |= clause_special_bits;
        }
        else if (op == '-') {
            mode &= ~applied_rwx_bits;
            mode &= ~clause_special_bits;
        }
        else {
            mode &= ~clear_rwx_mask;
            mode &= ~affected_special_mask;
            mode |= applied_rwx_bits;
            mode |= clause_special_bits;
        }

        if (*p == ',') {
            p++;
            if (*p == '\0' || *p == ',') {
                return false;
            }
        }
    }

    *mode_out = mode & params->result_mask;
    return true;
}

bool bx_mode_parse(const char* text, const struct bx_mode_parse_params* params, mode_t* mode_out) {
    if (text == NULL || text[0] == '\0') {
        return false;
    }

    if (bx_mode_is_octal_string(text)) {
        return bx_mode_parse_numeric(text, params->max_numeric_mode, mode_out);
    }

    return bx_mode_parse_symbolic(text, params, mode_out);
}
