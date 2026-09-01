#ifndef BX_APPLETS_SHELL_ASH_VARIABLES_H
#define BX_APPLETS_SHELL_ASH_VARIABLES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "applets/shell/ash/variable_value.h"

struct ash_shell;

enum ash_var_attribute {
    ASH_VAR_ATTR_INTEGER = 1u << 0,
    ASH_VAR_ATTR_READONLY = 1u << 1,
    ASH_VAR_ATTR_EXPORT = 1u << 2,
    ASH_VAR_ATTR_NAMEREF = 1u << 3,
    ASH_VAR_ATTR_LOCAL = 1u << 4,
    ASH_VAR_ATTR_TRACE = 1u << 5,
    ASH_VAR_ATTR_UPPERCASE = 1u << 6,
    ASH_VAR_ATTR_LOWERCASE = 1u << 7,
    ASH_VAR_ATTR_ALL = (1u << 8) - 1u,
};

struct ash_var {
    /* The containing scope owns names, values, and nodes. */
    char* name;
    size_t name_length;
    struct ash_value value;
    /* Metadata is orthogonal to the tagged value and composes as flags. */
    uint32_t attributes;
    struct ash_var* next;
};

typedef void (*ash_var_visitor_fn)(
    const struct ash_var* variable,
    void* user_data
);

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
/* Returned values are borrowed until the next mutation of that variable. */
const char* ash_var_get(const struct ash_shell* shell, const char* name);
bool ash_var_exists(const struct ash_shell* shell, const char* name);
bool ash_var_attributes_valid(uint32_t attributes);
bool ash_var_has_attribute(
    const struct ash_shell* shell,
    const char* name,
    enum ash_var_attribute attribute
);
bool ash_var_update_attributes(
    struct ash_shell* shell,
    const char* name,
    uint32_t set,
    uint32_t clear
);
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
bool ash_var_set_local(
    struct ash_shell* shell,
    const char* name,
    const char* value,
    bool mark_export
);
bool ash_var_set_temporary(
    struct ash_shell* shell,
    const char* name,
    size_t name_length,
    const char* value,
    bool mark_export
);
bool ash_var_export(struct ash_shell* shell, const char* name);
void ash_vars_visit_visible(
    const struct ash_shell* shell,
    ash_var_visitor_fn visitor,
    void* user_data
);
bool ash_var_publish_visible(
    struct ash_shell* shell,
    const char* name,
    size_t name_length
);
void ash_var_unset(struct ash_shell* shell, const char* name);
void ash_var_list_destroy(struct ash_var** variables);
bool ash_import_environment(struct ash_shell* shell);

#endif /* BX_APPLETS_SHELL_ASH_VARIABLES_H */
