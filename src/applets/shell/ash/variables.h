#ifndef BX_APPLETS_SHELL_ASH_VARIABLES_H
#define BX_APPLETS_SHELL_ASH_VARIABLES_H

#include <stdbool.h>
#include <stddef.h>

struct ash_shell;

struct ash_var {
    char* name;
    char* value;
    bool exported;
    struct ash_var* next;
};

struct ash_var_saved {
    char* name;
    struct ash_var* original;
    bool existed;
};

bool ash_is_name_start(unsigned char ch);
bool ash_is_name_char(unsigned char ch);
bool ash_is_valid_name_span(const char* text, size_t length);
bool ash_parse_assignment(
    const char* text,
    size_t* name_length,
    const char** value
);

const char* ash_var_get_len(
    const struct ash_shell* shell,
    const char* name,
    size_t length
);
const char* ash_var_get(const struct ash_shell* shell, const char* name);
bool ash_var_exists(const struct ash_shell* shell, const char* name);
bool ash_var_set_with_export(
    struct ash_shell* shell,
    const char* name,
    size_t name_length,
    const char* value,
    bool mark_export
);
bool ash_var_set(
    struct ash_shell* shell,
    const char* name,
    const char* value,
    bool mark_export
);
bool ash_var_export(struct ash_shell* shell, const char* name);
bool ash_var_save(
    struct ash_shell* shell,
    const char* name,
    size_t length,
    struct ash_var_saved* saved
);
void ash_var_restore(struct ash_shell* shell, struct ash_var_saved* saved);
void ash_var_unset(struct ash_shell* shell, const char* name);
void ash_vars_destroy(struct ash_shell* shell);
bool ash_import_environment(struct ash_shell* shell);

#endif /* BX_APPLETS_SHELL_ASH_VARIABLES_H */
