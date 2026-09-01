#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "applets.h"
#include "applets/shell/ash/command.h"
#include "applets/shell/ash/command_resolution.h"
#include "applets/shell/ash/diagnostic.h"
#include "applets/shell/ash/expansion.h"
#include "applets/shell/ash/external_command.h"
#include "applets/shell/ash/functions.h"
#include "applets/shell/ash/input.h"
#include "applets/shell/ash/lexer.h"
#include "applets/shell/ash/pattern.h"
#include "applets/shell/ash/parser.h"
#include "applets/shell/ash/process.h"
#include "applets/shell/ash/redirection.h"
#include "applets/shell/ash/shell_context.h"
#include "applets/shell/ash/variables.h"
#include "lib/cli_common.h"
#include "lib/fd_ops.h"
#include "lib/text_buffer.h"

static int ash_execute_ast(struct ash_shell* shell, const struct ash_ast* node);

static void ash_print_exported_variable(
    const struct ash_var* var,
    void* user_data
) {
    (void)user_data;
    if ((var->attributes & ASH_VAR_ATTR_EXPORT) != 0u) {
        const char* value = ash_value_get_scalar(&var->value);
        if (value != NULL) {
            printf("%s=%s\n", var->name, value);
        }
    }
}

static void ash_print_scalar_variable(
    const struct ash_var* var,
    void* user_data
) {
    (void)user_data;
    const char* value = ash_value_get_scalar(&var->value);
    if (value != NULL) {
        printf("%s=%s\n", var->name, value);
    }
}

static const char* ash_basename(const char* path) {
    return bx_cli_progname(path, "ash");
}

static const char* ash_effective_name(const char* argv0) {
    const char* base = ash_basename(argv0);
    while (*base == '-') {
        base++;
    }
    return (*base != '\0') ? base : "ash";
}

static void* ash_malloc_bytes(const struct ash_shell* shell, size_t size) {
    void* out = malloc(size);
    if (out == NULL && size != 0u) {
        ash_diag_oom(shell);
        return NULL;
    }
    return out;
}

static bool ash_allocation_size(const struct ash_shell* shell, size_t count, size_t size, size_t* bytes_out) {
    if (size != 0u && count > SIZE_MAX / size) {
        return ash_diag_oom(shell);
    }

    *bytes_out = count * size;
    return true;
}

static void* ash_realloc_array(const struct ash_shell* shell, void* ptr, size_t count, size_t size) {
    size_t bytes = 0u;
    if (!ash_allocation_size(shell, count, size, &bytes)) {
        return NULL;
    }

    void* out = realloc(ptr, bytes);
    if (out == NULL && bytes != 0u) {
        ash_diag_oom(shell);
        return NULL;
    }
    return out;
}

static char* ash_slice_dup(const struct ash_shell* shell, const char* text, size_t len) {
    if (len == SIZE_MAX) {
        ash_diag_oom(shell);
        return NULL;
    }

    char* out = ash_malloc_bytes(shell, len + 1u);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, text, len);
    out[len] = '\0';
    return out;
}

static char* ash_strdup_text(const struct ash_shell* shell, const char* text) {
    return ash_slice_dup(shell, text, strlen(text));
}

static void ash_command_init(struct ash_command* command) {
    command->words = NULL;
    command->word_count = 0;
    command->word_cap = 0;

    command->assignments = NULL;
    command->assignment_count = 0;
    command->assignment_cap = 0;

    command->redirs = NULL;
    command->redir_count = 0;
    command->redir_cap = 0;
}

static bool ash_command_push_word(const struct ash_shell* shell, struct ash_command* command, const char* text) {
    if (command->word_count == command->word_cap) {
        size_t new_cap = (command->word_cap == 0) ? 8u : command->word_cap * 2u;
        if (command->word_cap != 0u && command->word_cap > SIZE_MAX / 2u) {
            return ash_diag_oom(shell);
        }
        if (new_cap == SIZE_MAX) {
            return ash_diag_oom(shell);
        }
        char** grown = ash_realloc_array(shell, command->words, new_cap + 1u, sizeof(*command->words));
        if (grown == NULL) {
            return false;
        }
        command->words = grown;
        command->word_cap = new_cap;
    }

    char* word = ash_strdup_text(shell, text);
    if (word == NULL) {
        return false;
    }
    command->words[command->word_count++] = word;
    command->words[command->word_count] = NULL;
    return true;
}

static bool ash_command_push_assignment(const struct ash_shell* shell, struct ash_command* command, const char* text) {
    if (command->assignment_count == command->assignment_cap) {
        size_t new_cap = (command->assignment_cap == 0) ? 4u : command->assignment_cap * 2u;
        if (command->assignment_cap != 0u && command->assignment_cap > SIZE_MAX / 2u) {
            return ash_diag_oom(shell);
        }
        char** grown = ash_realloc_array(shell, command->assignments, new_cap, sizeof(*command->assignments));
        if (grown == NULL) {
            return false;
        }
        command->assignments = grown;
        command->assignment_cap = new_cap;
    }

    char* assignment = ash_strdup_text(shell, text);
    if (assignment == NULL) {
        return false;
    }
    command->assignments[command->assignment_count++] = assignment;
    return true;
}

static bool ash_command_push_redir(struct ash_shell* shell, struct ash_command* command, int fd, enum ash_redir_kind kind, const char* target) {
    if (command->redir_count == command->redir_cap) {
        size_t new_cap = (command->redir_cap == 0) ? 4u : command->redir_cap * 2u;
        if (command->redir_cap != 0u && command->redir_cap > SIZE_MAX / 2u) {
            return ash_diag_oom(shell);
        }
        struct ash_redir* grown = ash_realloc_array(shell, command->redirs, new_cap, sizeof(*command->redirs));
        if (grown == NULL) {
            return false;
        }
        command->redirs = grown;
        command->redir_cap = new_cap;
    }

    struct ash_redir* redir = &command->redirs[command->redir_count++];
    redir->fd = fd;
    redir->kind = kind;
    redir->target = ash_strdup_text(shell, target);
    if (redir->target == NULL) {
        command->redir_count--;
        return false;
    }
    return true;
}

static void ash_command_destroy(struct ash_command* command) {
    for (size_t i = 0; i < command->word_count; i++) {
        free(command->words[i]);
    }
    free(command->words);

    for (size_t i = 0; i < command->assignment_count; i++) {
        free(command->assignments[i]);
    }
    free(command->assignments);

    for (size_t i = 0; i < command->redir_count; i++) {
        free(command->redirs[i].target);
    }
    free(command->redirs);

    ash_command_init(command);
}

static int ash_parse_status_code(const char* text, int* out_status) {
    char* endptr = NULL;
    errno = 0;
    long long value = strtoll(text, &endptr, 10);
    if (errno != 0 || endptr == text || *endptr != '\0') {
        return 1;
    }

    *out_status = (int)(value & 0xff);
    return 0;
}

static int ash_parse_umask_value(const char* text, mode_t* out_mode) {
    char* endptr = NULL;
    errno = 0;
    unsigned long value = strtoul(text, &endptr, 8);
    if (errno != 0 || endptr == text || *endptr != '\0' || value > 0777u) {
        return 1;
    }

    *out_mode = (mode_t)value;
    return 0;
}

static char* ash_getcwd_dup(void) {
    size_t size = 128u;

    while (true) {
        char* buffer = malloc(size);
        if (buffer == NULL) {
            errno = ENOMEM;
            return NULL;
        }
        if (getcwd(buffer, size) != NULL) {
            return buffer;
        }

        int err = errno;
        free(buffer);

        if (err != ERANGE) {
            errno = err;
            return NULL;
        }

        if (size > (SIZE_MAX / 2u)) {
            errno = ENOMEM;
            return NULL;
        }
        size *= 2u;
    }
}

static int ash_apply_command_assignments_shell(struct ash_shell* shell, const struct ash_command* command) {
    for (size_t i = 0; i < command->assignment_count; i++) {
        size_t name_len = 0;
        const char* value = NULL;
        if (!ash_parse_assignment(command->assignments[i], &name_len, &value)) {
            ash_diag(shell, "invalid assignment '%s'", command->assignments[i]);
            return 1;
        }

        if (!ash_var_set_with_export(shell, command->assignments[i], name_len, value, false)) {
            return 1;
        }
    }

    return 0;
}

static int ash_apply_command_assignments_env(struct ash_shell* shell, const struct ash_command* command) {
    (void)shell;
    for (size_t i = 0; i < command->assignment_count; i++) {
        size_t name_len = 0;
        const char* value = NULL;
        if (!ash_parse_assignment(command->assignments[i], &name_len, &value)) {
            return 1;
        }

        char* name = ash_slice_dup(shell, command->assignments[i], name_len);
        if (name == NULL) {
            return 1;
        }
        if (setenv(name, value, 1) != 0) {
            int err = errno;
            free(name);
            if (err == ENOMEM) {
                ash_diag_oom(shell);
            }
            else {
                ash_exec_error(shell, "setenv", err);
            }
            return 1;
        }
        free(name);
    }

    return 0;
}

static int ash_builtin_cd(struct ash_shell* shell, const struct ash_command* command) {
    if (command->word_count > 2u) {
        ash_diag(shell, "cd: too many arguments");
        return 1;
    }

    const char* target = NULL;
    bool print_new_dir = false;

    if (command->word_count == 1u) {
        target = ash_var_get(shell, "HOME");
        if (target == NULL || target[0] == '\0') {
            ash_diag(shell, "cd: HOME not set");
            return 1;
        }
    }
    else {
        target = command->words[1];
        if (strcmp(target, "-") == 0) {
            target = ash_var_get(shell, "OLDPWD");
            if (target == NULL || target[0] == '\0') {
                ash_diag(shell, "cd: OLDPWD not set");
                return 1;
            }
            print_new_dir = true;
        }
    }

    char* oldcwd = ash_getcwd_dup();

    if (chdir(target) != 0) {
        ash_exec_error(shell, target, errno);
        free(oldcwd);
        return 1;
    }

    char* newcwd = ash_getcwd_dup();
    if (newcwd != NULL) {
        char* new_physical = ash_strdup_text(shell, newcwd);
        char* new_logical = ash_strdup_text(shell, newcwd);
        char* new_old_logical = shell->cwd.logical != NULL
            ? ash_strdup_text(shell, shell->cwd.logical)
            : (oldcwd != NULL ? ash_strdup_text(shell, oldcwd) : NULL);
        if (new_physical == NULL || new_logical == NULL ||
            ((shell->cwd.logical != NULL || oldcwd != NULL) && new_old_logical == NULL)) {
            free(new_physical);
            free(new_logical);
            free(new_old_logical);
            free(oldcwd);
            free(newcwd);
            return 1;
        }
        free(shell->cwd.physical);
        free(shell->cwd.logical);
        free(shell->cwd.old_logical);
        shell->cwd.physical = new_physical;
        shell->cwd.logical = new_logical;
        shell->cwd.old_logical = new_old_logical;
        if (!ash_var_set(shell, "PWD", newcwd, false)) {
            free(oldcwd);
            free(newcwd);
            return 1;
        }
    }

    if (oldcwd != NULL) {
        if (!ash_var_set(shell, "OLDPWD", oldcwd, false)) {
            free(oldcwd);
            free(newcwd);
            return 1;
        }
    }

    if (print_new_dir) {
        if (newcwd != NULL) {
            printf("%s\n", newcwd);
        }
        else {
            printf("%s\n", target);
        }
    }

    free(oldcwd);
    free(newcwd);
    return 0;
}

static int ash_builtin_exit(struct ash_shell* shell, const struct ash_command* command, bool in_child) {
    if (command->word_count > 2u) {
        ash_diag(shell, "exit: too many arguments");
        return 1;
    }

    int status = shell->last_status;
    if (command->word_count == 2u) {
        if (ash_parse_status_code(command->words[1], &status) != 0) {
            ash_diag(shell, "exit: numeric argument required");
            status = 2;
        }
    }

    if (!in_child) {
        shell->should_exit = true;
        shell->requested_exit_status = status;
    }

    return status;
}

static int ash_builtin_export(struct ash_shell* shell, const struct ash_command* command) {
    if (command->word_count == 1u) {
        ash_vars_visit_visible(shell, ash_print_exported_variable, NULL);
        return 0;
    }

    int status = 0;

    for (size_t i = 1; i < command->word_count; i++) {
        const char* arg = command->words[i];
        size_t name_len = 0;
        const char* value = NULL;

        if (ash_parse_assignment(arg, &name_len, &value)) {
            if (!ash_var_set_with_export(shell, arg, name_len, value, true)) {
                status = 1;
            }
            continue;
        }

        size_t len = strlen(arg);
        if (!ash_is_valid_name_span(arg, len)) {
            ash_diag(shell, "export: invalid name '%s'", arg);
            status = 1;
            continue;
        }

        if (!ash_var_export(shell, arg)) {
            status = 1;
        }
    }

    return status;
}

static int ash_builtin_unset(struct ash_shell* shell, const struct ash_command* command) {
    int status = 0;
    bool unset_functions = false;
    size_t first_name = 1u;

    if (command->word_count > 1u) {
        if (strcmp(command->words[1], "-f") == 0) {
            unset_functions = true;
            first_name = 2u;
        }
        else if (strcmp(command->words[1], "-v") == 0) {
            first_name = 2u;
        }
        else if (strcmp(command->words[1], "--") == 0) {
            first_name = 2u;
        }
        else if (command->words[1][0] == '-' &&
                 command->words[1][1] != '\0') {
            ash_diag(
                shell,
                "unset: invalid option '%s'",
                command->words[1]
            );
            return 2;
        }
    }

    for (size_t i = first_name; i < command->word_count; i++) {
        const char* name = command->words[i];
        size_t len = strlen(name);
        if (!ash_is_valid_name_span(name, len)) {
            ash_diag(shell, "unset: invalid name '%s'", name);
            status = 1;
            continue;
        }
        if (unset_functions) {
            ash_function_unset(shell, name);
        }
        else {
            ash_var_unset(shell, name);
        }
    }

    return status;
}

static int ash_builtin_umask(struct ash_shell* shell, const struct ash_command* command) {
    if (command->word_count == 1u) {
        mode_t mask = umask(0);
        umask(mask);
        printf("%04o\n", (unsigned)(mask & 0777u));
        return 0;
    }

    if (command->word_count != 2u) {
        ash_diag(shell, "umask: expected zero or one operand");
        return 1;
    }

    mode_t new_mask = 0;
    if (ash_parse_umask_value(command->words[1], &new_mask) != 0) {
        ash_diag(shell, "umask: invalid mode '%s'", command->words[1]);
        return 1;
    }

    umask(new_mask);
    return 0;
}

static int ash_builtin_pwd(struct ash_shell* shell, const struct ash_command* command) {
    if (command->word_count > 1u) {
        ash_diag(shell, "pwd: extra operand '%s'", command->words[1]);
        return 1;
    }

    char* cwd = ash_getcwd_dup();
    if (cwd == NULL) {
        ash_exec_error(shell, "pwd", errno);
        return 1;
    }

    printf("%s\n", cwd);
    free(cwd);
    return 0;
}

static int ash_builtin_exec(struct ash_shell* shell, const struct ash_command* command) {
    if (command->word_count == 1u) {
        return 0;
    }

    char** argv = &command->words[1];
    struct ash_command_resolution resolution =
        ash_command_resolve_external(argv[0]);
    int status = ash_external_command_exec(
        shell,
        argv,
        &resolution
    );
    return status;
}

static int ash_builtin_set(struct ash_shell* shell, const struct ash_command* command) {
    if (command->word_count == 1u) {
        ash_vars_visit_visible(shell, ash_print_scalar_variable, NULL);
        return 0;
    }

    int status = 0;

    for (size_t i = 1; i < command->word_count; i++) {
        const char* arg = command->words[i];
        if (strcmp(arg, "--") == 0) {
            continue;
        }
        if ((strcmp(arg, "-C") == 0 || strcmp(arg, "+C") == 0)) {
            if (arg[0] == '-') {
                shell->options |= ASH_SHELL_OPTION_NOCLOBBER;
            }
            else {
                shell->options &= ~ASH_SHELL_OPTION_NOCLOBBER;
            }
            continue;
        }

        size_t name_len = 0;
        const char* value = NULL;
        if (!ash_parse_assignment(arg, &name_len, &value)) {
            ash_diag(shell, "set: unsupported operand '%s'", arg);
            status = 1;
            continue;
        }

        if (!ash_var_set_with_export(shell, arg, name_len, value, false)) {
            status = 1;
        }
    }

    return status;
}

static int ash_builtin_loop_control(
    struct ash_shell* shell,
    const struct ash_command* command,
    enum ash_control_kind kind
) {
    const char* name = kind == ASH_CONTROL_BREAK ? "break" : "continue";
    if (command->word_count > 2u) {
        ash_diag(shell, "%s: too many arguments", name);
        return 2;
    }

    unsigned long levels = 1u;
    if (command->word_count == 2u) {
        char* end = NULL;
        errno = 0;
        levels = strtoul(command->words[1], &end, 10);
        if (errno != 0 || end == command->words[1] || *end != '\0' ||
            levels == 0u || levels > UINT_MAX) {
            ash_diag(
                shell,
                "%s: Illegal number: %s",
                name,
                command->words[1]
            );
            return 2;
        }
    }

    ash_control_request_loop(shell, kind, (unsigned int)levels);
    return 0;
}

static int ash_builtin_return(
    struct ash_shell* shell,
    const struct ash_command* command
) {
    if (command->word_count > 2u) {
        ash_diag(shell, "return: too many arguments");
        return 2;
    }
    int status = shell->last_status;
    if (command->word_count == 2u &&
        ash_parse_status_code(command->words[1], &status) != 0) {
        ash_diag(shell, "return: numeric argument required");
        return 2;
    }
    if (!ash_control_request_return(shell, status)) {
        ash_diag(shell, "return: not in a function");
        return 2;
    }
    return status;
}

static int ash_builtin_shift(
    struct ash_shell* shell,
    const struct ash_command* command
) {
    if (command->word_count > 2u) {
        ash_diag(shell, "shift: too many arguments");
        return 2;
    }
    unsigned long count = 1u;
    if (command->word_count == 2u) {
        char* end = NULL;
        errno = 0;
        count = strtoul(command->words[1], &end, 10);
        if (errno != 0 || end == command->words[1] || *end != '\0' ||
            count > INT_MAX) {
            ash_diag(
                shell,
                "shift: Illegal number: %s",
                command->words[1]
            );
            return 2;
        }
    }
    struct ash_positional_frame* positionals =
        ash_scope_positionals_mut(shell);
    if (positionals == NULL ||
        count > (unsigned long)positionals->count) {
        ash_diag(shell, "shift: can't shift that many");
        return 1;
    }
    if (count != 0u) {
        positionals->values += count;
    }
    positionals->count -= (int)count;
    return 0;
}

static int ash_builtin_wait(
    struct ash_shell* shell,
    const struct ash_command* command
) {
    if (command->word_count == 1u) {
        int status;
        if (ash_jobs_wait_all(shell, &status) != 0) {
            ash_exec_error(shell, "wait", errno);
            return 1;
        }
        return 0;
    }

    int status = 0;
    for (size_t i = 1u; i < command->word_count; i++) {
        char* end = NULL;
        errno = 0;
        long parsed = strtol(command->words[i], &end, 10);
        if (errno != 0 || end == command->words[i] || *end != '\0' ||
            parsed <= 0 || parsed > INT_MAX) {
            ash_diag(shell, "wait: invalid pid '%s'", command->words[i]);
            status = 127;
            continue;
        }

        int child_status;
        if (ash_jobs_wait_pid(
                shell,
                (pid_t)parsed,
                &child_status
            ) != 0) {
            ash_exec_error(shell, command->words[i], errno);
            status = 127;
            continue;
        }
        status = child_status;
    }
    return status;
}

static int ash_run_builtin(struct ash_shell* shell, enum ash_builtin_kind builtin, const struct ash_command* command, bool in_child) {
    switch (builtin) {
        case ASH_BUILTIN_COLON:
            return 0;
        case ASH_BUILTIN_CD:
            return ash_builtin_cd(shell, command);
        case ASH_BUILTIN_EXIT:
            return ash_builtin_exit(shell, command, in_child);
        case ASH_BUILTIN_EXPORT:
            return ash_builtin_export(shell, command);
        case ASH_BUILTIN_UNSET:
            return ash_builtin_unset(shell, command);
        case ASH_BUILTIN_UMASK:
            return ash_builtin_umask(shell, command);
        case ASH_BUILTIN_PWD:
            return ash_builtin_pwd(shell, command);
        case ASH_BUILTIN_EXEC:
            return ash_builtin_exec(shell, command);
        case ASH_BUILTIN_SET:
            return ash_builtin_set(shell, command);
        case ASH_BUILTIN_BREAK:
            return ash_builtin_loop_control(
                shell,
                command,
                ASH_CONTROL_BREAK
            );
        case ASH_BUILTIN_CONTINUE:
            return ash_builtin_loop_control(
                shell,
                command,
                ASH_CONTROL_CONTINUE
            );
        case ASH_BUILTIN_RETURN:
            return ash_builtin_return(shell, command);
        case ASH_BUILTIN_SHIFT:
            return ash_builtin_shift(shell, command);
        case ASH_BUILTIN_WAIT:
            return ash_builtin_wait(shell, command);
        case ASH_BUILTIN_INVALID:
            break;
    }

    return 1;
}

static int ash_execute_function(
    struct ash_shell* shell,
    const struct ash_function* function,
    const struct ash_command* command
) {
    struct ash_ast* invocation_body = ash_ast_clone(function->body);
    if (invocation_body == NULL) {
        ash_diag_oom(shell);
        return 2;
    }

    bool temporary_scope = command->assignment_count != 0u;
    if (temporary_scope && !ash_scope_push_temporary(shell)) {
        ash_diag_oom(shell);
        ash_ast_destroy(invocation_body);
        return 2;
    }
    for (size_t i = 0u; i < command->assignment_count; i++) {
        size_t name_length = 0u;
        const char* value = NULL;
        if (!ash_parse_assignment(
                command->assignments[i],
                &name_length,
                &value
            ) ||
            !ash_var_set_temporary(
                shell,
                command->assignments[i],
                name_length,
                value,
                true
            )) {
            (void)ash_scope_pop(
                shell,
                ASH_SCOPE_TEMPORARY_ASSIGNMENT
            );
            ash_ast_destroy(invocation_body);
            return 2;
        }
    }

    struct ash_redirection_transaction saved_fds;
    ash_redirection_transaction_init(&saved_fds);
    if (ash_redirection_transaction_apply(shell, command, &saved_fds) != 0) {
        if (temporary_scope) {
            (void)ash_scope_pop(
                shell,
                ASH_SCOPE_TEMPORARY_ASSIGNMENT
            );
        }
        ash_ast_destroy(invocation_body);
        return 1;
    }

    if (!ash_scope_push_function(
            shell,
            command->word_count > 1u ? &command->words[1] : NULL,
            command->word_count > 1u ? command->word_count - 1u : 0u
        )) {
        ash_diag_oom(shell);
        (void)ash_redirection_transaction_rollback(shell, &saved_fds);
        if (temporary_scope) {
            (void)ash_scope_pop(
                shell,
                ASH_SCOPE_TEMPORARY_ASSIGNMENT
            );
        }
        ash_ast_destroy(invocation_body);
        return 2;
    }
    unsigned int caller_loop_depth = shell->control.loop_depth;
    shell->control.loop_depth = 0u;
    ash_control_enter_function(shell);
    int status = ash_execute_ast(shell, invocation_body);
    (void)ash_control_consume_return(shell, &status);
    ash_control_leave_function(shell);
    shell->control.loop_depth = caller_loop_depth;
    enum ash_scope_pop_result function_scope_pop = ash_scope_pop(
        shell,
        ASH_SCOPE_FUNCTION
    );

    if (ash_redirection_transaction_rollback(shell, &saved_fds) != 0) {
        status = 1;
    }
    enum ash_scope_pop_result temporary_scope_pop =
        ASH_SCOPE_POP_OK;
    if (temporary_scope) {
        temporary_scope_pop = ash_scope_pop(
            shell,
            ASH_SCOPE_TEMPORARY_ASSIGNMENT
        );
    }
    ash_ast_destroy(invocation_body);
    if (function_scope_pop != ASH_SCOPE_POP_OK ||
        temporary_scope_pop != ASH_SCOPE_POP_OK) {
        return 2;
    }
    return status;
}

static int ash_execute_in_child(
    struct ash_shell* shell,
    const struct ash_command* command,
    const struct ash_command_resolution* resolution
) {
    if (!ash_command_resolution_valid(resolution)) {
        ash_exec_error(
            shell,
            command->word_count != 0u ? command->words[0] : "",
            EINVAL
        );
        return 126;
    }
    if (resolution->kind == ASH_COMMAND_FUNCTION) {
        return ash_execute_function(
            shell,
            resolution->target.function,
            command
        );
    }
    if (ash_redirections_apply_permanently(shell, command) != 0) {
        return 1;
    }

    if (command->word_count == 0u) {
        if (ash_apply_command_assignments_shell(shell, command) != 0) {
            return 1;
        }
        return 0;
    }

    if (ash_command_resolution_is_builtin(resolution)) {
        if (ash_apply_command_assignments_shell(shell, command) != 0) {
            return 1;
        }
        return ash_run_builtin(
            shell,
            resolution->target.builtin,
            command,
            true
        );
    }

    if (ash_apply_command_assignments_env(shell, command) != 0) {
        return 1;
    }

    return ash_external_command_exec(
        shell,
        command->words,
        resolution
    );
}

static int ash_execute_single_command_parent(struct ash_shell* shell, const struct ash_command* command, enum ash_builtin_kind builtin) {
    if (ash_apply_command_assignments_shell(shell, command) != 0) {
        return 1;
    }

    struct ash_redirection_transaction saved;
    ash_redirection_transaction_init(&saved);

    if (ash_redirection_transaction_apply(shell, command, &saved) != 0) {
        return 1;
    }

    int status = ash_run_builtin(shell, builtin, command, false);

    if (!shell->should_exit) {
        if (ash_redirection_transaction_rollback(shell, &saved) != 0) {
            return 1;
        }
    }
    else if (ash_redirection_transaction_commit(shell, &saved) != 0) {
        return 1;
    }

    return status;
}

struct ash_command_child_context {
    struct ash_shell* shell;
    const struct ash_command* command;
    struct ash_command_resolution resolution;
};

static int ash_command_child_main(void* user_data) {
    struct ash_command_child_context* context = user_data;
    ash_shell_context_detach_after_fork(context->shell);
    return ash_execute_in_child(
        context->shell,
        context->command,
        &context->resolution
    );
}

static int ash_run_foreground_child(
    struct ash_shell* shell,
    enum ash_job_kind job_kind,
    enum ash_process_role process_role,
    ash_child_callback callback,
    void* user_data
) {
    struct ash_job* job = ash_job_create(shell, job_kind, true);
    if (job == NULL) {
        ash_diag_oom(shell);
        return 1;
    }
    size_t process_index;
    if (ash_job_start_process(
            job,
            process_role,
            callback,
            user_data,
            &process_index
        ) != 0) {
        int error = errno;
        ash_job_abort(job);
        ash_exec_error(shell, "fork", error);
        return 1;
    }
    (void)process_index;
    if (!ash_job_commit(job, ASH_JOB_PRIVATE)) {
        ash_job_abort(job);
        ash_exec_error(shell, "child registration", EINVAL);
        return 1;
    }
    int status;
    if (ash_job_wait(job, &status) != 0) {
        int error = errno;
        ash_job_abort(job);
        ash_exec_error(shell, "waitpid", error);
        return 1;
    }
    (void)ash_job_release(job);
    return status;
}

static int ash_execute_single_command_forked(
    struct ash_shell* shell,
    const struct ash_command* command,
    const struct ash_command_resolution* resolution
) {
    struct ash_command_child_context context = {
        .shell = shell,
        .command = command,
        .resolution = *resolution,
    };
    return ash_run_foreground_child(
        shell,
        ASH_JOB_FOREGROUND_COMMAND,
        ASH_PROCESS_EXTERNAL_COMMAND,
        ash_command_child_main,
        &context
    );
}

static int ash_execute_command(
    struct ash_shell* shell,
    const struct ash_command* command
) {
    if (command->word_count == 0u) {
        if (ash_apply_command_assignments_shell(shell, command) != 0) {
            return 1;
        }
        if (command->redir_count == 0u) {
            return 0;
        }

        struct ash_redirection_transaction saved;
        ash_redirection_transaction_init(&saved);
        if (ash_redirection_transaction_apply(shell, command, &saved) != 0) {
            return 1;
        }
        return ash_redirection_transaction_rollback(shell, &saved);
    }

    struct ash_command_resolution resolution =
        ash_command_resolve(shell, command->words[0]);
    if (!ash_command_resolution_valid(&resolution)) {
        ash_exec_error(shell, command->words[0], EINVAL);
        return 126;
    }
    switch (resolution.kind) {
        case ASH_COMMAND_SPECIAL_BUILTIN:
        case ASH_COMMAND_REGULAR_BUILTIN:
            return ash_execute_single_command_parent(
                shell,
                command,
                resolution.target.builtin
            );
        case ASH_COMMAND_FUNCTION:
            return ash_execute_function(
                shell,
                resolution.target.function,
                command
            );
        case ASH_COMMAND_BX_APPLET:
        case ASH_COMMAND_PATH_SEARCH:
        case ASH_COMMAND_HASHED_EXTERNAL:
        case ASH_COMMAND_EXPLICIT_PATH:
        case ASH_COMMAND_NOT_FOUND:
            return ash_execute_single_command_forked(
                shell,
                command,
                &resolution
            );
        case ASH_COMMAND_INVALID:
            break;
    }
    ash_exec_error(shell, command->words[0], EINVAL);
    return 126;
}

static int ash_execute_buffer(
    struct ash_shell* shell,
    const char* input,
    bool final_input,
    bool* incomplete_out,
    bool* parser_error_out
);

struct ash_substitution_child_context {
    struct ash_shell* shell;
    const char* command;
    size_t length;
    int read_fd;
    int write_fd;
};

static int ash_substitution_child_main(void* user_data) {
    struct ash_substitution_child_context* context = user_data;
    close(context->read_fd);
    if (bx_fd_dup2_exact(context->write_fd, STDOUT_FILENO) < 0) {
        ash_exec_error(context->shell, "dup2", errno);
        return 1;
    }
    close(context->write_fd);

    char* input = ash_slice_dup(
        context->shell,
        context->command,
        context->length
    );
    if (input == NULL) {
        return 2;
    }
    struct ash_shell* child = context->shell;
    ash_shell_context_detach_after_fork(child);
    child->should_exit = false;
    child->requested_exit_status = 0;
    bool incomplete = false;
    bool parser_error = false;
    int status = ash_execute_buffer(
        child,
        input,
        true,
        &incomplete,
        &parser_error
    );
    free(input);
    return status;
}

static bool ash_command_substitute(
    struct ash_shell* shell,
    const char* command,
    size_t length,
    char** output
) {
    *output = NULL;
    int pipe_fds[2];
    if (bx_fd_pipe_cloexec(pipe_fds) != 0) {
        ash_exec_error(shell, "pipe", errno);
        return false;
    }

    struct ash_job* job = ash_job_create(
        shell,
        ASH_JOB_COMMAND_SUBSTITUTION,
        true
    );
    if (job == NULL) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return ash_diag_oom(shell);
    }
    struct ash_substitution_child_context context = {
        .shell = shell,
        .command = command,
        .length = length,
        .read_fd = pipe_fds[0],
        .write_fd = pipe_fds[1],
    };
    size_t process_index;
    if (ash_job_start_process(
            job,
            ASH_PROCESS_COMMAND_SUBSTITUTION,
            ash_substitution_child_main,
            &context,
            &process_index
        ) != 0) {
        int error = errno;
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        ash_job_abort(job);
        ash_exec_error(shell, "fork", error);
        return false;
    }
    (void)process_index;
    if (!ash_job_commit(job, ASH_JOB_PRIVATE)) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        ash_job_abort(job);
        ash_exec_error(shell, "child registration", EINVAL);
        return false;
    }

    close(pipe_fds[1]);
    struct bx_text_buffer captured;
    bx_text_buffer_init(&captured);
    bool read_ok = true;
    char chunk[4096];
    while (true) {
        ssize_t count = read(pipe_fds[0], chunk, sizeof(chunk));
        if (count > 0) {
            if (!bx_text_buffer_append_span(
                    &captured,
                    chunk,
                    (size_t)count
                )) {
                ash_diag_oom(shell);
                read_ok = false;
                break;
            }
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            ash_exec_error(shell, "read", errno);
            read_ok = false;
        }
        break;
    }
    close(pipe_fds[0]);

    int status;
    if (ash_job_wait(job, &status) != 0) {
        int error = errno;
        ash_job_abort(job);
        ash_exec_error(shell, "waitpid", error);
        bx_text_buffer_destroy(&captured);
        return false;
    }
    (void)ash_job_release(job);
    shell->last_status = status;
    if (!read_ok) {
        bx_text_buffer_destroy(&captured);
        return false;
    }

    while (captured.length != 0u &&
           captured.data[captured.length - 1u] == '\n') {
        captured.data[--captured.length] = '\0';
    }
    *output = bx_text_buffer_take(&captured);
    if (*output == NULL) {
        bx_text_buffer_destroy(&captured);
        return ash_diag_oom(shell);
    }
    return true;
}

static bool ash_ast_add_redirection(
    struct ash_shell* shell,
    struct ash_command* command,
    const struct ash_redirection* redirection
) {
    int fd;
    if (redirection->io_number != NULL) {
        if (!ash_redirection_parse_fd(shell, redirection->io_number, &fd)) {
            return false;
        }
    }
    else {
        fd = (redirection->operator == ASH_TOKEN_LESS ||
              redirection->operator == ASH_TOKEN_LESS_AND ||
              redirection->operator == ASH_TOKEN_LESS_GREAT) ? 0 : 1;
    }

    enum ash_redir_kind kind;
    switch (redirection->operator) {
        case ASH_TOKEN_LESS:
            kind = ASH_REDIR_IN;
            break;
        case ASH_TOKEN_GREAT:
            kind = ASH_REDIR_OUT;
            break;
        case ASH_TOKEN_CLOBBER:
            kind = ASH_REDIR_CLOBBER;
            break;
        case ASH_TOKEN_DGREAT:
            kind = ASH_REDIR_APPEND;
            break;
        case ASH_TOKEN_LESS_GREAT:
            kind = ASH_REDIR_READWRITE;
            break;
        case ASH_TOKEN_LESS_AND:
        case ASH_TOKEN_GREAT_AND:
            kind = ASH_REDIR_DUP;
            break;
        default:
            ash_diag(
                shell,
                "redirection not implemented: %s",
                ash_token_kind_name(redirection->operator)
            );
            return false;
    }

    char* target = NULL;
    if (!ash_expand_word(
            shell,
            &redirection->target.syntax,
            &target
        )) {
        return false;
    }
    bool added = ash_command_push_redir(shell, command, fd, kind, target);
    free(target);
    return added;
}

static bool ash_ast_simple_to_command(
    struct ash_shell* shell,
    const struct ash_ast* node,
    struct ash_command* command
) {
    ash_command_init(command);
    for (size_t i = 0u; i < node->value.simple.count; i++) {
        const struct ash_simple_item* item = &node->value.simple.items[i];
        if (item->kind == ASH_SIMPLE_REDIRECTION) {
            if (!ash_ast_add_redirection(
                    shell,
                    command,
                    &item->value.redirection
                )) {
                ash_command_destroy(command);
                return false;
            }
            continue;
        }

        if (item->kind == ASH_SIMPLE_ASSIGNMENT) {
            char* text = NULL;
            if (!ash_expand_word(
                    shell,
                    &item->value.word.syntax,
                    &text
                )) {
                ash_command_destroy(command);
                return false;
            }
            bool added = ash_command_push_assignment(
                shell,
                command,
                text
            );
            free(text);
            if (!added) {
                ash_command_destroy(command);
                return false;
            }
        }
        else {
            struct ash_expanded_fields fields;
            if (!ash_expand_argument(
                    shell,
                    &item->value.word.syntax,
                    &fields
                )) {
                ash_command_destroy(command);
                return false;
            }
            for (size_t j = 0u; j < fields.count; j++) {
                if (!ash_command_push_word(
                        shell,
                        command,
                        fields.values[j]
                    )) {
                    ash_expanded_fields_destroy(&fields);
                    ash_command_destroy(command);
                    return false;
                }
            }
            ash_expanded_fields_destroy(&fields);
        }
    }
    return true;
}

static bool ash_ast_trailing_redirections_to_command(
    struct ash_shell* shell,
    const struct ash_ast* node,
    struct ash_command* command
) {
    ash_command_init(command);
    for (size_t i = 0u; i < node->trailing_redirection_count; i++) {
        if (!ash_ast_add_redirection(
                shell,
                command,
                &node->trailing_redirections[i]
            )) {
            ash_command_destroy(command);
            return false;
        }
    }
    return true;
}

static int ash_execute_ast_simple(
    struct ash_shell* shell,
    const struct ash_ast* node
) {
    struct ash_command command;
    if (!ash_ast_simple_to_command(shell, node, &command)) {
        return 2;
    }
    int status = ash_execute_command(shell, &command);
    ash_command_destroy(&command);
    return status;
}

struct ash_subshell_child_context {
    struct ash_shell* shell;
    const struct ash_ast* node;
};

static int ash_subshell_child_main(void* user_data) {
    struct ash_subshell_child_context* context = user_data;
    ash_shell_context_detach_after_fork(context->shell);
    struct ash_command redirections;
    if (!ash_ast_trailing_redirections_to_command(
            context->shell,
            context->node,
            &redirections
        )) {
        return 2;
    }
    struct ash_redirection_transaction saved;
    ash_redirection_transaction_init(&saved);
    if (ash_redirection_transaction_apply(
            context->shell,
            &redirections,
            &saved
        ) != 0) {
        ash_command_destroy(&redirections);
        return 1;
    }
    ash_command_destroy(&redirections);
    int status = ash_execute_ast(
        context->shell,
        context->node->value.group.body
    );
    return ash_redirection_transaction_commit(
        context->shell,
        &saved
    ) == 0 ? status : 1;
}

static int ash_execute_ast_group(
    struct ash_shell* shell,
    const struct ash_ast* node,
    bool subshell
) {
    if (subshell) {
        struct ash_subshell_child_context context = {
            .shell = shell,
            .node = node,
        };
        return ash_run_foreground_child(
            shell,
            ASH_JOB_FOREGROUND_COMMAND,
            ASH_PROCESS_SUBSHELL,
            ash_subshell_child_main,
            &context
        );
    }

    struct ash_command redirections;
    if (!ash_ast_trailing_redirections_to_command(shell, node, &redirections)) {
        return 2;
    }
    struct ash_redirection_transaction saved;
    ash_redirection_transaction_init(&saved);
    if (ash_redirection_transaction_apply(
            shell,
            &redirections,
            &saved
        ) != 0) {
        ash_command_destroy(&redirections);
        return 1;
    }
    ash_command_destroy(&redirections);
    int status = ash_execute_ast(shell, node->value.group.body);
    return ash_redirection_transaction_rollback(shell, &saved) == 0 ?
        status : 1;
}

struct ash_pipeline_child_context {
    struct ash_shell* shell;
    const struct ash_ast* command;
    int previous_read;
    int pipe_read;
    int pipe_write;
    bool pipe_stderr;
};

static int ash_pipeline_child_main(void* user_data) {
    struct ash_pipeline_child_context* context = user_data;
    ash_shell_context_detach_after_fork(context->shell);
    if (context->previous_read >= 0 &&
        bx_fd_dup2_exact(context->previous_read, STDIN_FILENO) < 0) {
        ash_exec_error(context->shell, "dup2", errno);
        return 1;
    }
    if (context->pipe_write >= 0) {
        if (bx_fd_dup2_exact(context->pipe_write, STDOUT_FILENO) < 0) {
            ash_exec_error(context->shell, "dup2", errno);
            return 1;
        }
        if (context->pipe_stderr &&
            bx_fd_dup2_exact(context->pipe_write, STDERR_FILENO) < 0) {
            ash_exec_error(context->shell, "dup2", errno);
            return 1;
        }
    }
    if (context->previous_read >= 0) {
        close(context->previous_read);
    }
    if (context->pipe_read >= 0) {
        close(context->pipe_read);
    }
    if (context->pipe_write >= 0) {
        close(context->pipe_write);
    }
    context->shell->should_exit = false;
    context->shell->control = (struct ash_control_state){0};
    return ash_execute_ast(context->shell, context->command);
}

static int ash_execute_ast_pipeline_forked(
    struct ash_shell* shell,
    const struct ash_ast* node
) {
    size_t command_count = node->value.pipeline.count;
    struct ash_job* job = ash_job_create(shell, ASH_JOB_PIPELINE, true);
    if (job == NULL) {
        ash_diag_oom(shell);
        return 1;
    }

    int previous_read = -1;
    for (size_t i = 0u; i < command_count; i++) {
        int pipe_fds[2] = {-1, -1};
        if (i + 1u < command_count &&
            bx_fd_pipe_cloexec(pipe_fds) != 0) {
            int error = errno;
            if (previous_read >= 0) {
                close(previous_read);
            }
            ash_job_abort(job);
            ash_exec_error(shell, "pipe", error);
            return 1;
        }

        struct ash_pipeline_child_context context = {
            .shell = shell,
            .command = node->value.pipeline.commands[i],
            .previous_read = previous_read,
            .pipe_read = pipe_fds[0],
            .pipe_write = pipe_fds[1],
            .pipe_stderr = i + 1u < command_count &&
                node->value.pipeline.operators[i] ==
                    ASH_PIPE_STDOUT_STDERR,
        };
        size_t process_index;
        if (ash_job_start_process(
                job,
                ASH_PROCESS_PIPELINE_MEMBER,
                ash_pipeline_child_main,
                &context,
                &process_index
            ) != 0) {
            int error = errno;
            if (previous_read >= 0) {
                close(previous_read);
            }
            if (pipe_fds[0] >= 0) {
                close(pipe_fds[0]);
                close(pipe_fds[1]);
            }
            ash_job_abort(job);
            ash_exec_error(shell, "fork", error);
            return 1;
        }
        (void)process_index;
        if (previous_read >= 0) {
            close(previous_read);
        }
        if (pipe_fds[1] >= 0) {
            close(pipe_fds[1]);
        }
        previous_read = pipe_fds[0];
    }
    if (previous_read >= 0) {
        close(previous_read);
    }

    if (!ash_job_commit(job, ASH_JOB_PRIVATE)) {
        ash_job_abort(job);
        ash_exec_error(shell, "child registration", EINVAL);
        return 1;
    }
    int status;
    if (ash_job_wait(job, &status) != 0) {
        int error = errno;
        ash_job_abort(job);
        ash_exec_error(shell, "waitpid", error);
        return 1;
    }
    (void)ash_job_release(job);
    return status;
}

static int ash_execute_ast_pipeline(
    struct ash_shell* shell,
    const struct ash_ast* node
) {
    int status;
    if (node->value.pipeline.count == 1u) {
        status = ash_execute_ast(shell, node->value.pipeline.commands[0]);
    }
    else {
        status = ash_execute_ast_pipeline_forked(shell, node);
    }

    return node->value.pipeline.negated ? (status == 0 ? 1 : 0) : status;
}

static int ash_execute_ast_and_or(
    struct ash_shell* shell,
    const struct ash_ast* node
) {
    int status = shell->last_status;
    for (size_t i = 0u; i < node->value.and_or.count; i++) {
        if (i != 0u) {
            enum ash_and_or_operator operator =
                node->value.and_or.operators[i - 1u];
            if ((operator == ASH_AND_IF && status != 0) ||
                (operator == ASH_OR_IF && status == 0)) {
                continue;
            }
        }
        status = ash_execute_ast(shell, node->value.and_or.pipelines[i]);
        shell->last_status = status;
        if (shell->should_exit || ash_control_pending(shell)) {
            break;
        }
    }
    return status;
}

struct ash_async_child_context {
    struct ash_shell* shell;
    const struct ash_ast* command;
};

static int ash_async_child_main(void* user_data) {
    struct ash_async_child_context* context = user_data;
    ash_shell_context_detach_after_fork(context->shell);
    if (!ash_shell_policy_has(
            &context->shell->policy,
            ASH_SHELL_POLICY_INTERACTIVE
        )) {
        int null_fd = bx_fd_open_cloexec(
            "/dev/null",
            O_RDONLY,
            0
        );
        if (null_fd >= 0) {
            (void)bx_fd_dup2_exact(null_fd, STDIN_FILENO);
            close(null_fd);
        }
    }
    context->shell->should_exit = false;
    return ash_execute_ast(context->shell, context->command);
}

static int ash_execute_ast_async(
    struct ash_shell* shell,
    const struct ash_ast* command
) {
    struct ash_job* job = ash_job_create(shell, ASH_JOB_ASYNC, false);
    if (job == NULL) {
        ash_diag_oom(shell);
        return 1;
    }
    struct ash_async_child_context context = {
        .shell = shell,
        .command = command,
    };
    size_t process_index;
    if (ash_job_start_process(
            job,
            ASH_PROCESS_ASYNC_COMMAND,
            ash_async_child_main,
            &context,
            &process_index
        ) != 0) {
        int error = errno;
        ash_job_abort(job);
        ash_exec_error(shell, "fork", error);
        return 1;
    }
    (void)process_index;
    if (!ash_job_commit(job, ASH_JOB_PUBLISHED)) {
        ash_job_abort(job);
        ash_exec_error(shell, "child registration", EINVAL);
        return 1;
    }
    shell->last_async_pid = ash_job_last_pid(job);
    return 0;
}

static int ash_execute_ast_list(
    struct ash_shell* shell,
    const struct ash_ast* node
) {
    int status = shell->last_status;
    for (size_t i = 0u; i < node->value.list.count; i++) {
        const struct ash_list_entry* entry = &node->value.list.entries[i];
        status = entry->asynchronous ?
            ash_execute_ast_async(shell, entry->command) :
            ash_execute_ast(shell, entry->command);
        shell->last_status = status;
        if (shell->should_exit || ash_control_pending(shell)) {
            break;
        }
    }
    return status;
}

static int ash_execute_ast_if(
    struct ash_shell* shell,
    const struct ash_ast* node
) {
    int condition = ash_execute_ast(shell, node->value.conditional.condition);
    if (shell->should_exit || ash_control_pending(shell)) {
        return condition;
    }
    if (condition == 0) {
        return ash_execute_ast(shell, node->value.conditional.then_branch);
    }
    if (node->value.conditional.else_branch != NULL) {
        return ash_execute_ast(shell, node->value.conditional.else_branch);
    }
    return 0;
}

static int ash_execute_ast_loop(
    struct ash_shell* shell,
    const struct ash_ast* node
) {
    int status = 0;
    ash_control_enter_loop(shell);
    while (!shell->should_exit) {
        int condition = ash_execute_ast(shell, node->value.loop.condition);
        if (shell->should_exit) {
            break;
        }
        if (ash_control_pending(shell)) {
            enum ash_loop_control control = ash_control_consume_loop(shell);
            if (control == ASH_LOOP_CONTROL_CONTINUE) {
                continue;
            }
            break;
        }
        bool run = (node->kind == ASH_AST_WHILE) ?
            condition == 0 : condition != 0;
        if (!run) {
            break;
        }
        status = ash_execute_ast(shell, node->value.loop.body);
        if (ash_control_pending(shell)) {
            enum ash_loop_control control = ash_control_consume_loop(shell);
            if (control == ASH_LOOP_CONTROL_CONTINUE) {
                continue;
            }
            break;
        }
    }
    ash_control_leave_loop(shell);
    return status;
}

static int ash_execute_ast_for(
    struct ash_shell* shell,
    const struct ash_ast* node
) {
    int status = 0;
    ash_control_enter_loop(shell);
    if (node->value.for_loop.explicit_words) {
        for (size_t i = 0u;
             i < node->value.for_loop.word_count && !shell->should_exit;
             i++) {
            struct ash_expanded_fields fields;
            if (!ash_expand_argument(
                    shell,
                    &node->value.for_loop.words[i].syntax,
                    &fields
                )) {
                status = 2;
                break;
            }
            bool stop = false;
            for (size_t j = 0u;
                 j < fields.count && !shell->should_exit;
                 j++) {
                if (!ash_var_set(
                        shell,
                        node->value.for_loop.name,
                        fields.values[j],
                        false
                    )) {
                    status = 2;
                    stop = true;
                    break;
                }
                status = ash_execute_ast(
                    shell,
                    node->value.for_loop.body
                );
                if (ash_control_pending(shell)) {
                    enum ash_loop_control control =
                        ash_control_consume_loop(shell);
                    if (control != ASH_LOOP_CONTROL_CONTINUE) {
                        stop = true;
                        break;
                    }
                }
            }
            ash_expanded_fields_destroy(&fields);
            if (stop) {
                break;
            }
        }
    }
    else {
        const struct ash_positional_frame* positionals =
            ash_scope_positionals(shell);
        size_t positional_count = positionals != NULL ?
            positionals->count : 0u;
        for (size_t i = 0u;
             i < positional_count && !shell->should_exit;
             i++) {
            if (!ash_var_set(
                    shell,
                    node->value.for_loop.name,
                    positionals->values[i],
                    false
                )) {
                status = 2;
                break;
            }
            status = ash_execute_ast(shell, node->value.for_loop.body);
            if (ash_control_pending(shell)) {
                enum ash_loop_control control =
                    ash_control_consume_loop(shell);
                if (control != ASH_LOOP_CONTROL_CONTINUE) {
                    break;
                }
            }
        }
    }
    ash_control_leave_loop(shell);
    return status;
}

static int ash_execute_ast_case_body(
    struct ash_shell* shell,
    const struct ash_ast* node
) {
    char* subject = NULL;
    if (!ash_expand_word(
            shell,
            &node->value.case_command.subject.syntax,
            &subject
        )) {
        return 2;
    }

    int status = 0;
    for (size_t i = 0u;
         i < node->value.case_command.clause_count;
         i++) {
        const struct ash_case_clause* clause =
            &node->value.case_command.clauses[i];
        bool clause_matched = false;
        for (size_t j = 0u; j < clause->pattern_count; j++) {
            struct ash_pattern pattern = {0};
            const struct ash_pattern_options pattern_options = {
                .purpose = ASH_PATTERN_CASE,
                .domain = ASH_PATTERN_STRING,
            };
            enum ash_pattern_compile_result compile_result =
                ash_pattern_compile_word(
                    shell,
                    &clause->patterns[j].syntax,
                    &pattern_options,
                    &pattern
                );
            if (compile_result != ASH_PATTERN_COMPILE_OK) {
                free(subject);
                return 2;
            }
            enum ash_pattern_match_result match_result =
                ash_pattern_match(&pattern, subject);
            ash_pattern_destroy(&pattern);
            if (match_result == ASH_PATTERN_MATCH_ERROR ||
                match_result == ASH_PATTERN_MATCH_UNSUPPORTED) {
                free(subject);
                return 2;
            }
            clause_matched = match_result == ASH_PATTERN_MATCH;
            if (clause_matched) {
                break;
            }
        }
        if (!clause_matched) {
            continue;
        }
        if (clause->body->value.list.count != 0u) {
            status = ash_execute_ast(shell, clause->body);
        }
        break;
    }
    free(subject);
    return status;
}

static int ash_execute_ast_case(
    struct ash_shell* shell,
    const struct ash_ast* node
) {
    struct ash_command redirections;
    if (!ash_ast_trailing_redirections_to_command(shell, node, &redirections)) {
        return 2;
    }
    struct ash_redirection_transaction saved;
    ash_redirection_transaction_init(&saved);
    if (ash_redirection_transaction_apply(
            shell,
            &redirections,
            &saved
        ) != 0) {
        ash_command_destroy(&redirections);
        return 1;
    }
    ash_command_destroy(&redirections);
    int status = ash_execute_ast_case_body(shell, node);
    return ash_redirection_transaction_rollback(shell, &saved) == 0 ?
        status : 1;
}

static int ash_execute_ast_function(
    struct ash_shell* shell,
    const struct ash_ast* node
) {
    return ash_function_define(
        shell,
        node->value.function.name,
        node->value.function.body
    ) ? 0 : 2;
}

static int ash_execute_ast(struct ash_shell* shell, const struct ash_ast* node) {
    ash_shell_context_assert_invariants(shell);
    int status = 2;
    switch (node->kind) {
        case ASH_AST_SIMPLE:
            status = ash_execute_ast_simple(shell, node);
            break;
        case ASH_AST_LIST:
            status = ash_execute_ast_list(shell, node);
            break;
        case ASH_AST_AND_OR:
            status = ash_execute_ast_and_or(shell, node);
            break;
        case ASH_AST_PIPELINE:
            status = ash_execute_ast_pipeline(shell, node);
            break;
        case ASH_AST_SUBSHELL:
            status = ash_execute_ast_group(shell, node, true);
            break;
        case ASH_AST_BRACE_GROUP:
            status = ash_execute_ast_group(shell, node, false);
            break;
        case ASH_AST_IF:
            status = ash_execute_ast_if(shell, node);
            break;
        case ASH_AST_WHILE:
        case ASH_AST_UNTIL:
            status = ash_execute_ast_loop(shell, node);
            break;
        case ASH_AST_FOR:
            status = ash_execute_ast_for(shell, node);
            break;
        case ASH_AST_CASE:
            status = ash_execute_ast_case(shell, node);
            break;
        case ASH_AST_FUNCTION:
            status = ash_execute_ast_function(shell, node);
            break;
        case ASH_AST_ARITHMETIC_COMMAND:
        case ASH_AST_CONDITIONAL_COMMAND:
        case ASH_AST_C_STYLE_FOR:
        case ASH_AST_SELECT:
        case ASH_AST_TIME:
        case ASH_AST_COPROC:
            ash_diag(shell, "shell construct is not implemented");
            status = 2;
            break;
    }
    ash_shell_context_assert_invariants(shell);
    return status;
}

static const char* ash_parser_diagnostic(const char* diagnostic) {
    if (diagnostic == NULL) {
        return "syntax error";
    }
    if (strcmp(diagnostic, "unterminated parameter expansion") == 0) {
        return "bad substitution";
    }
    if (strcmp(diagnostic, "command expected after pipe") == 0) {
        return "syntax error near unexpected token '|'";
    }
    if (strcmp(diagnostic, "redirection target expected") == 0 ||
        strcmp(diagnostic, "redirection target must be a word") == 0) {
        return "redirection requires a target";
    }
    return diagnostic;
}

static int ash_execute_buffer(
    struct ash_shell* shell,
    const char* input,
    bool final_input,
    bool* incomplete_out,
    bool* parser_error_out
) {
    *incomplete_out = false;
    *parser_error_out = false;

    struct ash_parser* parser = ash_shell_context_begin_parse(
        shell,
        ash_input_source_name(shell),
        input,
        strlen(input)
    );
    if (parser == NULL) {
        ash_diag(shell, "parser state is already active");
        *parser_error_out = true;
        shell->last_status = 2;
        return 2;
    }
    struct ash_ast* program = NULL;
    enum ash_parser_result result = ash_parser_parse_program(parser, &program);
    if (result == ASH_PARSER_INCOMPLETE && !final_input) {
        *incomplete_out = true;
        ash_shell_context_end_parse(shell);
        return shell->last_status;
    }
    if (result != ASH_PARSER_COMPLETE) {
        ash_diag(shell, "%s", ash_parser_diagnostic(parser->error));
        *parser_error_out = true;
        ash_shell_context_end_parse(shell);
        shell->last_status = 2;
        return 2;
    }

    ash_shell_context_end_parse(shell);
    int status = ash_execute_ast(shell, program);
    shell->last_status = status;
    ash_ast_destroy(program);
    return status;
}

static const char* ash_default_prompt(void) {
    return (geteuid() == 0) ? "# " : "$ ";
}

static void ash_print_prompt(struct ash_shell* shell, bool continuation) {
    const char* prompt = ash_var_get(shell, continuation ? "PS2" : "PS1");
    if (prompt == NULL) {
        prompt = continuation ? "> " : ash_default_prompt();
    }

    fputs(prompt, stdout);
    fflush(stdout);
}

static int ash_execute_input(struct ash_shell* shell, bool prompt) {
    char* line = NULL;
    size_t cap = 0;
    int status = shell->last_status;
    struct bx_text_buffer pending;
    bx_text_buffer_init(&pending);
    bool continuation = false;

    while (!shell->should_exit) {
        if (prompt) {
            ash_print_prompt(shell, continuation);
        }

        errno = 0;
        ssize_t nread = ash_input_read_line(shell, &line, &cap);
        if (nread < 0) {
            bool read_error = false;
            if (shell->input_stack != NULL &&
                shell->input_stack->kind == ASH_INPUT_FILE) {
                read_error = ferror(shell->input_stack->source.file.stream);
            }
            else {
                read_error = errno != 0;
            }
            if (read_error) {
                ash_exec_error(
                    shell,
                    "getline",
                    (errno != 0) ? errno : EIO
                );
                status = 1;
            }
            else if (pending.length != 0u) {
                bool incomplete = false;
                bool parser_error = false;
                status = ash_execute_buffer(
                    shell,
                    pending.data,
                    true,
                    &incomplete,
                    &parser_error
                );
                (void)incomplete;
            }
            break;
        }

        if (!bx_text_buffer_append_span(&pending, line, (size_t)nread)) {
            ash_diag_oom(shell);
            status = 2;
            break;
        }

        bool incomplete = false;
        bool parser_error = false;
        status = ash_execute_buffer(
            shell,
            pending.data,
            false,
            &incomplete,
            &parser_error
        );
        if (incomplete) {
            continuation = true;
            continue;
        }

        bx_text_buffer_clear(&pending);
        continuation = false;
        if (parser_error &&
            !ash_shell_policy_has(
                &shell->policy,
                ASH_SHELL_POLICY_INTERACTIVE
            )) {
            break;
        }
    }

    bx_text_buffer_destroy(&pending);
    free(line);
    return status;
}

static void ash_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [-Cis] [-c command] [script [arg ...]]\n", progname);
    fprintf(stream, "\n");
    fprintf(stream, "Minimal rescue shell applet.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -c command   run command string\n");
    fprintf(stream, "  -C           prevent output redirection from replacing files\n");
    fprintf(stream, "  -i           force interactive mode\n");
    fprintf(stream, "  -s           read commands from stdin\n");
    fprintf(stream, "  --help       display this help and exit\n");
    fprintf(stream, "  --version    output version information and exit\n");
}

static void ash_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

static bool ash_initialize_personality_variables(struct ash_shell* shell) {
    const char* bash_version = ash_shell_policy_bash_version(&shell->policy);
    if (bash_version == NULL) {
        return true;
    }

    /*
     * BASH_VERSION is shell-owned, not inherited process state. Recreate it
     * as an unexported variable after environment import. The variable model
     * will add readonly attributes in its dedicated compatibility phase.
     */
    ash_var_unset(shell, "BASH_VERSION");
    return ash_var_set(shell, "BASH_VERSION", bash_version, false);
}

int bx_ash_main(int argc, char** argv) {
    const char* invoked = ash_basename((argc > 0) ? argv[0] : "ash");
    const char* progname = ash_effective_name(invoked);
    bool login_shell = invoked[0] == '-';

    bool force_interactive = false;
    bool read_stdin = false;
    uint32_t initial_options = 0u;
    const char* command_string = NULL;

    int index = 1;
    while (index < argc) {
        const char* arg = argv[index];

        if (arg[0] != '-' || arg[1] == '\0') {
            break;
        }

        if (strcmp(arg, "--") == 0) {
            index++;
            break;
        }

        if (strcmp(arg, "--help") == 0) {
            ash_print_help(stdout, progname);
            return 0;
        }

        if (strcmp(arg, "--version") == 0) {
            ash_print_version(progname);
            return 0;
        }

        const char* opt = arg + 1;
        bool consumed_command = false;
        while (*opt != '\0') {
            if (*opt == 'i') {
                force_interactive = true;
                opt++;
                continue;
            }

            if (*opt == 's') {
                read_stdin = true;
                opt++;
                continue;
            }
            if (*opt == 'C') {
                initial_options |= ASH_SHELL_OPTION_NOCLOBBER;
                opt++;
                continue;
            }

            if (*opt == 'c') {
                opt++;
                if (*opt != '\0') {
                    command_string = opt;
                }
                else {
                    if (index + 1 >= argc) {
                        fprintf(stderr, "%s: option requires an argument -- 'c'\n", progname);
                        return 2;
                    }
                    command_string = argv[index + 1];
                    index++;
                }
                consumed_command = true;
                break;
            }

            fprintf(stderr, "%s: unknown option -- '%c'\n", progname, *opt);
            return 2;
        }

        index++;

        if (consumed_command) {
            break;
        }
    }

    const char* script_path = NULL;
    if (command_string == NULL && !read_stdin && index < argc) {
        script_path = argv[index++];
    }

    const char* argv0_param = invoked;
    char** positional_args = NULL;
    int positional_count = 0;

    if (command_string != NULL) {
        if (index < argc) {
            argv0_param = argv[index++];
        }
        positional_args = argv + index;
        positional_count = argc - index;
    }
    else if (script_path != NULL) {
        argv0_param = script_path;
        positional_args = argv + index;
        positional_count = argc - index;
    }
    else {
        positional_args = argv + index;
        positional_count = argc - index;
    }

    bool interactive = force_interactive;
    if (!interactive && command_string == NULL && script_path == NULL) {
        interactive = isatty(STDIN_FILENO);
    }
    uint32_t policy_flags = 0u;
    if (interactive) {
        policy_flags |= ASH_SHELL_POLICY_INTERACTIVE;
    }
    if (login_shell) {
        policy_flags |= ASH_SHELL_POLICY_LOGIN;
    }
    struct ash_shell_policy policy;
    if (!ash_shell_policy_for_invocation(
            progname,
            policy_flags,
            &policy
        )) {
        fprintf(stderr, "%s: unsupported shell invocation\n", progname);
        return 2;
    }

    struct ash_shell shell;
    const struct ash_shell_context_config context_config = {
        .progname = progname,
        .argv0 = argv0_param,
        .positional_values = positional_args,
        .positional_count = positional_count,
        .options = initial_options |
            (read_stdin ? ASH_SHELL_OPTION_STDIN : 0u),
        .policy = policy,
        .shell_pid = getpid(),
        .command_substitution = ash_command_substitute,
    };
    if (!ash_shell_context_init(&shell, &context_config)) {
        fprintf(stderr, "%s: invalid shell context configuration\n", progname);
        return 2;
    }

    if (!ash_import_environment(&shell)) {
        ash_shell_context_release_owned(&shell);
        return 1;
    }
    if (!ash_initialize_personality_variables(&shell)) {
        ash_shell_context_release_owned(&shell);
        return 1;
    }

    if (ash_shell_policy_has(
            &shell.policy,
            ASH_SHELL_POLICY_INTERACTIVE
        ) &&
        !ash_var_exists(&shell, "PS1")) {
        if (!ash_var_set(&shell, "PS1", ash_default_prompt(), false)) {
            ash_shell_context_release_owned(&shell);
            return 1;
        }
    }

    int status = 0;
    if (command_string != NULL) {
        if (!ash_input_push_string(&shell, "-c", command_string)) {
            ash_diag_oom(&shell);
            ash_shell_context_release_owned(&shell);
            return 1;
        }
        status = ash_execute_input(&shell, false);
        ash_input_pop(&shell);
    }
    else if (script_path != NULL) {
        FILE* script = fopen(script_path, "r");
        if (script == NULL) {
            ash_exec_error(&shell, script_path, errno);
            ash_shell_context_release_owned(&shell);
            return 1;
        }

        if (!ash_input_push_file(
                &shell,
                script_path,
                script,
                ASH_INPUT_TAKE_STREAM
            )) {
            ash_diag_oom(&shell);
            fclose(script);
            ash_shell_context_release_owned(&shell);
            return 1;
        }
        status = ash_execute_input(
            &shell,
            ash_shell_policy_has(
                &shell.policy,
                ASH_SHELL_POLICY_INTERACTIVE
            ) && ash_input_source_is_terminal(&shell)
        );
        ash_input_pop(&shell);
    }
    else {
        if (!ash_input_push_file(
                &shell,
                "<stdin>",
                stdin,
                ASH_INPUT_BORROW_STREAM
            )) {
            ash_diag_oom(&shell);
            ash_shell_context_release_owned(&shell);
            return 1;
        }
        status = ash_execute_input(
            &shell,
            ash_shell_policy_has(
                &shell.policy,
                ASH_SHELL_POLICY_INTERACTIVE
            )
        );
        ash_input_pop(&shell);
    }

    if (shell.should_exit) {
        status = shell.requested_exit_status;
    }

    ash_shell_context_release_owned(&shell);
    return status;
}
