#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "applets/shell/ash/scope.h"
#include "applets/shell/ash/shell_context.h"
#include "applets/shell/ash/variables.h"

static struct ash_scope* ash_scope_allocate(enum ash_scope_kind kind) {
    struct ash_scope* scope = calloc(1u, sizeof(*scope));
    if (scope != NULL) {
        scope->kind = kind;
    }
    return scope;
}

static bool ash_scope_chain_acyclic(const struct ash_scope* scope) {
    const struct ash_scope* slow = scope;
    const struct ash_scope* fast = scope;
    while (fast != NULL && fast->parent != NULL) {
        slow = slow->parent;
        fast = fast->parent->parent;
        if (slow == fast) {
            return false;
        }
    }
    return true;
}

bool ash_scope_stack_invariants(const struct ash_shell* shell) {
    if (shell == NULL || shell->scopes == NULL ||
        !ash_scope_chain_acyclic(shell->scopes)) {
        return false;
    }

    size_t global_count = 0u;
    for (const struct ash_scope* scope = shell->scopes;
         scope != NULL;
         scope = scope->parent) {
        if (scope->kind < ASH_SCOPE_GLOBAL ||
            scope->kind > ASH_SCOPE_FUNCTION ||
            !ash_var_list_invariants(scope->variables)) {
            return false;
        }
        if (scope->kind == ASH_SCOPE_GLOBAL) {
            global_count++;
            if (scope->parent != NULL) {
                return false;
            }
        }

        bool owns_positionals =
            scope->kind == ASH_SCOPE_GLOBAL ||
            scope->kind == ASH_SCOPE_FUNCTION;
        if (scope->has_positionals != owns_positionals) {
            return false;
        }
        if (scope->has_positionals) {
            if (scope->positionals.argv0 == NULL ||
                (scope->positionals.count != 0u &&
                 scope->positionals.values == NULL)) {
                return false;
            }
        }
        else if (scope->positionals.argv0 != NULL ||
            scope->positionals.values != NULL ||
            scope->positionals.count != 0u) {
            return false;
        }
    }
    return global_count == 1u;
}

bool ash_scope_stack_init(
    struct ash_shell* shell,
    const char* argv0,
    char** positional_values,
    size_t positional_count
) {
    if (shell == NULL || shell->scopes != NULL || argv0 == NULL ||
        (positional_count > 0 && positional_values == NULL)) {
        return false;
    }
    struct ash_scope* global = ash_scope_allocate(ASH_SCOPE_GLOBAL);
    if (global == NULL) {
        return false;
    }
    global->has_positionals = true;
    global->positionals = (struct ash_positional_frame){
        .argv0 = argv0,
        .values = positional_values,
        .count = positional_count,
    };
    shell->scopes = global;
    assert(ash_scope_stack_invariants(shell));
    return true;
}

bool ash_scope_push_temporary(struct ash_shell* shell) {
    if (shell == NULL || shell->scopes == NULL) {
        return false;
    }
    assert(ash_scope_stack_invariants(shell));
    struct ash_scope* scope = ash_scope_allocate(
        ASH_SCOPE_TEMPORARY_ASSIGNMENT
    );
    if (scope == NULL) {
        return false;
    }
    scope->parent = shell->scopes;
    shell->scopes = scope;
    assert(ash_scope_stack_invariants(shell));
    return true;
}

bool ash_scope_push_function(
    struct ash_shell* shell,
    char** positional_values,
    size_t positional_count
) {
    if (shell == NULL || shell->scopes == NULL ||
        (positional_count > 0 && positional_values == NULL)) {
        return false;
    }
    assert(ash_scope_stack_invariants(shell));
    const struct ash_positional_frame* caller = ash_scope_positionals(shell);
    if (caller == NULL) {
        return false;
    }
    struct ash_scope* scope = ash_scope_allocate(ASH_SCOPE_FUNCTION);
    if (scope == NULL) {
        return false;
    }
    scope->has_positionals = true;
    scope->positionals = (struct ash_positional_frame){
        .argv0 = caller->argv0,
        .values = positional_values,
        .count = positional_count,
    };
    scope->parent = shell->scopes;
    shell->scopes = scope;
    assert(ash_scope_stack_invariants(shell));
    return true;
}

enum ash_scope_pop_result ash_scope_pop(
    struct ash_shell* shell,
    enum ash_scope_kind expected_kind
) {
    if (shell == NULL || shell->scopes == NULL) {
        return ASH_SCOPE_POP_MISMATCH;
    }
    assert(ash_scope_stack_invariants(shell));
    if (shell->scopes->kind == ASH_SCOPE_GLOBAL ||
        shell->scopes->kind != expected_kind) {
        return ASH_SCOPE_POP_MISMATCH;
    }

    struct ash_scope* removed = shell->scopes;
    shell->scopes = removed->parent;
    bool published = true;
    for (const struct ash_var* var = removed->variables;
         var != NULL;
         var = var->next) {
        if (!ash_var_publish_visible(shell, var->name, var->name_length)) {
            published = false;
        }
    }
    ash_var_list_destroy(&removed->variables);
    free(removed);
    assert(ash_scope_stack_invariants(shell));
    return published ? ASH_SCOPE_POP_OK :
        ASH_SCOPE_POP_PUBLICATION_ERROR;
}

void ash_scope_stack_destroy(struct ash_shell* shell) {
    if (shell == NULL) {
        return;
    }
    if (shell->scopes != NULL) {
        assert(ash_scope_stack_invariants(shell));
    }
    while (shell->scopes != NULL) {
        struct ash_scope* scope = shell->scopes;
        shell->scopes = scope->parent;
        ash_var_list_destroy(&scope->variables);
        free(scope);
    }
    assert(shell->scopes == NULL);
}

struct ash_scope* ash_scope_current(struct ash_shell* shell) {
    return shell != NULL ? shell->scopes : NULL;
}

struct ash_scope* ash_scope_global(struct ash_shell* shell) {
    if (shell == NULL) {
        return NULL;
    }
    struct ash_scope* scope = shell->scopes;
    while (scope != NULL && scope->parent != NULL) {
        scope = scope->parent;
    }
    return scope != NULL && scope->kind == ASH_SCOPE_GLOBAL ? scope : NULL;
}

struct ash_scope* ash_scope_current_function(struct ash_shell* shell) {
    if (shell == NULL) {
        return NULL;
    }
    for (struct ash_scope* scope = shell->scopes;
         scope != NULL;
         scope = scope->parent) {
        if (scope->kind == ASH_SCOPE_FUNCTION) {
            return scope;
        }
    }
    return NULL;
}

struct ash_var* ash_scope_lookup_variable_mut(
    struct ash_shell* shell,
    const char* name,
    size_t name_length,
    enum ash_scope_lookup_mode mode,
    struct ash_scope** owner
) {
    if (owner != NULL) {
        *owner = NULL;
    }
    if (shell == NULL || name == NULL ||
        mode < ASH_SCOPE_LOOKUP_VISIBLE ||
        mode > ASH_SCOPE_LOOKUP_GLOBAL) {
        return NULL;
    }

    struct ash_scope* first = NULL;
    if (mode == ASH_SCOPE_LOOKUP_CURRENT) {
        first = shell->scopes;
    }
    else if (mode == ASH_SCOPE_LOOKUP_GLOBAL) {
        first = ash_scope_global(shell);
    }
    else {
        first = shell->scopes;
    }

    for (struct ash_scope* scope = first;
         scope != NULL;
         scope = mode == ASH_SCOPE_LOOKUP_VISIBLE ? scope->parent : NULL) {
        for (struct ash_var* var = scope->variables;
             var != NULL;
             var = var->next) {
            if (var->name_length == name_length &&
                memcmp(var->name, name, name_length) == 0) {
                if (owner != NULL) {
                    *owner = scope;
                }
                return var;
            }
        }
    }
    return NULL;
}

const struct ash_positional_frame* ash_scope_positionals(
    const struct ash_shell* shell
) {
    if (shell == NULL) {
        return NULL;
    }
    for (const struct ash_scope* scope = shell->scopes;
         scope != NULL;
         scope = scope->parent) {
        if (scope->has_positionals) {
            return &scope->positionals;
        }
    }
    return NULL;
}

struct ash_positional_frame* ash_scope_positionals_mut(
    struct ash_shell* shell
) {
    if (shell == NULL) {
        return NULL;
    }
    for (struct ash_scope* scope = shell->scopes;
         scope != NULL;
         scope = scope->parent) {
        if (scope->has_positionals) {
            return &scope->positionals;
        }
    }
    return NULL;
}

static const struct ash_var* ash_scope_find_in_frame(
    const struct ash_scope* scope,
    const char* name,
    size_t name_length
) {
    for (const struct ash_var* var = scope != NULL ? scope->variables : NULL;
         var != NULL;
         var = var->next) {
        if (var->name_length == name_length &&
            memcmp(var->name, name, name_length) == 0) {
            return var;
        }
    }
    return NULL;
}

static const struct ash_scope* ash_scope_global_const(
    const struct ash_shell* shell
) {
    if (shell == NULL) {
        return NULL;
    }
    const struct ash_scope* scope = shell->scopes;
    while (scope != NULL && scope->parent != NULL) {
        scope = scope->parent;
    }
    return scope != NULL && scope->kind == ASH_SCOPE_GLOBAL ? scope : NULL;
}

static const struct ash_var* ash_scope_lookup_raw(
    const struct ash_shell* shell,
    const char* name,
    size_t name_length,
    enum ash_scope_lookup_mode mode,
    const struct ash_scope** owner
) {
    *owner = NULL;
    if (shell == NULL || name == NULL) {
        return NULL;
    }

    const struct ash_scope* first = NULL;
    if (mode == ASH_SCOPE_LOOKUP_CURRENT) {
        first = shell->scopes;
    }
    else if (mode == ASH_SCOPE_LOOKUP_GLOBAL) {
        first = ash_scope_global_const(shell);
    }
    else {
        first = shell->scopes;
    }

    for (const struct ash_scope* scope = first;
         scope != NULL;
         scope = mode == ASH_SCOPE_LOOKUP_VISIBLE ? scope->parent : NULL) {
        const struct ash_var* var = ash_scope_find_in_frame(
            scope,
            name,
            name_length
        );
        if (var != NULL) {
            *owner = scope;
            return var;
        }
    }
    return NULL;
}

static size_t ash_scope_variable_count(const struct ash_shell* shell) {
    size_t count = 0u;
    for (const struct ash_scope* scope = shell != NULL ?
             shell->scopes : NULL;
         scope != NULL;
         scope = scope->parent) {
        for (const struct ash_var* var = scope->variables;
             var != NULL;
             var = var->next) {
            count++;
        }
    }
    return count;
}

enum ash_scope_lookup_status ash_scope_lookup(
    const struct ash_shell* shell,
    const char* name,
    size_t name_length,
    enum ash_scope_lookup_mode mode,
    bool follow_nameref,
    struct ash_scope_binding* binding
) {
    if (binding == NULL) {
        return ASH_SCOPE_LOOKUP_UNSET;
    }
    *binding = (struct ash_scope_binding){
        .name = name,
        .name_length = name_length,
    };
    if (shell == NULL || name == NULL ||
        mode < ASH_SCOPE_LOOKUP_VISIBLE ||
        mode > ASH_SCOPE_LOOKUP_GLOBAL) {
        return ASH_SCOPE_LOOKUP_UNSET;
    }

    const struct ash_scope* owner = NULL;
    const struct ash_var* var = ash_scope_lookup_raw(
        shell,
        name,
        name_length,
        mode,
        &owner
    );
    if (var == NULL) {
        return ASH_SCOPE_LOOKUP_UNSET;
    }

    size_t followed = 0u;
    const size_t binding_count = ash_scope_variable_count(shell);
    while (follow_nameref &&
           (var->attributes & ASH_VAR_ATTR_NAMEREF) != 0u) {
        if (followed == binding_count) {
            return ASH_SCOPE_LOOKUP_NAMEREF_CYCLE;
        }
        followed++;
        const char* target = ash_value_get_scalar(&var->value);
        if (target == NULL ||
            !ash_is_valid_name_span(target, strlen(target))) {
            binding->name = target;
            binding->name_length = target != NULL ? strlen(target) : 0u;
            return ASH_SCOPE_LOOKUP_INVALID_NAMEREF;
        }
        binding->name = target;
        binding->name_length = strlen(target);
        var = ash_scope_lookup_raw(
            shell,
            target,
            binding->name_length,
            ASH_SCOPE_LOOKUP_VISIBLE,
            &owner
        );
        if (var == NULL) {
            return ASH_SCOPE_LOOKUP_UNSET;
        }
    }

    binding->scope = owner;
    binding->variable = var;
    binding->name = var->name;
    binding->name_length = var->name_length;
    return ASH_SCOPE_LOOKUP_FOUND;
}
