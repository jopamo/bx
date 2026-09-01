#ifndef BX_APPLETS_SHELL_ASH_SCOPE_H
#define BX_APPLETS_SHELL_ASH_SCOPE_H

#include <stdbool.h>
#include <stddef.h>

struct ash_shell;
struct ash_var;

enum ash_scope_kind {
    ASH_SCOPE_GLOBAL,
    ASH_SCOPE_TEMPORARY_ASSIGNMENT,
    ASH_SCOPE_FUNCTION,
};

struct ash_positional_frame {
    const char* argv0;
    char** values;
    size_t count;
};

/*
 * The scope stack is the sole authority for variable and positional lookup.
 * Frames own their variable nodes. Positional strings and arrays are borrowed
 * for the lifetime of the frame.
 */
struct ash_scope {
    enum ash_scope_kind kind;
    struct ash_var* variables;
    bool has_positionals;
    struct ash_positional_frame positionals;
    struct ash_scope* parent;
};

enum ash_scope_lookup_mode {
    ASH_SCOPE_LOOKUP_VISIBLE,
    ASH_SCOPE_LOOKUP_CURRENT,
    ASH_SCOPE_LOOKUP_GLOBAL,
};

enum ash_scope_lookup_status {
    ASH_SCOPE_LOOKUP_FOUND,
    ASH_SCOPE_LOOKUP_UNSET,
    ASH_SCOPE_LOOKUP_INVALID_NAMEREF,
    ASH_SCOPE_LOOKUP_NAMEREF_CYCLE,
};

enum ash_scope_pop_result {
    ASH_SCOPE_POP_MISMATCH,
    ASH_SCOPE_POP_OK,
    /* The expected frame was consumed, but environment publication failed. */
    ASH_SCOPE_POP_PUBLICATION_ERROR,
};

struct ash_scope_binding {
    const struct ash_scope* scope;
    const struct ash_var* variable;
    /*
     * For FOUND this names variable. For UNSET after nameref traversal it is
     * the borrowed terminal target name.
     */
    const char* name;
    size_t name_length;
};

bool ash_scope_stack_init(
    struct ash_shell* shell,
    const char* argv0,
    char** positional_values,
    size_t positional_count
);
bool ash_scope_push_temporary(struct ash_shell* shell);
bool ash_scope_push_function(
    struct ash_shell* shell,
    char** positional_values,
    size_t positional_count
);
enum ash_scope_pop_result ash_scope_pop(
    struct ash_shell* shell,
    enum ash_scope_kind expected_kind
);
bool ash_scope_stack_invariants(const struct ash_shell* shell);
void ash_scope_stack_destroy(struct ash_shell* shell);

struct ash_scope* ash_scope_current(struct ash_shell* shell);
struct ash_scope* ash_scope_global(struct ash_shell* shell);
struct ash_scope* ash_scope_current_function(struct ash_shell* shell);
struct ash_var* ash_scope_lookup_variable_mut(
    struct ash_shell* shell,
    const char* name,
    size_t name_length,
    enum ash_scope_lookup_mode mode,
    struct ash_scope** owner
);
const struct ash_positional_frame* ash_scope_positionals(
    const struct ash_shell* shell
);
struct ash_positional_frame* ash_scope_positionals_mut(
    struct ash_shell* shell
);

enum ash_scope_lookup_status ash_scope_lookup(
    const struct ash_shell* shell,
    const char* name,
    size_t name_length,
    enum ash_scope_lookup_mode mode,
    bool follow_nameref,
    struct ash_scope_binding* binding
);

#endif /* BX_APPLETS_SHELL_ASH_SCOPE_H */
