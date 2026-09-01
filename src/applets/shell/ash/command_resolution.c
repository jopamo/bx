#include <errno.h>
#include <stddef.h>
#include <string.h>

#include "applets/shell/ash/command_resolution.h"
#include "applets/shell/ash/functions.h"
#include "applets/shell/ash/shell_context.h"

/*
 * Keep the current Linux self-dispatch mechanism behind one exact path. The
 * standalone policy never retries PATH after selecting a registry identity.
 */
static const char ash_standalone_applet_exec_path[] = "/proc/self/exe";

struct ash_builtin_definition {
    const char* name;
    enum ash_builtin_kind builtin;
};

static const struct ash_builtin_definition ash_builtins[] = {
    {":", ASH_BUILTIN_COLON},
    {"break", ASH_BUILTIN_BREAK},
    {"continue", ASH_BUILTIN_CONTINUE},
    {"exec", ASH_BUILTIN_EXEC},
    {"exit", ASH_BUILTIN_EXIT},
    {"export", ASH_BUILTIN_EXPORT},
    {"return", ASH_BUILTIN_RETURN},
    {"set", ASH_BUILTIN_SET},
    {"shift", ASH_BUILTIN_SHIFT},
    {"unset", ASH_BUILTIN_UNSET},
    {"cd", ASH_BUILTIN_CD},
    {"pwd", ASH_BUILTIN_PWD},
    {"umask", ASH_BUILTIN_UMASK},
    {"wait", ASH_BUILTIN_WAIT},
};

static enum ash_command_resolution_kind ash_builtin_resolution_kind(
    enum ash_builtin_kind builtin
) {
    switch (builtin) {
        case ASH_BUILTIN_COLON:
        case ASH_BUILTIN_EXIT:
        case ASH_BUILTIN_EXPORT:
        case ASH_BUILTIN_UNSET:
        case ASH_BUILTIN_EXEC:
        case ASH_BUILTIN_SET:
        case ASH_BUILTIN_BREAK:
        case ASH_BUILTIN_CONTINUE:
        case ASH_BUILTIN_RETURN:
        case ASH_BUILTIN_SHIFT:
            return ASH_COMMAND_SPECIAL_BUILTIN;
        case ASH_BUILTIN_CD:
        case ASH_BUILTIN_UMASK:
        case ASH_BUILTIN_PWD:
        case ASH_BUILTIN_WAIT:
            return ASH_COMMAND_REGULAR_BUILTIN;
        case ASH_BUILTIN_INVALID:
            return ASH_COMMAND_INVALID;
    }
    return ASH_COMMAND_INVALID;
}

static const struct ash_builtin_definition* ash_builtin_find(
    const char* name
) {
    if (name == NULL) {
        return NULL;
    }
    for (size_t i = 0u;
         i < sizeof(ash_builtins) / sizeof(ash_builtins[0]);
         i++) {
        if (strcmp(name, ash_builtins[i].name) == 0) {
            return &ash_builtins[i];
        }
    }
    return NULL;
}

static struct ash_command_resolution ash_builtin_resolution(
    const char* name,
    const struct ash_builtin_definition* definition
) {
    return (struct ash_command_resolution){
        .kind = ash_builtin_resolution_kind(definition->builtin),
        .command_name = name,
        .target.builtin = definition->builtin,
    };
}

struct ash_command_resolution ash_command_resolution_function(
    const char* name,
    const struct ash_function* function
) {
    if (name == NULL || function == NULL) {
        return ash_command_resolution_not_found(name, ENOENT);
    }
    return (struct ash_command_resolution){
        .kind = ASH_COMMAND_FUNCTION,
        .command_name = name,
        .target.function = function,
    };
}

struct ash_command_resolution ash_command_resolution_bx_applet(
    const char* name,
    const struct bx_applet* applet,
    const char* exec_path
) {
    if (name == NULL || applet == NULL || exec_path == NULL ||
        strchr(exec_path, '/') == NULL) {
        return ash_command_resolution_not_found(name, ENOENT);
    }
    return (struct ash_command_resolution){
        .kind = ASH_COMMAND_BX_APPLET,
        .command_name = name,
        .target.bx_applet = {
            .applet = applet,
            .execution_class = bx_applet_execution_class_get(applet),
            .exec_path = exec_path,
        },
    };
}

struct ash_command_resolution ash_command_resolution_path_search(
    const char* name
) {
    if (name == NULL || name[0] == '\0' || strchr(name, '/') != NULL) {
        return ash_command_resolution_not_found(name, ENOENT);
    }
    return (struct ash_command_resolution){
        .kind = ASH_COMMAND_PATH_SEARCH,
        .command_name = name,
    };
}

struct ash_command_resolution ash_command_resolution_hashed_external(
    const char* name,
    const char* path
) {
    if (name == NULL || path == NULL || path[0] == '\0') {
        return ash_command_resolution_not_found(name, ENOENT);
    }
    return (struct ash_command_resolution){
        .kind = ASH_COMMAND_HASHED_EXTERNAL,
        .command_name = name,
        .target.path = path,
    };
}

struct ash_command_resolution ash_command_resolution_explicit_path(
    const char* name,
    const char* path
) {
    if (name == NULL || path == NULL || strchr(path, '/') == NULL) {
        return ash_command_resolution_not_found(name, ENOENT);
    }
    return (struct ash_command_resolution){
        .kind = ASH_COMMAND_EXPLICIT_PATH,
        .command_name = name,
        .target.path = path,
    };
}

struct ash_command_resolution ash_command_resolution_not_found(
    const char* name,
    int lookup_error
) {
    return (struct ash_command_resolution){
        .kind = ASH_COMMAND_NOT_FOUND,
        .command_name = name != NULL ? name : "",
        .target.lookup_error = lookup_error != 0 ? lookup_error : ENOENT,
    };
}

struct ash_command_resolution ash_command_resolve_external(const char* name) {
    if (name == NULL || name[0] == '\0') {
        return ash_command_resolution_not_found(name, ENOENT);
    }
    if (strchr(name, '/') != NULL) {
        return ash_command_resolution_explicit_path(name, name);
    }
    return ash_command_resolution_path_search(name);
}

struct ash_command_resolution ash_command_resolve(
    const struct ash_shell* shell,
    const char* name
) {
    struct ash_command_resolution external =
        ash_command_resolve_external(name);
    if (external.kind != ASH_COMMAND_PATH_SEARCH) {
        return external;
    }

    const struct ash_builtin_definition* builtin =
        ash_builtin_find(name);
    if (builtin != NULL &&
        ash_builtin_resolution_kind(builtin->builtin) ==
            ASH_COMMAND_SPECIAL_BUILTIN) {
        return ash_builtin_resolution(name, builtin);
    }

    const struct ash_function* function =
        shell != NULL ? ash_function_find(shell, name) : NULL;
    if (function != NULL) {
        return ash_command_resolution_function(name, function);
    }

    if (builtin != NULL) {
        return ash_builtin_resolution(name, builtin);
    }

    if (shell != NULL &&
        ash_shell_policy_valid(&shell->policy) &&
        ash_shell_policy_has(
            &shell->policy,
            ASH_SHELL_POLICY_STANDALONE_APPLETS
        )) {
        const struct bx_applet* applet = bx_applet_find(name);
        if (applet != NULL) {
            return ash_command_resolution_bx_applet(
                name,
                applet,
                ash_standalone_applet_exec_path
            );
        }
    }
    return external;
}

bool ash_command_resolution_valid(
    const struct ash_command_resolution* resolution
) {
    if (resolution == NULL || resolution->command_name == NULL) {
        return false;
    }
    if (resolution->kind != ASH_COMMAND_NOT_FOUND &&
        resolution->command_name[0] == '\0') {
        return false;
    }
    switch (resolution->kind) {
        case ASH_COMMAND_INVALID:
            return false;
        case ASH_COMMAND_SPECIAL_BUILTIN:
        case ASH_COMMAND_REGULAR_BUILTIN:
            return resolution->kind ==
                ash_builtin_resolution_kind(
                    resolution->target.builtin
                );
        case ASH_COMMAND_FUNCTION:
            return resolution->target.function != NULL;
        case ASH_COMMAND_BX_APPLET:
            return resolution->target.bx_applet.applet != NULL &&
                resolution->target.bx_applet.execution_class ==
                    bx_applet_execution_class_get(
                        resolution->target.bx_applet.applet
                    ) &&
                resolution->target.bx_applet.exec_path != NULL &&
                strchr(
                    resolution->target.bx_applet.exec_path,
                    '/'
                ) != NULL;
        case ASH_COMMAND_PATH_SEARCH:
            return resolution->command_name[0] != '\0' &&
                strchr(resolution->command_name, '/') == NULL;
        case ASH_COMMAND_HASHED_EXTERNAL:
            return resolution->target.path != NULL &&
                resolution->target.path[0] != '\0';
        case ASH_COMMAND_EXPLICIT_PATH:
            return resolution->target.path != NULL &&
                strchr(resolution->target.path, '/') != NULL;
        case ASH_COMMAND_NOT_FOUND:
            return resolution->target.lookup_error != 0;
    }
    return false;
}

bool ash_command_resolution_is_builtin(
    const struct ash_command_resolution* resolution
) {
    return ash_command_resolution_valid(resolution) &&
        (resolution->kind == ASH_COMMAND_SPECIAL_BUILTIN || resolution->kind == ASH_COMMAND_REGULAR_BUILTIN);
}
