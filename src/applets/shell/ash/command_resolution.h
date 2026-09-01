#ifndef BX_APPLETS_SHELL_ASH_COMMAND_RESOLUTION_H
#define BX_APPLETS_SHELL_ASH_COMMAND_RESOLUTION_H

#include <stdbool.h>

#include "dispatch/dispatch.h"

struct ash_function;
struct ash_shell;

enum ash_builtin_kind {
    ASH_BUILTIN_INVALID = 0,
    ASH_BUILTIN_COLON,
    ASH_BUILTIN_CD,
    ASH_BUILTIN_EXIT,
    ASH_BUILTIN_EXPORT,
    ASH_BUILTIN_UNSET,
    ASH_BUILTIN_UMASK,
    ASH_BUILTIN_PWD,
    ASH_BUILTIN_EXEC,
    ASH_BUILTIN_SET,
    ASH_BUILTIN_BREAK,
    ASH_BUILTIN_CONTINUE,
    ASH_BUILTIN_RETURN,
    ASH_BUILTIN_SHIFT,
    ASH_BUILTIN_WAIT,
};

enum ash_command_resolution_kind {
    ASH_COMMAND_INVALID = 0,
    ASH_COMMAND_SPECIAL_BUILTIN,
    ASH_COMMAND_REGULAR_BUILTIN,
    ASH_COMMAND_FUNCTION,
    ASH_COMMAND_BX_APPLET,
    ASH_COMMAND_PATH_SEARCH,
    ASH_COMMAND_HASHED_EXTERNAL,
    ASH_COMMAND_EXPLICIT_PATH,
    ASH_COMMAND_NOT_FOUND,
};

/*
 * Resolution values own no storage. command_name and every pointer in target
 * borrow from the command, shell context, dispatch registry, or command cache
 * and must remain valid until execution consumes the value.
 */
struct ash_command_resolution {
    enum ash_command_resolution_kind kind;
    const char* command_name;
    union {
        enum ash_builtin_kind builtin;
        const struct ash_function* function;
        struct {
            const struct bx_applet* applet;
            enum bx_applet_execution_class execution_class;
            /*
             * Exact executable selected by ordinary command lookup. Unsafe
             * or otherwise ineligible applets must exec this path without
             * repeating PATH search.
             */
            const char* fallback_path;
        } bx_applet;
        const char* path;
        int lookup_error;
    } target;
};

/*
 * Ordinary shell precedence is special builtin, function, regular builtin,
 * then external lookup. Slash-containing names bypass shell namespaces.
 */
struct ash_command_resolution ash_command_resolve(
    const struct ash_shell* shell,
    const char* name
);
/* Resolve an operand that must cross an external-command exec boundary. */
struct ash_command_resolution ash_command_resolve_external(const char* name);

struct ash_command_resolution ash_command_resolution_function(
    const char* name,
    const struct ash_function* function
);
struct ash_command_resolution ash_command_resolution_bx_applet(
    const char* name,
    const struct bx_applet* applet,
    const char* fallback_path
);
struct ash_command_resolution ash_command_resolution_path_search(
    const char* name
);
struct ash_command_resolution ash_command_resolution_hashed_external(
    const char* name,
    const char* path
);
struct ash_command_resolution ash_command_resolution_explicit_path(
    const char* name,
    const char* path
);
struct ash_command_resolution ash_command_resolution_not_found(
    const char* name,
    int lookup_error
);

bool ash_command_resolution_valid(
    const struct ash_command_resolution* resolution
);
bool ash_command_resolution_is_builtin(
    const struct ash_command_resolution* resolution
);

#endif /* BX_APPLETS_SHELL_ASH_COMMAND_RESOLUTION_H */
