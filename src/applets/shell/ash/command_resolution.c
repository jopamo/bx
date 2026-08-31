#include <errno.h>
#include <stddef.h>
#include <string.h>

#include "applets/shell/ash/command_resolution.h"

static struct ash_command_resolution ash_builtin_resolution(
    const char* name,
    enum ash_command_resolution_kind kind,
    enum ash_builtin_kind builtin
) {
    return (struct ash_command_resolution){
        .kind = kind,
        .command_name = name,
        .target.builtin = builtin,
    };
}

struct ash_command_resolution ash_command_resolve_builtin(const char* name) {
    static const struct {
        const char* name;
        enum ash_command_resolution_kind kind;
        enum ash_builtin_kind builtin;
    } builtins[] = {
        {":", ASH_COMMAND_SPECIAL_BUILTIN, ASH_BUILTIN_COLON},
        {"break", ASH_COMMAND_SPECIAL_BUILTIN, ASH_BUILTIN_BREAK},
        {"continue", ASH_COMMAND_SPECIAL_BUILTIN, ASH_BUILTIN_CONTINUE},
        {"exec", ASH_COMMAND_SPECIAL_BUILTIN, ASH_BUILTIN_EXEC},
        {"exit", ASH_COMMAND_SPECIAL_BUILTIN, ASH_BUILTIN_EXIT},
        {"export", ASH_COMMAND_SPECIAL_BUILTIN, ASH_BUILTIN_EXPORT},
        {"set", ASH_COMMAND_SPECIAL_BUILTIN, ASH_BUILTIN_SET},
        {"unset", ASH_COMMAND_SPECIAL_BUILTIN, ASH_BUILTIN_UNSET},
        {"cd", ASH_COMMAND_REGULAR_BUILTIN, ASH_BUILTIN_CD},
        {"pwd", ASH_COMMAND_REGULAR_BUILTIN, ASH_BUILTIN_PWD},
        {"umask", ASH_COMMAND_REGULAR_BUILTIN, ASH_BUILTIN_UMASK},
    };

    for (size_t i = 0u; i < sizeof(builtins) / sizeof(builtins[0]); i++) {
        if (strcmp(name, builtins[i].name) == 0) {
            return ash_builtin_resolution(name, builtins[i].kind, builtins[i].builtin);
        }
    }
    return ash_command_resolution_not_found(name, ENOENT);
}

struct ash_command_resolution ash_command_resolution_function(const char* name, const struct ash_function* function) {
    return (struct ash_command_resolution){
        .kind = ASH_COMMAND_FUNCTION,
        .command_name = name,
        .target.function = function,
    };
}

enum ash_applet_execution_class ash_applet_execution_classify(const struct bx_applet* applet) {
    (void)applet;
    /*
     * Fail closed. No current bx applet has yet passed the repeated-call,
     * process-state restoration, and fatal-path audit required for an
     * in-process shell execution class.
     */
    return ASH_APPLET_EXEC_ONLY;
}

struct ash_command_resolution ash_command_resolution_bx_applet(const char* name, const struct bx_applet* applet) {
    return (struct ash_command_resolution){
        .kind = ASH_COMMAND_BX_APPLET,
        .command_name = name,
        .target.bx_applet = {
            .applet = applet,
            .execution_class = ash_applet_execution_classify(applet),
        },
    };
}

struct ash_command_resolution ash_command_resolution_hashed_external(const char* name, const char* path) {
    return (struct ash_command_resolution){
        .kind = ASH_COMMAND_HASHED_EXTERNAL,
        .command_name = name,
        .target.path = path,
    };
}

struct ash_command_resolution ash_command_resolution_explicit_path(const char* name, const char* path) {
    return (struct ash_command_resolution){
        .kind = ASH_COMMAND_EXPLICIT_PATH,
        .command_name = name,
        .target.path = path,
    };
}

struct ash_command_resolution ash_command_resolution_not_found(const char* name, int lookup_error) {
    return (struct ash_command_resolution){
        .kind = ASH_COMMAND_NOT_FOUND,
        .command_name = name,
        .target.lookup_error = lookup_error,
    };
}

bool ash_command_resolution_is_builtin(const struct ash_command_resolution* resolution) {
    return resolution != NULL &&
        (resolution->kind == ASH_COMMAND_SPECIAL_BUILTIN || resolution->kind == ASH_COMMAND_REGULAR_BUILTIN);
}
