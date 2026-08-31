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
#include <sys/wait.h>
#include <unistd.h>

#include "applets.h"
#include "applets/shell/ash/command_resolution.h"
#include "applets/shell/ash/diagnostic.h"
#include "applets/shell/ash/expansion.h"
#include "applets/shell/ash/functions.h"
#include "applets/shell/ash/input.h"
#include "applets/shell/ash/lexer.h"
#include "applets/shell/ash/pattern.h"
#include "applets/shell/ash/parser.h"
#include "applets/shell/ash/shell_context.h"
#include "applets/shell/ash/variables.h"
#include "lib/cli_common.h"
#include "lib/fd_ops.h"
#include "lib/text_buffer.h"

extern char** environ;

enum ash_redir_kind {
    ASH_REDIR_IN = 0,
    ASH_REDIR_OUT,
    ASH_REDIR_CLOBBER,
    ASH_REDIR_APPEND,
    ASH_REDIR_READWRITE,
    ASH_REDIR_DUP,
};

struct ash_redir {
    int fd;
    enum ash_redir_kind kind;
    char* target;
};

struct ash_command {
    char** words;
    size_t word_count;
    size_t word_cap;

    char** assignments;
    size_t assignment_count;
    size_t assignment_cap;

    struct ash_redir* redirs;
    size_t redir_count;
    size_t redir_cap;
};

struct ash_pipeline {
    struct ash_command* commands;
    size_t count;
    size_t cap;
};

struct ash_saved_fd {
    int target_fd;
    int saved_fd;
};

struct ash_saved_fds {
    struct ash_saved_fd* items;
    size_t len;
    size_t cap;
};

static int ash_execute_ast(struct ash_shell* shell, const struct ash_ast* node);

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

static bool ash_command_is_empty(const struct ash_command* command) {
    return command->word_count == 0u && command->assignment_count == 0u && command->redir_count == 0u;
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

static void ash_pipeline_init(struct ash_pipeline* pipeline) {
    pipeline->commands = NULL;
    pipeline->count = 0;
    pipeline->cap = 0;
}

static bool ash_pipeline_push_command(const struct ash_shell* shell, struct ash_pipeline* pipeline, const struct ash_command* command) {
    if (pipeline->count == pipeline->cap) {
        size_t new_cap = (pipeline->cap == 0) ? 4u : pipeline->cap * 2u;
        if (pipeline->cap != 0u && pipeline->cap > SIZE_MAX / 2u) {
            return ash_diag_oom(shell);
        }
        struct ash_command* grown = ash_realloc_array(shell, pipeline->commands, new_cap, sizeof(*pipeline->commands));
        if (grown == NULL) {
            return false;
        }
        pipeline->commands = grown;
        pipeline->cap = new_cap;
    }

    pipeline->commands[pipeline->count++] = *command;
    return true;
}

static void ash_pipeline_destroy(struct ash_pipeline* pipeline) {
    for (size_t i = 0; i < pipeline->count; i++) {
        ash_command_destroy(&pipeline->commands[i]);
    }
    free(pipeline->commands);
    pipeline->commands = NULL;
    pipeline->count = 0;
    pipeline->cap = 0;
}

static void ash_saved_fds_init(struct ash_saved_fds* saved) {
    saved->items = NULL;
    saved->len = 0;
    saved->cap = 0;
}

static bool ash_saved_fds_has_target(const struct ash_saved_fds* saved, int fd) {
    for (size_t i = 0; i < saved->len; i++) {
        if (saved->items[i].target_fd == fd) {
            return true;
        }
    }
    return false;
}

static bool ash_saved_fds_push(const struct ash_shell* shell, struct ash_saved_fds* saved, int target_fd, int saved_fd) {
    if (saved->len == saved->cap) {
        size_t new_cap = (saved->cap == 0) ? 4u : saved->cap * 2u;
        if (saved->cap != 0u && saved->cap > SIZE_MAX / 2u) {
            return ash_diag_oom(shell);
        }
        struct ash_saved_fd* grown = ash_realloc_array(shell, saved->items, new_cap, sizeof(*saved->items));
        if (grown == NULL) {
            return false;
        }
        saved->items = grown;
        saved->cap = new_cap;
    }

    saved->items[saved->len].target_fd = target_fd;
    saved->items[saved->len].saved_fd = saved_fd;
    saved->len++;
    return true;
}

static void ash_saved_fds_restore(const struct ash_shell* shell, struct ash_saved_fds* saved) {
    for (size_t idx = saved->len; idx > 0; idx--) {
        struct ash_saved_fd item = saved->items[idx - 1u];
        if (item.saved_fd >= 0) {
            if (bx_fd_dup2_exact(item.saved_fd, item.target_fd) < 0) {
                ash_exec_error(shell, "dup2", errno);
            }
            close(item.saved_fd);
        }
        else {
            close(item.target_fd);
        }
    }

    free(saved->items);
    saved->items = NULL;
    saved->len = 0;
    saved->cap = 0;
}

static void ash_saved_fds_destroy(struct ash_saved_fds* saved) {
    for (size_t i = 0; i < saved->len; i++) {
        if (saved->items[i].saved_fd >= 0) {
            close(saved->items[i].saved_fd);
        }
    }
    free(saved->items);
    saved->items = NULL;
    saved->len = 0;
    saved->cap = 0;
}

static bool ash_parse_lexed_io_number(
    const struct ash_shell* shell,
    const char* text,
    int* fd_out
) {
    char* end = NULL;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < 0 || value > INT_MAX) {
        ash_diag(shell, "invalid redirection fd");
        return false;
    }
    *fd_out = (int)value;
    return true;
}

static int ash_open_redirection(
    const struct ash_shell* shell,
    const struct ash_redir* redir
) {
    if (redir->kind == ASH_REDIR_OUT &&
        (shell->options & ASH_SHELL_OPTION_NOCLOBBER) != 0u) {
        int fd = bx_fd_open_cloexec(
            redir->target,
            O_WRONLY | O_CREAT | O_EXCL,
            0666
        );
        if (fd >= 0 || errno != EEXIST) {
            return fd;
        }

        fd = bx_fd_open_cloexec(redir->target, O_WRONLY, 0);
        if (fd < 0) {
            return -1;
        }
        struct stat status;
        if (fstat(fd, &status) != 0) {
            int error = errno;
            close(fd);
            errno = error;
            return -1;
        }
        if (S_ISREG(status.st_mode)) {
            close(fd);
            errno = EEXIST;
            return -1;
        }
        return fd;
    }

    int flags;
    switch (redir->kind) {
        case ASH_REDIR_IN:
            flags = O_RDONLY;
            break;
        case ASH_REDIR_OUT:
        case ASH_REDIR_CLOBBER:
            flags = O_WRONLY | O_CREAT | O_TRUNC;
            break;
        case ASH_REDIR_APPEND:
            flags = O_WRONLY | O_CREAT | O_APPEND;
            break;
        case ASH_REDIR_READWRITE:
            flags = O_RDWR | O_CREAT;
            break;
        case ASH_REDIR_DUP:
            errno = EINVAL;
            return -1;
    }
    return bx_fd_open_cloexec(redir->target, flags, 0666);
}

static int ash_apply_redirections(const struct ash_shell* shell, const struct ash_command* command, struct ash_saved_fds* saved) {
    int minimum_saved_fd = 10;
    for (size_t i = 0u; i < command->redir_count; i++) {
        if (command->redirs[i].fd >= minimum_saved_fd &&
            command->redirs[i].fd < INT_MAX) {
            minimum_saved_fd = command->redirs[i].fd + 1;
        }
    }
    for (size_t i = 0; i < command->redir_count; i++) {
        const struct ash_redir* redir = &command->redirs[i];

        if (saved != NULL && !ash_saved_fds_has_target(saved, redir->fd)) {
            int dup_fd = bx_fd_dup_cloexec_min(
                redir->fd,
                minimum_saved_fd
            );
            if (dup_fd < 0 && errno != EBADF) {
                ash_exec_error(shell, "dup", errno);
                return 1;
            }
            if (!ash_saved_fds_push(shell, saved, redir->fd, dup_fd)) {
                if (dup_fd >= 0) {
                    close(dup_fd);
                }
                return 1;
            }
        }

        if (redir->kind == ASH_REDIR_DUP) {
            if (strcmp(redir->target, "-") == 0) {
                if (close(redir->fd) != 0 && errno != EBADF) {
                    ash_exec_error(shell, "close", errno);
                    return 1;
                }
                continue;
            }
            int source_fd;
            if (!ash_parse_lexed_io_number(
                    shell,
                    redir->target,
                    &source_fd
                )) {
                return 1;
            }
            if (bx_fd_dup2_exact(source_fd, redir->fd) < 0) {
                ash_exec_error(shell, redir->target, errno);
                return 1;
            }
            continue;
        }

        int fd = ash_open_redirection(shell, redir);
        if (fd < 0) {
            ash_exec_error(shell, redir->target, errno);
            return 1;
        }

        if (fd == redir->fd) {
            if (bx_fd_set_cloexec(fd, false) != 0) {
                int err = errno;
                close(fd);
                ash_exec_error(shell, "fcntl", err);
                return 1;
            }
        }
        else {
            if (bx_fd_dup2_exact(fd, redir->fd) < 0) {
                int err = errno;
                close(fd);
                ash_exec_error(shell, "dup2", err);
                return 1;
            }
            close(fd);
        }
    }

    return 0;
}

static int ash_wait_status_to_exit_status(int wait_status) {
    if (WIFEXITED(wait_status)) {
        return WEXITSTATUS(wait_status);
    }
    if (WIFSIGNALED(wait_status)) {
        return 128 + WTERMSIG(wait_status);
    }
    return 1;
}

static int ash_exec_external(struct ash_shell* shell, char** argv) {
    if (argv == NULL || argv[0] == NULL) {
        return 0;
    }

    const char* command = argv[0];

    if (strchr(command, '/') != NULL) {
        execve(command, argv, environ);
        int err = errno;
        ash_exec_error(shell, command, err);
        return (err == ENOENT) ? 127 : 126;
    }

    const char* path = ash_var_get(shell, "PATH");
    if (path == NULL || path[0] == '\0') {
        path = "/bin:/usr/bin";
    }

    int best_error = ENOENT;
    bool saw_eacces = false;

    const char* segment = path;
    while (true) {
        size_t dir_len = strcspn(segment, ":");
        bool has_colon = (segment[dir_len] == ':');

        struct bx_text_buffer candidate;
        bx_text_buffer_init(&candidate);

        if (dir_len == 0u) {
            if (!bx_text_buffer_append_text(&candidate, "./")) {
                ash_diag_oom(shell);
                bx_text_buffer_destroy(&candidate);
                return 126;
            }
        }
        else {
            if (!bx_text_buffer_append_span(&candidate, segment, dir_len) ||
                !bx_text_buffer_append_char(&candidate, '/')) {
                ash_diag_oom(shell);
                bx_text_buffer_destroy(&candidate);
                return 126;
            }
        }
        if (!bx_text_buffer_append_text(&candidate, command)) {
            ash_diag_oom(shell);
            bx_text_buffer_destroy(&candidate);
            return 126;
        }

        char* candidate_path = bx_text_buffer_take(&candidate);
        if (candidate_path == NULL) {
            ash_diag_oom(shell);
            bx_text_buffer_destroy(&candidate);
            return 126;
        }
        execve(candidate_path, argv, environ);

        int err = errno;
        if (err != ENOENT && err != ENOTDIR) {
            if (err == EACCES) {
                saw_eacces = true;
            }
            if (best_error == ENOENT || best_error == ENOTDIR) {
                best_error = err;
            }
        }

        free(candidate_path);

        if (!has_colon) {
            break;
        }
        segment += dir_len + 1u;
    }

    if (saw_eacces) {
        ash_exec_error(shell, command, EACCES);
        return 126;
    }

    if (best_error != ENOENT && best_error != ENOTDIR) {
        ash_exec_error(shell, command, best_error);
        return 126;
    }

    ash_exec_not_found(shell, command);
    return 127;
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
        for (const struct ash_var* var = shell->vars; var != NULL; var = var->next) {
            if (var->exported) {
                printf("%s=%s\n", var->name, var->value);
            }
        }
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

    for (size_t i = 1; i < command->word_count; i++) {
        const char* name = command->words[i];
        size_t len = strlen(name);
        if (!ash_is_valid_name_span(name, len)) {
            ash_diag(shell, "unset: invalid name '%s'", name);
            status = 1;
            continue;
        }
        ash_var_unset(shell, name);
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
    int status = ash_exec_external(shell, argv);
    return status;
}

static int ash_builtin_set(struct ash_shell* shell, const struct ash_command* command) {
    if (command->word_count == 1u) {
        for (const struct ash_var* var = shell->vars; var != NULL; var = var->next) {
            printf("%s=%s\n", var->name, var->value);
        }
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
        case ASH_BUILTIN_INVALID:
            break;
    }

    return 1;
}

static struct ash_command_resolution ash_resolve_shell_command(
    const struct ash_shell* shell,
    const char* name
) {
    struct ash_command_resolution builtin = ash_command_resolve_builtin(name);
    if (builtin.kind == ASH_COMMAND_SPECIAL_BUILTIN) {
        return builtin;
    }
    const struct ash_function* function = ash_function_find(shell, name);
    if (function != NULL) {
        return ash_command_resolution_function(name, function);
    }
    return builtin;
}

static void ash_restore_function_assignments(
    struct ash_shell* shell,
    struct ash_var_saved* saved,
    size_t count
) {
    while (count != 0u) {
        ash_var_restore(shell, &saved[--count]);
    }
    free(saved);
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

    struct ash_var_saved* saved = ash_realloc_array(
        shell,
        NULL,
        command->assignment_count,
        sizeof(*saved)
    );
    if (command->assignment_count != 0u && saved == NULL) {
        ash_ast_destroy(invocation_body);
        return 2;
    }
    size_t saved_count = 0u;
    for (size_t i = 0u; i < command->assignment_count; i++) {
        size_t name_length = 0u;
        const char* value = NULL;
        if (!ash_parse_assignment(
                command->assignments[i],
                &name_length,
                &value
            ) ||
            !ash_var_save(
                shell,
                command->assignments[i],
                name_length,
                &saved[saved_count]
            )) {
            ash_restore_function_assignments(shell, saved, saved_count);
            ash_ast_destroy(invocation_body);
            return 2;
        }
        saved_count++;
        if (!ash_var_set_with_export(
                shell,
                command->assignments[i],
                name_length,
                value,
                true
            )) {
            ash_restore_function_assignments(shell, saved, saved_count);
            ash_ast_destroy(invocation_body);
            return 2;
        }
    }

    struct ash_saved_fds saved_fds;
    ash_saved_fds_init(&saved_fds);
    if (ash_apply_redirections(shell, command, &saved_fds) != 0) {
        ash_saved_fds_restore(shell, &saved_fds);
        ash_restore_function_assignments(shell, saved, saved_count);
        ash_ast_destroy(invocation_body);
        return 1;
    }

    struct ash_positional_frame previous = shell->positionals;
    shell->positionals = (struct ash_positional_frame){
        .argv0 = previous.argv0,
        .values = command->word_count > 1u ? &command->words[1] : NULL,
        .count = command->word_count > 1u ?
            (int)(command->word_count - 1u) : 0,
        .previous = &previous,
    };
    unsigned int caller_loop_depth = shell->control.loop_depth;
    shell->control.loop_depth = 0u;
    ash_control_enter_function(shell);
    int status = ash_execute_ast(shell, invocation_body);
    (void)ash_control_consume_return(shell, &status);
    ash_control_leave_function(shell);
    shell->control.loop_depth = caller_loop_depth;
    shell->positionals = previous;

    ash_saved_fds_restore(shell, &saved_fds);
    ash_restore_function_assignments(shell, saved, saved_count);
    ash_ast_destroy(invocation_body);
    return status;
}

static int ash_execute_in_child(struct ash_shell* shell, const struct ash_command* command) {
    if (command->word_count != 0u) {
        struct ash_command_resolution resolution =
            ash_resolve_shell_command(shell, command->words[0]);
        if (resolution.kind == ASH_COMMAND_FUNCTION) {
            return ash_execute_function(
                shell,
                resolution.target.function,
                command
            );
        }
    }
    if (ash_apply_redirections(shell, command, NULL) != 0) {
        return 1;
    }

    if (command->word_count == 0u) {
        if (ash_apply_command_assignments_shell(shell, command) != 0) {
            return 1;
        }
        return 0;
    }

    struct ash_command_resolution resolution =
        ash_resolve_shell_command(shell, command->words[0]);
    if (ash_command_resolution_is_builtin(&resolution)) {
        if (ash_apply_command_assignments_shell(shell, command) != 0) {
            return 1;
        }
        return ash_run_builtin(shell, resolution.target.builtin, command, true);
    }

    if (ash_apply_command_assignments_env(shell, command) != 0) {
        return 1;
    }

    return ash_exec_external(shell, command->words);
}

static int ash_execute_single_command_parent(struct ash_shell* shell, const struct ash_command* command, enum ash_builtin_kind builtin) {
    if (ash_apply_command_assignments_shell(shell, command) != 0) {
        return 1;
    }

    struct ash_saved_fds saved;
    ash_saved_fds_init(&saved);

    if (ash_apply_redirections(shell, command, &saved) != 0) {
        ash_saved_fds_restore(shell, &saved);
        return 1;
    }

    int status = ash_run_builtin(shell, builtin, command, false);

    if (!shell->should_exit) {
        ash_saved_fds_restore(shell, &saved);
    }
    else {
        ash_saved_fds_destroy(&saved);
    }

    return status;
}

static int ash_execute_single_command_forked(struct ash_shell* shell, const struct ash_command* command) {
    pid_t pid = fork();
    if (pid < 0) {
        ash_exec_error(shell, "fork", errno);
        return 1;
    }

    if (pid == 0) {
        int status = ash_execute_in_child(shell, command);
        _exit(status);
    }

    int wait_status = 0;
    if (waitpid(pid, &wait_status, 0) < 0) {
        ash_exec_error(shell, "waitpid", errno);
        return 1;
    }

    return ash_wait_status_to_exit_status(wait_status);
}

static int ash_execute_pipeline_forked(struct ash_shell* shell, const struct ash_pipeline* pipeline) {
    size_t command_count = pipeline->count;
    pid_t* pids = ash_realloc_array(shell, NULL, command_count, sizeof(*pids));
    if (pids == NULL) {
        return 1;
    }

    size_t spawned = 0;
    int prev_read = -1;

    for (size_t i = 0; i < command_count; i++) {
        int pipe_fds[2] = {-1, -1};

        if (i + 1u < command_count) {
            if (bx_fd_pipe_cloexec(pipe_fds) != 0) {
                ash_exec_error(shell, "pipe", errno);
                if (prev_read >= 0) {
                    close(prev_read);
                }
                for (size_t j = 0; j < spawned; j++) {
                    int ignored_status = 0;
                    waitpid(pids[j], &ignored_status, 0);
                }
                free(pids);
                return 1;
            }
        }

        pid_t pid = fork();
        if (pid < 0) {
            ash_exec_error(shell, "fork", errno);
            if (pipe_fds[0] >= 0) {
                close(pipe_fds[0]);
            }
            if (pipe_fds[1] >= 0) {
                close(pipe_fds[1]);
            }
            if (prev_read >= 0) {
                close(prev_read);
            }
            for (size_t j = 0; j < spawned; j++) {
                int ignored_status = 0;
                waitpid(pids[j], &ignored_status, 0);
            }
            free(pids);
            return 1;
        }

        if (pid == 0) {
            if (prev_read >= 0) {
                if (bx_fd_dup2_exact(prev_read, STDIN_FILENO) < 0) {
                    ash_exec_error(shell, "dup2", errno);
                    _exit(1);
                }
            }

            if (pipe_fds[1] >= 0) {
                if (bx_fd_dup2_exact(pipe_fds[1], STDOUT_FILENO) < 0) {
                    ash_exec_error(shell, "dup2", errno);
                    _exit(1);
                }
            }

            if (pipe_fds[0] >= 0) {
                close(pipe_fds[0]);
            }
            if (pipe_fds[1] >= 0) {
                close(pipe_fds[1]);
            }
            if (prev_read >= 0) {
                close(prev_read);
            }

            int status = ash_execute_in_child(shell, &pipeline->commands[i]);
            _exit(status);
        }

        pids[spawned++] = pid;

        if (prev_read >= 0) {
            close(prev_read);
        }
        if (pipe_fds[1] >= 0) {
            close(pipe_fds[1]);
        }

        prev_read = pipe_fds[0];
    }

    if (prev_read >= 0) {
        close(prev_read);
    }

    int last_status = 0;
    pid_t last_pid = pids[command_count - 1u];

    for (size_t i = 0; i < command_count; i++) {
        int wait_status = 0;
        if (waitpid(pids[i], &wait_status, 0) < 0) {
            ash_exec_error(shell, "waitpid", errno);
            free(pids);
            return 1;
        }

        if (pids[i] == last_pid) {
            last_status = ash_wait_status_to_exit_status(wait_status);
        }
    }

    free(pids);
    return last_status;
}

static int ash_execute_pipeline(struct ash_shell* shell, const struct ash_pipeline* pipeline) {
    if (pipeline->count == 0u) {
        return shell->last_status;
    }

    if (pipeline->count == 1u) {
        const struct ash_command* command = &pipeline->commands[0];

        if (command->word_count == 0u) {
            if (ash_apply_command_assignments_shell(shell, command) != 0) {
                return 1;
            }
            if (command->redir_count == 0u) {
                return 0;
            }

            struct ash_saved_fds saved;
            ash_saved_fds_init(&saved);
            if (ash_apply_redirections(shell, command, &saved) != 0) {
                ash_saved_fds_restore(shell, &saved);
                return 1;
            }
            ash_saved_fds_restore(shell, &saved);
            return 0;
        }

        struct ash_command_resolution resolution =
            ash_resolve_shell_command(shell, command->words[0]);
        if (resolution.kind == ASH_COMMAND_FUNCTION) {
            return ash_execute_function(
                shell,
                resolution.target.function,
                command
            );
        }
        if (ash_command_resolution_is_builtin(&resolution)) {
            return ash_execute_single_command_parent(shell, command, resolution.target.builtin);
        }

        return ash_execute_single_command_forked(shell, command);
    }

    return ash_execute_pipeline_forked(shell, pipeline);
}

static int ash_execute_buffer(
    struct ash_shell* shell,
    const char* input,
    bool final_input,
    bool* incomplete_out,
    bool* parser_error_out
);

static bool ash_command_substitute(
    struct ash_shell* shell,
    const char* command,
    size_t length,
    char** output
) {
    int pipe_fds[2];
    if (bx_fd_pipe_cloexec(pipe_fds) != 0) {
        ash_exec_error(shell, "pipe", errno);
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        int error = errno;
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        ash_exec_error(shell, "fork", error);
        return false;
    }
    if (pid == 0) {
        close(pipe_fds[0]);
        if (bx_fd_dup2_exact(pipe_fds[1], STDOUT_FILENO) < 0) {
            ash_exec_error(shell, "dup2", errno);
            _exit(1);
        }
        close(pipe_fds[1]);

        char* input = ash_slice_dup(shell, command, length);
        if (input == NULL) {
            _exit(2);
        }
        struct ash_shell child = *shell;
        child.should_exit = false;
        child.requested_exit_status = 0;
        bool incomplete = false;
        bool parser_error = false;
        int status = ash_execute_buffer(
            &child,
            input,
            true,
            &incomplete,
            &parser_error
        );
        free(input);
        _exit(status);
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

    int wait_status;
    while (waitpid(pid, &wait_status, 0) < 0) {
        if (errno == EINTR) {
            continue;
        }
        ash_exec_error(shell, "waitpid", errno);
        bx_text_buffer_destroy(&captured);
        return false;
    }
    shell->last_status = ash_wait_status_to_exit_status(wait_status);
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
        if (!ash_parse_lexed_io_number(shell, redirection->io_number, &fd)) {
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
    if (!ash_expand_word(shell, &redirection->target, &target)) {
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
            if (!ash_expand_word(shell, &item->value.word, &text)) {
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
                    &item->value.word,
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

    struct ash_pipeline pipeline;
    ash_pipeline_init(&pipeline);
    if (!ash_pipeline_push_command(shell, &pipeline, &command)) {
        ash_command_destroy(&command);
        ash_pipeline_destroy(&pipeline);
        return 2;
    }

    int status = ash_execute_pipeline(shell, &pipeline);
    ash_pipeline_destroy(&pipeline);
    return status;
}

static int ash_execute_ast_group(
    struct ash_shell* shell,
    const struct ash_ast* node,
    bool subshell
) {
    if (subshell) {
        pid_t pid = fork();
        if (pid < 0) {
            ash_exec_error(shell, "fork", errno);
            return 1;
        }
        if (pid == 0) {
            struct ash_command redirections;
            if (!ash_ast_trailing_redirections_to_command(
                    shell,
                    node,
                    &redirections
                )) {
                _exit(2);
            }
            struct ash_saved_fds saved;
            ash_saved_fds_init(&saved);
            if (ash_apply_redirections(shell, &redirections, &saved) != 0) {
                ash_command_destroy(&redirections);
                ash_saved_fds_destroy(&saved);
                _exit(1);
            }
            ash_command_destroy(&redirections);
            int status = ash_execute_ast(shell, node->value.group.body);
            ash_saved_fds_destroy(&saved);
            _exit(status);
        }

        int wait_status;
        while (waitpid(pid, &wait_status, 0) < 0) {
            if (errno == EINTR) {
                continue;
            }
            ash_exec_error(shell, "waitpid", errno);
            return 1;
        }
        return ash_wait_status_to_exit_status(wait_status);
    }

    struct ash_command redirections;
    if (!ash_ast_trailing_redirections_to_command(shell, node, &redirections)) {
        return 2;
    }
    struct ash_saved_fds saved;
    ash_saved_fds_init(&saved);
    if (ash_apply_redirections(shell, &redirections, &saved) != 0) {
        ash_command_destroy(&redirections);
        ash_saved_fds_restore(shell, &saved);
        return 1;
    }
    ash_command_destroy(&redirections);
    int status = ash_execute_ast(shell, node->value.group.body);
    ash_saved_fds_restore(shell, &saved);
    return status;
}

static void ash_reap_spawned_pipeline(
    const pid_t* pids,
    size_t count,
    bool terminate
) {
    if (terminate) {
        for (size_t i = 0u; i < count; i++) {
            (void)kill(pids[i], SIGTERM);
        }
    }
    for (size_t i = 0u; i < count; i++) {
        int ignored;
        while (waitpid(pids[i], &ignored, 0) < 0 && errno == EINTR) {
        }
    }
}

static int ash_execute_ast_pipeline_forked(
    struct ash_shell* shell,
    const struct ash_ast* node
) {
    size_t command_count = node->value.pipeline.count;
    pid_t* pids = ash_realloc_array(
        shell,
        NULL,
        command_count,
        sizeof(*pids)
    );
    if (pids == NULL) {
        return 1;
    }

    size_t spawned = 0u;
    int previous_read = -1;
    for (size_t i = 0u; i < command_count; i++) {
        int pipe_fds[2] = {-1, -1};
        if (i + 1u < command_count &&
            bx_fd_pipe_cloexec(pipe_fds) != 0) {
            ash_exec_error(shell, "pipe", errno);
            if (previous_read >= 0) {
                close(previous_read);
            }
            ash_reap_spawned_pipeline(pids, spawned, true);
            free(pids);
            return 1;
        }

        pid_t pid = fork();
        if (pid < 0) {
            ash_exec_error(shell, "fork", errno);
            if (previous_read >= 0) {
                close(previous_read);
            }
            if (pipe_fds[0] >= 0) {
                close(pipe_fds[0]);
                close(pipe_fds[1]);
            }
            ash_reap_spawned_pipeline(pids, spawned, true);
            free(pids);
            return 1;
        }
        if (pid == 0) {
            if (previous_read >= 0 &&
                bx_fd_dup2_exact(previous_read, STDIN_FILENO) < 0) {
                ash_exec_error(shell, "dup2", errno);
                _exit(1);
            }
            if (pipe_fds[1] >= 0) {
                if (bx_fd_dup2_exact(pipe_fds[1], STDOUT_FILENO) < 0) {
                    ash_exec_error(shell, "dup2", errno);
                    _exit(1);
                }
                if (node->value.pipeline.operators[i] ==
                        ASH_PIPE_STDOUT_STDERR &&
                    bx_fd_dup2_exact(pipe_fds[1], STDERR_FILENO) < 0) {
                    ash_exec_error(shell, "dup2", errno);
                    _exit(1);
                }
            }
            if (previous_read >= 0) {
                close(previous_read);
            }
            if (pipe_fds[0] >= 0) {
                close(pipe_fds[0]);
                close(pipe_fds[1]);
            }
            shell->should_exit = false;
            shell->control = (struct ash_control_state){0};
            _exit(ash_execute_ast(
                shell,
                node->value.pipeline.commands[i]
            ));
        }

        pids[spawned++] = pid;
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

    int last_status = 1;
    for (size_t i = 0u; i < command_count; i++) {
        int wait_status;
        while (waitpid(pids[i], &wait_status, 0) < 0) {
            if (errno == EINTR) {
                continue;
            }
            ash_exec_error(shell, "waitpid", errno);
            ash_reap_spawned_pipeline(
                pids + i + 1u,
                command_count - i - 1u,
                true
            );
            free(pids);
            return 1;
        }
        if (i + 1u == command_count) {
            last_status = ash_wait_status_to_exit_status(wait_status);
        }
    }
    free(pids);
    return last_status;
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

static int ash_execute_ast_async(
    struct ash_shell* shell,
    const struct ash_ast* command
) {
    pid_t pid = fork();
    if (pid < 0) {
        ash_exec_error(shell, "fork", errno);
        return 1;
    }
    if (pid == 0) {
        if (!shell->interactive) {
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
        shell->should_exit = false;
        _exit(ash_execute_ast(shell, command));
    }
    shell->last_async_pid = pid;
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
                    &node->value.for_loop.words[i],
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
        for (int i = 0;
             i < shell->positionals.count && !shell->should_exit;
             i++) {
            if (!ash_var_set(
                    shell,
                    node->value.for_loop.name,
                    shell->positionals.values[i],
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
            &node->value.case_command.subject,
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
            if (!ash_pattern_matches(
                    shell,
                    &clause->patterns[j],
                    subject,
                    &clause_matched
                )) {
                free(subject);
                return 2;
            }
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
    struct ash_saved_fds saved;
    ash_saved_fds_init(&saved);
    if (ash_apply_redirections(shell, &redirections, &saved) != 0) {
        ash_command_destroy(&redirections);
        ash_saved_fds_restore(shell, &saved);
        return 1;
    }
    ash_command_destroy(&redirections);
    int status = ash_execute_ast_case_body(shell, node);
    ash_saved_fds_restore(shell, &saved);
    return status;
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
    switch (node->kind) {
        case ASH_AST_SIMPLE:
            return ash_execute_ast_simple(shell, node);
        case ASH_AST_LIST:
            return ash_execute_ast_list(shell, node);
        case ASH_AST_AND_OR:
            return ash_execute_ast_and_or(shell, node);
        case ASH_AST_PIPELINE:
            return ash_execute_ast_pipeline(shell, node);
        case ASH_AST_SUBSHELL:
            return ash_execute_ast_group(shell, node, true);
        case ASH_AST_BRACE_GROUP:
            return ash_execute_ast_group(shell, node, false);
        case ASH_AST_IF:
            return ash_execute_ast_if(shell, node);
        case ASH_AST_WHILE:
        case ASH_AST_UNTIL:
            return ash_execute_ast_loop(shell, node);
        case ASH_AST_FOR:
            return ash_execute_ast_for(shell, node);
        case ASH_AST_CASE:
            return ash_execute_ast_case(shell, node);
        case ASH_AST_FUNCTION:
            return ash_execute_ast_function(shell, node);
    }
    return 2;
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

    struct ash_parser parser;
    ash_parser_init(
        &parser,
        ash_input_source_name(shell),
        input,
        strlen(input)
    );
    struct ash_ast* program = NULL;
    enum ash_parser_result result = ash_parser_parse_program(&parser, &program);
    if (result == ASH_PARSER_INCOMPLETE && !final_input) {
        *incomplete_out = true;
        ash_parser_destroy(&parser);
        return shell->last_status;
    }
    if (result != ASH_PARSER_COMPLETE) {
        ash_diag(shell, "%s", ash_parser_diagnostic(parser.error));
        *parser_error_out = true;
        ash_parser_destroy(&parser);
        shell->last_status = 2;
        return 2;
    }

    int status = ash_execute_ast(shell, program);
    shell->last_status = status;
    ash_ast_destroy(program);
    ash_parser_destroy(&parser);
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
        if (parser_error && !shell->interactive) {
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

    const char* argv0_param = progname;
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

    struct ash_shell shell = {
        .progname = progname,
        .positionals = {
            .argv0 = argv0_param,
            .values = positional_args,
            .count = positional_count,
            .previous = NULL,
        },
        .vars = NULL,
        .options = initial_options |
            (read_stdin ? ASH_SHELL_OPTION_STDIN : 0u),
        .aliases = NULL,
        .functions = NULL,
        .traps = NULL,
        .jobs = NULL,
        .command_cache = NULL,
        .input_stack = NULL,
        .cwd = {0},
        .shell_pid = getpid(),
        .last_async_pid = -1,
        .last_status = 0,
        .interactive = false,
        .login_shell = login_shell,
        .should_exit = false,
        .requested_exit_status = 0,
        .command_substitution = ash_command_substitute,
    };

    if (!ash_import_environment(&shell)) {
        ash_vars_destroy(&shell);
        ash_shell_context_release_owned(&shell);
        return 1;
    }

    bool interactive = force_interactive;
    if (!interactive && command_string == NULL && script_path == NULL) {
        interactive = isatty(STDIN_FILENO);
    }

    shell.interactive = interactive;
    if (interactive) {
        shell.options |= ASH_SHELL_OPTION_INTERACTIVE;
    }

    if (shell.interactive && !ash_var_exists(&shell, "PS1")) {
        if (!ash_var_set(&shell, "PS1", ash_default_prompt(), false)) {
            ash_vars_destroy(&shell);
            ash_shell_context_release_owned(&shell);
            return 1;
        }
    }

    int status = 0;
    if (command_string != NULL) {
        if (!ash_input_push_string(&shell, "-c", command_string)) {
            ash_diag_oom(&shell);
            ash_vars_destroy(&shell);
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
            ash_vars_destroy(&shell);
            ash_shell_context_release_owned(&shell);
            return 1;
        }

        if (!ash_input_push_file(&shell, script_path, script, true)) {
            ash_diag_oom(&shell);
            fclose(script);
            ash_vars_destroy(&shell);
            ash_shell_context_release_owned(&shell);
            return 1;
        }
        status = ash_execute_input(
            &shell,
            shell.interactive && ash_input_source_is_terminal(&shell)
        );
        ash_input_pop(&shell);
    }
    else {
        if (!ash_input_push_file(&shell, "<stdin>", stdin, false)) {
            ash_diag_oom(&shell);
            ash_vars_destroy(&shell);
            ash_shell_context_release_owned(&shell);
            return 1;
        }
        status = ash_execute_input(&shell, shell.interactive);
        ash_input_pop(&shell);
    }

    if (shell.should_exit) {
        status = shell.requested_exit_status;
    }

    ash_vars_destroy(&shell);
    ash_shell_context_release_owned(&shell);
    return status;
}
