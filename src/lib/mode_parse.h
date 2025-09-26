#ifndef BX_COMMON_MODE_PARSE_H
#define BX_COMMON_MODE_PARSE_H

#include <stdbool.h>
#include <sys/stat.h>

enum bx_mode_x_policy {
    BX_MODE_X_DISABLED = 0,
    BX_MODE_X_ALWAYS,
    BX_MODE_X_IF_ANY_EXEC,
    BX_MODE_X_IF_DIRECTORY_OR_ANY_EXEC,
};

struct bx_mode_parse_params {
    mode_t initial_mode;
    mode_t result_mask;
    mode_t max_numeric_mode;
    mode_t umask_value;
    mode_t sticky_bit;
    enum bx_mode_x_policy x_policy;
    bool is_directory;
    bool apply_umask_when_who_omitted;
    bool allow_setuid;
    bool allow_setgid;
    bool allow_sticky;
};

mode_t bx_mode_current_umask(void);
bool bx_mode_parse_numeric(const char* text, mode_t max_mode, mode_t* mode_out);
bool bx_mode_parse_symbolic(const char* text, const struct bx_mode_parse_params* params, mode_t* mode_out);
bool bx_mode_parse(const char* text, const struct bx_mode_parse_params* params, mode_t* mode_out);

#endif /* BX_COMMON_MODE_PARSE_H */
