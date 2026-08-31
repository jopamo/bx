#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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
    size_t length
) {
    for (struct ash_var* var = shell->vars; var != NULL; var = var->next) {
        if (strlen(var->name) == length &&
            memcmp(var->name, name, length) == 0) {
            return var;
        }
    }
    return NULL;
}

static const struct ash_var* ash_var_find_len_const(
    const struct ash_shell* shell,
    const char* name,
    size_t length
) {
    for (const struct ash_var* var = shell->vars;
         var != NULL;
         var = var->next) {
        if (strlen(var->name) == length &&
            memcmp(var->name, name, length) == 0) {
            return var;
        }
    }
    return NULL;
}

const char* ash_var_get_len(
    const struct ash_shell* shell,
    const char* name,
    size_t length
) {
    const struct ash_var* var = ash_var_find_len_const(shell, name, length);
    return (var != NULL) ? var->value : NULL;
}

const char* ash_var_get(const struct ash_shell* shell, const char* name) {
    return ash_var_get_len(shell, name, strlen(name));
}

bool ash_var_exists(const struct ash_shell* shell, const char* name) {
    return ash_var_get(shell, name) != NULL;
}

static bool ash_var_publish_exported(
    struct ash_shell* shell,
    struct ash_var* var
) {
    if (!var->exported) {
        return true;
    }
    if (setenv(var->name, var->value, 1) != 0) {
        if (errno == ENOMEM) {
            return ash_vars_oom(shell);
        }
        ash_vars_error(shell, var->name, errno);
        return false;
    }
    return true;
}

bool ash_var_set_with_export(
    struct ash_shell* shell,
    const char* name,
    size_t name_length,
    const char* value,
    bool mark_export
) {
    struct ash_var* var = ash_var_find_len(shell, name, name_length);
    if (var == NULL) {
        var = malloc(sizeof(*var));
        if (var == NULL) {
            return ash_vars_oom(shell);
        }
        *var = (struct ash_var){0};
        var->name = ash_vars_duplicate(shell, name, name_length);
        var->value = ash_vars_duplicate(shell, value, strlen(value));
        if (var->name == NULL || var->value == NULL) {
            free(var->name);
            free(var->value);
            free(var);
            return false;
        }
        var->exported = mark_export;
        var->next = shell->vars;
        shell->vars = var;
    }
    else {
        char* replacement = ash_vars_duplicate(shell, value, strlen(value));
        if (replacement == NULL) {
            return false;
        }
        free(var->value);
        var->value = replacement;
        var->exported = var->exported || mark_export;
    }
    return ash_var_publish_exported(shell, var);
}

bool ash_var_set(
    struct ash_shell* shell,
    const char* name,
    const char* value,
    bool mark_export
) {
    return ash_var_set_with_export(
        shell,
        name,
        strlen(name),
        value,
        mark_export
    );
}

bool ash_var_export(struct ash_shell* shell, const char* name) {
    size_t length = strlen(name);
    struct ash_var* var = ash_var_find_len(shell, name, length);
    if (var == NULL) {
        return ash_var_set_with_export(shell, name, length, "", true);
    }
    var->exported = true;
    return ash_var_publish_exported(shell, var);
}

void ash_var_unset(struct ash_shell* shell, const char* name) {
    struct ash_var** link = &shell->vars;
    while (*link != NULL) {
        struct ash_var* current = *link;
        if (strcmp(current->name, name) == 0) {
            *link = current->next;
            (void)unsetenv(current->name);
            free(current->name);
            free(current->value);
            free(current);
            return;
        }
        link = &current->next;
    }
}

void ash_vars_destroy(struct ash_shell* shell) {
    while (shell->vars != NULL) {
        struct ash_var* var = shell->vars;
        shell->vars = var->next;
        free(var->name);
        free(var->value);
        free(var);
    }
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
