#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "applets/shell/ash/scope.h"
#include "applets/shell/ash/shell_context.h"
#include "applets/shell/ash/variables.h"
#include "bx/diag.h"

extern char** environ;

static bool ash_vars_oom(const struct ash_shell* shell) {
    fprintf(stderr, "%s: out of memory\n", shell->progname);
    return false;
}

static void ash_vars_error(
    const struct ash_shell* shell,
    const char* subject,
    int error
) {
    fprintf(
        stderr,
        "%s: %s: %s\n",
        shell->progname,
        subject,
        bx_strerror(error)
    );
}

static char* ash_vars_duplicate(
    const struct ash_shell* shell,
    const char* text,
    size_t length
) {
    if (length == SIZE_MAX) {
        ash_vars_oom(shell);
        return NULL;
    }
    char* copy = malloc(length + 1u);
    if (copy == NULL) {
        ash_vars_oom(shell);
        return NULL;
    }
    memcpy(copy, text, length);
    copy[length] = '\0';
    return copy;
}

bool ash_is_name_start(unsigned char ch) {
    return isalpha(ch) || ch == '_';
}

bool ash_is_name_char(unsigned char ch) {
    return isalnum(ch) || ch == '_';
}

bool ash_is_valid_name_span(const char* text, size_t length) {
    if (length == 0u || !ash_is_name_start((unsigned char)text[0])) {
        return false;
    }
    for (size_t i = 1u; i < length; i++) {
        if (!ash_is_name_char((unsigned char)text[i])) {
            return false;
        }
    }
    return true;
}

bool ash_parse_assignment(
    const char* text,
    size_t* name_length,
    const char** value
) {
    const char* equals = strchr(text, '=');
    if (equals == NULL || equals == text) {
        return false;
    }
    size_t length = (size_t)(equals - text);
    if (!ash_is_valid_name_span(text, length)) {
        return false;
    }
    *name_length = length;
    *value = equals + 1;
    return true;
}

static struct ash_var* ash_var_find_len(
    struct ash_shell* shell,
    const char* name,
    size_t length,
    struct ash_scope** owner
) {
    return ash_scope_lookup_variable_mut(
            shell,
            name,
            length,
            ASH_SCOPE_LOOKUP_VISIBLE,
            owner
        );
}

static const struct ash_var* ash_var_find_len_const(
    const struct ash_shell* shell,
    const char* name,
    size_t length
) {
    struct ash_scope_binding binding;
    if (ash_scope_lookup(
            shell,
            name,
            length,
            ASH_SCOPE_LOOKUP_VISIBLE,
            false,
            &binding
        ) != ASH_SCOPE_LOOKUP_FOUND) {
        return NULL;
    }
    return binding.variable;
}

const char* ash_var_get_len(
    const struct ash_shell* shell,
    const char* name,
    size_t length
) {
    struct ash_scope_binding binding;
    if (ash_scope_lookup(
            shell,
            name,
            length,
            ASH_SCOPE_LOOKUP_VISIBLE,
            true,
            &binding
        ) != ASH_SCOPE_LOOKUP_FOUND) {
        return NULL;
    }
    return ash_value_get_scalar(&binding.variable->value);
}

const char* ash_var_get(const struct ash_shell* shell, const char* name) {
    return ash_var_get_len(shell, name, strlen(name));
}

bool ash_var_exists(const struct ash_shell* shell, const char* name) {
    return ash_var_find_len_const(shell, name, strlen(name)) != NULL;
}

bool ash_var_attributes_valid(uint32_t attributes) {
    return (attributes & ~ASH_VAR_ATTR_ALL) == 0u &&
        (attributes & (ASH_VAR_ATTR_UPPERCASE | ASH_VAR_ATTR_LOWERCASE)) !=
            (ASH_VAR_ATTR_UPPERCASE | ASH_VAR_ATTR_LOWERCASE);
}

bool ash_var_list_invariants(const struct ash_var* variables) {
    const struct ash_var* slow = variables;
    const struct ash_var* fast = variables;
    while (fast != NULL && fast->next != NULL) {
        slow = slow->next;
        fast = fast->next->next;
        if (slow == fast) {
            return false;
        }
    }

    for (const struct ash_var* variable = variables;
         variable != NULL;
         variable = variable->next) {
        if (variable->name == NULL ||
            strlen(variable->name) != variable->name_length ||
            !ash_is_valid_name_span(
                variable->name,
                variable->name_length
            ) ||
            !ash_var_attributes_valid(variable->attributes) ||
            !ash_value_invariants(&variable->value)) {
            return false;
        }
    }
    for (const struct ash_var* variable = variables;
         variable != NULL;
         variable = variable->next) {
        for (const struct ash_var* duplicate = variable->next;
             duplicate != NULL;
             duplicate = duplicate->next) {
            if (variable->name_length == duplicate->name_length &&
                memcmp(
                    variable->name,
                    duplicate->name,
                    variable->name_length
                ) == 0) {
                return false;
            }
        }
    }
    return true;
}

bool ash_var_has_attribute(
    const struct ash_shell* shell,
    const char* name,
    enum ash_var_attribute attribute
) {
    const struct ash_var* var = ash_var_find_len_const(
        shell,
        name,
        strlen(name)
    );
    return var != NULL && ((var->attributes & (uint32_t)attribute) != 0u);
}

bool ash_var_publish_visible(
    struct ash_shell* shell,
    const char* name,
    size_t name_length
) {
    const struct ash_var* var = ash_var_find_len_const(
        shell,
        name,
        name_length
    );
    const char* scalar = var != NULL ?
        ash_value_get_scalar(&var->value) : NULL;
    if (var == NULL ||
        (var->attributes & ASH_VAR_ATTR_EXPORT) == 0u ||
        scalar == NULL) {
        /*
         * Setters may receive a name span from "name=value"; prefer the
         * frame-owned NUL-terminated name whenever a binding remains.
         */
        (void)unsetenv(var != NULL ? var->name : name);
        return true;
    }
    if (setenv(var->name, scalar, 1) != 0) {
        if (errno == ENOMEM) {
            return ash_vars_oom(shell);
        }
        ash_vars_error(shell, var->name, errno);
        return false;
    }
    return true;
}

static struct ash_var* ash_var_find_in_scope(
    struct ash_scope* scope,
    const char* name,
    size_t name_length
) {
    for (struct ash_var* var = scope != NULL ? scope->variables : NULL;
         var != NULL;
         var = var->next) {
        if (var->name_length == name_length &&
            memcmp(var->name, name, name_length) == 0) {
            return var;
        }
    }
    return NULL;
}

static bool ash_var_set_in_scope(
    struct ash_shell* shell,
    struct ash_scope* scope,
    const char* name,
    size_t name_length,
    const char* value,
    uint32_t attributes,
    bool force_export
) {
    if (scope == NULL) {
        errno = EINVAL;
        return false;
    }
    /*
     * This is the sole assignment-to-export policy point. Once allexport is
     * active, every shell assignment acquires the persistent export
     * attribute; clearing allexport affects later assignments only.
     */
    if (force_export ||
        (shell->options & ASH_SHELL_OPTION_ALLEXPORT) != 0u) {
        attributes |= ASH_VAR_ATTR_EXPORT;
    }
    struct ash_var* var = ash_var_find_in_scope(scope, name, name_length);
    if (var == NULL) {
        var = malloc(sizeof(*var));
        if (var == NULL) {
            return ash_vars_oom(shell);
        }
        *var = (struct ash_var){0};
        var->name = ash_vars_duplicate(shell, name, name_length);
        var->name_length = name_length;
        if (var->name == NULL ||
            !ash_value_init_scalar(&var->value, value)) {
            free(var->name);
            ash_value_destroy(&var->value);
            free(var);
            ash_vars_oom(shell);
            return false;
        }
        var->attributes = attributes;
        var->next = scope->variables;
        scope->variables = var;
    }
    else {
        if (!ash_value_set_scalar(&var->value, value)) {
            ash_vars_oom(shell);
            return false;
        }
        var->attributes |= attributes;
    }
    assert(ash_var_list_invariants(scope->variables));
    return ash_var_publish_visible(shell, name, name_length);
}

bool ash_var_set_with_export(
    struct ash_shell* shell,
    const char* name,
    size_t name_length,
    const char* value,
    bool force_export
) {
    struct ash_scope* owner = NULL;
    (void)ash_var_find_len(shell, name, name_length, &owner);
    if (owner == NULL) {
        owner = ash_scope_global(shell);
    }
    return ash_var_set_in_scope(
        shell,
        owner,
        name,
        name_length,
        value,
        0u,
        force_export
    );
}

bool ash_var_set(
    struct ash_shell* shell,
    const char* name,
    const char* value,
    bool force_export
) {
    return ash_var_set_with_export(
        shell,
        name,
        strlen(name),
        value,
        force_export
    );
}

bool ash_var_set_local(
    struct ash_shell* shell,
    const char* name,
    const char* value,
    bool force_export
) {
    return ash_var_set_in_scope(
        shell,
        ash_scope_current_function(shell),
        name,
        strlen(name),
        value,
        ASH_VAR_ATTR_LOCAL,
        force_export
    );
}

bool ash_var_set_temporary(
    struct ash_shell* shell,
    const char* name,
    size_t name_length,
    const char* value,
    bool force_export
) {
    struct ash_scope* scope = ash_scope_current(shell);
    if (scope == NULL || scope->kind != ASH_SCOPE_TEMPORARY_ASSIGNMENT) {
        errno = EINVAL;
        return false;
    }
    return ash_var_set_in_scope(
        shell,
        scope,
        name,
        name_length,
        value,
        0u,
        force_export
    );
}

bool ash_var_export(struct ash_shell* shell, const char* name) {
    size_t length = strlen(name);
    struct ash_var* var = ash_var_find_len(shell, name, length, NULL);
    if (var == NULL) {
        return ash_var_set_with_export(shell, name, length, "", true);
    }
    return ash_var_update_attributes(
        shell,
        name,
        ASH_VAR_ATTR_EXPORT,
        0u
    );
}

bool ash_var_update_attributes(
    struct ash_shell* shell,
    const char* name,
    uint32_t set,
    uint32_t clear
) {
    size_t name_length = strlen(name);
    struct ash_var* var = ash_var_find_len(
        shell,
        name,
        name_length,
        NULL
    );
    if (var == NULL || (set & clear) != 0u ||
        ((set | clear) & ~ASH_VAR_ATTR_ALL) != 0u) {
        errno = EINVAL;
        return false;
    }
    uint32_t candidate = (var->attributes | set) & ~clear;
    if (!ash_var_attributes_valid(candidate)) {
        errno = EINVAL;
        return false;
    }
    uint32_t previous = var->attributes;
    var->attributes = candidate;
    if (!ash_var_publish_visible(shell, name, name_length)) {
        var->attributes = previous;
        (void)ash_var_publish_visible(shell, name, name_length);
        assert(ash_scope_stack_invariants(shell));
        return false;
    }
    assert(ash_scope_stack_invariants(shell));
    return true;
}

void ash_vars_visit_visible(
    const struct ash_shell* shell,
    ash_var_visitor_fn visitor,
    void* user_data
) {
    if (shell == NULL || visitor == NULL) {
        return;
    }
    for (const struct ash_scope* scope = shell->scopes;
         scope != NULL;
         scope = scope->parent) {
        for (const struct ash_var* var = scope->variables;
             var != NULL;
             var = var->next) {
            struct ash_scope_binding binding;
            if (ash_scope_lookup(
                    shell,
                    var->name,
                    var->name_length,
                    ASH_SCOPE_LOOKUP_VISIBLE,
                    false,
                    &binding
                ) == ASH_SCOPE_LOOKUP_FOUND &&
                binding.variable == var) {
                visitor(var, user_data);
            }
        }
    }
}

void ash_var_unset(struct ash_shell* shell, const char* name) {
    size_t name_length = strlen(name);
    struct ash_scope* owner = NULL;
    if (ash_var_find_len(shell, name, name_length, &owner) == NULL ||
        owner == NULL) {
        return;
    }
    struct ash_var** link = &owner->variables;
    while (*link != NULL) {
        struct ash_var* current = *link;
        if (current->name_length == name_length &&
            memcmp(current->name, name, name_length) == 0) {
            *link = current->next;
            (void)ash_var_publish_visible(shell, name, name_length);
            free(current->name);
            ash_value_destroy(&current->value);
            free(current);
            assert(ash_var_list_invariants(owner->variables));
            return;
        }
        link = &current->next;
    }
}

void ash_var_list_destroy(struct ash_var** variables) {
    if (variables == NULL) {
        return;
    }
    while (*variables != NULL) {
        struct ash_var* var = *variables;
        *variables = var->next;
        free(var->name);
        ash_value_destroy(&var->value);
        free(var);
    }
    assert(ash_var_list_invariants(*variables));
}

bool ash_import_environment(struct ash_shell* shell) {
    for (char** envp = environ; envp != NULL && *envp != NULL; envp++) {
        const char* entry = *envp;
        const char* equals = strchr(entry, '=');
        if (equals == NULL || equals == entry) {
            continue;
        }
        size_t name_length = (size_t)(equals - entry);
        if (!ash_is_valid_name_span(entry, name_length) ||
            (name_length == 3u && memcmp(entry, "IFS", 3u) == 0)) {
            continue;
        }
        if (!ash_var_set_with_export(
                shell,
                entry,
                name_length,
                equals + 1,
                true
            )) {
            return false;
        }
    }

    if (unsetenv("IFS") != 0 ||
        !ash_var_set(shell, "IFS", " \t\n", false)) {
        return false;
    }

    char* cwd = getcwd(NULL, 0);
    if (cwd != NULL) {
        const char* inherited_pwd = ash_var_get(shell, "PWD");
        struct stat cwd_stat;
        struct stat pwd_stat;
        bool valid_pwd = inherited_pwd != NULL &&
            inherited_pwd[0] == '/' &&
            stat(".", &cwd_stat) == 0 &&
            stat(inherited_pwd, &pwd_stat) == 0 &&
            cwd_stat.st_dev == pwd_stat.st_dev &&
            cwd_stat.st_ino == pwd_stat.st_ino;
        const char* logical = valid_pwd ? inherited_pwd : cwd;

        shell->cwd.physical = ash_vars_duplicate(shell, cwd, strlen(cwd));
        shell->cwd.logical = ash_vars_duplicate(
            shell,
            logical,
            strlen(logical)
        );
        const char* inherited_oldpwd = ash_var_get(shell, "OLDPWD");
        if (inherited_oldpwd != NULL) {
            shell->cwd.old_logical = ash_vars_duplicate(
                shell,
                inherited_oldpwd,
                strlen(inherited_oldpwd)
            );
        }
        if (shell->cwd.physical == NULL || shell->cwd.logical == NULL ||
            (inherited_oldpwd != NULL && shell->cwd.old_logical == NULL)) {
            free(cwd);
            return false;
        }
        if (!valid_pwd && !ash_var_set(shell, "PWD", cwd, true)) {
            free(cwd);
            return false;
        }
        free(cwd);
    }

    if (!ash_var_exists(shell, "PATH") &&
        !ash_var_set(shell, "PATH", "/bin:/usr/bin", true)) {
        return false;
    }
    return true;
}
