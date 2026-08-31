#ifndef BX_APPLETS_SHELL_ASH_COMMAND_RESOLUTION_H
#define BX_APPLETS_SHELL_ASH_COMMAND_RESOLUTION_H

#include <stdbool.h>

struct ash_function;
struct bx_applet;

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
};

enum ash_command_resolution_kind {
    ASH_COMMAND_SPECIAL_BUILTIN = 0,
    ASH_COMMAND_REGULAR_BUILTIN,
    ASH_COMMAND_FUNCTION,
    ASH_COMMAND_BX_APPLET,
    ASH_COMMAND_HASHED_EXTERNAL,
    ASH_COMMAND_EXPLICIT_PATH,
    ASH_COMMAND_NOT_FOUND,
};

enum ash_applet_execution_class {
    ASH_APPLET_CURRENT_SHELL_SAFE = 0,
    ASH_APPLET_CHILD_ONLY,
    ASH_APPLET_EXEC_ONLY,
};

struct ash_command_resolution {
    enum ash_command_resolution_kind kind;
    const char* command_name;
    union {
        enum ash_builtin_kind builtin;
        const struct ash_function* function;
        struct {
            const struct bx_applet* applet;
            enum ash_applet_execution_class execution_class;
        } bx_applet;
        const char* path;
        int lookup_error;
    } target;
};

struct ash_command_resolution ash_command_resolve_builtin(const char* name);
struct ash_command_resolution ash_command_resolution_function(const char* name, const struct ash_function* function);
struct ash_command_resolution ash_command_resolution_bx_applet(const char* name, const struct bx_applet* applet);
struct ash_command_resolution ash_command_resolution_hashed_external(const char* name, const char* path);
struct ash_command_resolution ash_command_resolution_explicit_path(const char* name, const char* path);
struct ash_command_resolution ash_command_resolution_not_found(const char* name, int lookup_error);

bool ash_command_resolution_is_builtin(const struct ash_command_resolution* resolution);
enum ash_applet_execution_class ash_applet_execution_classify(const struct bx_applet* applet);

#endif /* BX_APPLETS_SHELL_ASH_COMMAND_RESOLUTION_H */
