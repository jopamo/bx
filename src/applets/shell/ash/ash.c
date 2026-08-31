#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
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
#include "applets/shell/ash/input.h"
#include "applets/shell/ash/lexer.h"
#include "applets/shell/ash/shell_context.h"
#include "bx/diag.h"
#include "lib/cli_common.h"
#include "lib/fd_ops.h"

extern char** environ;

enum ash_exec_token_kind {
    ASH_TOK_WORD = 0,
    ASH_TOK_PIPE,
    ASH_TOK_SEMI,
    ASH_TOK_REDIR_IN,
    ASH_TOK_REDIR_OUT,
    ASH_TOK_REDIR_APPEND,
    ASH_TOK_EOF,
};

enum ash_redir_kind {
    ASH_REDIR_IN = 0,
    ASH_REDIR_OUT,
    ASH_REDIR_APPEND,
};

struct ash_var {
    char* name;
    char* value;
    bool exported;
    struct ash_var* next;
};

struct ash_exec_token {
    enum ash_exec_token_kind kind;
    char* text;
    int redir_fd;
};

struct ash_exec_tokens {
    struct ash_exec_token* items;
    size_t len;
    size_t cap;
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

struct ash_string {
    char* data;
    size_t len;
    size_t cap;
};

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

static void ash_diag(const struct ash_shell* shell, const char* fmt, ...) {
    va_list ap;
    fprintf(stderr, "%s: ", shell->progname);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static bool ash_diag_oom(const struct ash_shell* shell) {
    ash_diag(shell, "out of memory");
    return false;
}

static void ash_exec_error(const struct ash_shell* shell, const char* path, int err) {
    fprintf(stderr, "%s: %s: %s\n", shell->progname, path, bx_strerror(err));
}

static void ash_exec_not_found(const struct ash_shell* shell, const char* path) {
    fprintf(stderr, "%s: %s: not found\n", shell->progname, path);
}

static void ash_string_init(struct ash_string* string) {
    string->data = NULL;
    string->len = 0;
    string->cap = 0;
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

static bool ash_string_reserve(const struct ash_shell* shell, struct ash_string* string, size_t needed) {
    if (string->cap >= needed) {
        return true;
    }

    size_t new_cap = (string->cap == 0) ? 32u : string->cap;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2u) {
            new_cap = needed;
            break;
        }
        new_cap *= 2u;
    }

    char* grown = ash_realloc_array(shell, string->data, new_cap, sizeof(*string->data));
    if (grown == NULL) {
        return false;
    }

    string->data = grown;
    string->cap = new_cap;
    return true;
}

static bool ash_string_append_char(const struct ash_shell* shell, struct ash_string* string, char ch) {
    if (string->len > SIZE_MAX - 2u || !ash_string_reserve(shell, string, string->len + 2u)) {
        return false;
    }
    string->data[string->len++] = ch;
    string->data[string->len] = '\0';
    return true;
}

static bool ash_string_append_text(const struct ash_shell* shell, struct ash_string* string, const char* text) {
    size_t len = strlen(text);
    if (string->len > SIZE_MAX - len - 1u || !ash_string_reserve(shell, string, string->len + len + 1u)) {
        return false;
    }
    memcpy(string->data + string->len, text, len + 1u);
    string->len += len;
    return true;
}

static bool ash_string_append_span(const struct ash_shell* shell, struct ash_string* string, const char* text, size_t len) {
    if (len > (size_t)PTRDIFF_MAX || string->len > SIZE_MAX - len - 1u) {
        return ash_diag_oom(shell);
    }

    if (!ash_string_reserve(shell, string, string->len + len + 1u)) {
        return false;
    }
    memcpy(string->data + string->len, text, len);
    string->len += len;
    string->data[string->len] = '\0';
    return true;
}

static char* ash_string_take(const struct ash_shell* shell, struct ash_string* string) {
    if (string->data == NULL) {
        return ash_strdup_text(shell, "");
    }

    char* out = string->data;
    string->data = NULL;
    string->len = 0;
    string->cap = 0;
    return out;
}

static void ash_string_destroy(struct ash_string* string) {
    free(string->data);
    string->data = NULL;
    string->len = 0;
    string->cap = 0;
}

static void ash_tokens_init(struct ash_exec_tokens* tokens) {
    tokens->items = NULL;
    tokens->len = 0;
    tokens->cap = 0;
}

static bool ash_tokens_push(const struct ash_shell* shell, struct ash_exec_tokens* tokens, struct ash_exec_token token) {
    if (tokens->len == tokens->cap) {
        size_t new_cap = (tokens->cap == 0) ? 16u : tokens->cap * 2u;
        if (tokens->cap != 0u && tokens->cap > SIZE_MAX / 2u) {
            return ash_diag_oom(shell);
        }
        struct ash_exec_token* grown = ash_realloc_array(shell, tokens->items, new_cap, sizeof(*tokens->items));
        if (grown == NULL) {
            return false;
        }
        tokens->items = grown;
        tokens->cap = new_cap;
    }

    tokens->items[tokens->len++] = token;
    return true;
}

static void ash_tokens_destroy(struct ash_exec_tokens* tokens) {
    for (size_t i = 0; i < tokens->len; i++) {
        free(tokens->items[i].text);
    }
    free(tokens->items);
    tokens->items = NULL;
    tokens->len = 0;
    tokens->cap = 0;
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

static bool ash_is_name_start(unsigned char ch) {
    return isalpha(ch) || ch == '_';
}

static bool ash_is_name_char(unsigned char ch) {
    return isalnum(ch) || ch == '_';
}

static bool ash_is_valid_name_span(const char* text, size_t len) {
    if (len == 0u) {
        return false;
    }

    if (!ash_is_name_start((unsigned char)text[0])) {
        return false;
    }

    for (size_t i = 1; i < len; i++) {
        if (!ash_is_name_char((unsigned char)text[i])) {
            return false;
        }
    }

    return true;
}

static bool ash_parse_assignment(const char* text, size_t* name_len_out, const char** value_out) {
    const char* eq = strchr(text, '=');
    if (eq == NULL || eq == text) {
        return false;
    }

    size_t name_len = (size_t)(eq - text);
    if (!ash_is_valid_name_span(text, name_len)) {
        return false;
    }

    *name_len_out = name_len;
    *value_out = eq + 1;
    return true;
}

static bool ash_is_assignment_word(const char* text) {
    size_t name_len = 0;
    const char* value = NULL;
    (void)value;
    return ash_parse_assignment(text, &name_len, &value);
}

static struct ash_var* ash_var_find_len(struct ash_shell* shell, const char* name, size_t len) {
    for (struct ash_var* var = shell->vars; var != NULL; var = var->next) {
        if (strlen(var->name) == len && memcmp(var->name, name, len) == 0) {
            return var;
        }
    }
    return NULL;
}

static const struct ash_var* ash_var_find_len_const(const struct ash_shell* shell, const char* name, size_t len) {
    for (const struct ash_var* var = shell->vars; var != NULL; var = var->next) {
        if (strlen(var->name) == len && memcmp(var->name, name, len) == 0) {
            return var;
        }
    }
    return NULL;
}

static const char* ash_var_get_len(const struct ash_shell* shell, const char* name, size_t len) {
    const struct ash_var* var = ash_var_find_len_const(shell, name, len);
    return (var != NULL) ? var->value : NULL;
}

static const char* ash_var_get(const struct ash_shell* shell, const char* name) {
    return ash_var_get_len(shell, name, strlen(name));
}

static bool ash_var_exists(const struct ash_shell* shell, const char* name) {
    return ash_var_get(shell, name) != NULL;
}

static bool ash_var_publish_exported(struct ash_shell* shell, struct ash_var* var) {
    if (!var->exported) {
        return true;
    }

    if (setenv(var->name, var->value, 1) != 0) {
        if (errno == ENOMEM) {
            return ash_diag_oom(shell);
        }
        ash_exec_error(shell, var->name, errno);
        return false;
    }

    return true;
}

static bool ash_var_set_with_export(struct ash_shell* shell, const char* name, size_t name_len, const char* value, bool mark_export) {
    struct ash_var* var = ash_var_find_len(shell, name, name_len);
    if (var == NULL) {
        var = ash_malloc_bytes(shell, sizeof(*var));
        if (var == NULL) {
            return false;
        }
        var->name = ash_slice_dup(shell, name, name_len);
        var->value = ash_strdup_text(shell, value);
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
        char* new_value = ash_strdup_text(shell, value);
        if (new_value == NULL) {
            return false;
        }
        free(var->value);
        var->value = new_value;
        if (mark_export) {
            var->exported = true;
        }
    }

    return ash_var_publish_exported(shell, var);
}

static bool ash_var_set(struct ash_shell* shell, const char* name, const char* value, bool mark_export) {
    return ash_var_set_with_export(shell, name, strlen(name), value, mark_export);
}

static bool ash_var_export(struct ash_shell* shell, const char* name) {
    size_t name_len = strlen(name);
    struct ash_var* var = ash_var_find_len(shell, name, name_len);
    if (var == NULL) {
        return ash_var_set_with_export(shell, name, name_len, "", true);
    }

    var->exported = true;
    return ash_var_publish_exported(shell, var);
}

static void ash_var_unset(struct ash_shell* shell, const char* name) {
    struct ash_var* prev = NULL;
    struct ash_var* current = shell->vars;

    while (current != NULL) {
        if (strcmp(current->name, name) == 0) {
            if (prev != NULL) {
                prev->next = current->next;
            }
            else {
                shell->vars = current->next;
            }

            unsetenv(current->name);
            free(current->name);
            free(current->value);
            free(current);
            return;
        }

        prev = current;
        current = current->next;
    }
}

static void ash_vars_destroy(struct ash_shell* shell) {
    struct ash_var* var = shell->vars;
    while (var != NULL) {
        struct ash_var* next = var->next;
        free(var->name);
        free(var->value);
        free(var);
        var = next;
    }
    shell->vars = NULL;
}

static bool ash_import_environment(struct ash_shell* shell) {
    for (char** envp = environ; envp != NULL && *envp != NULL; envp++) {
        const char* entry = *envp;
        const char* eq = strchr(entry, '=');
        if (eq == NULL || eq == entry) {
            continue;
        }

        size_t name_len = (size_t)(eq - entry);
        if (!ash_is_valid_name_span(entry, name_len)) {
            continue;
        }
        if (name_len == 3u && memcmp(entry, "IFS", 3u) == 0) {
            continue;
        }

        if (!ash_var_set_with_export(shell, entry, name_len, eq + 1, true)) {
            return false;
        }
    }

    if (unsetenv("IFS") != 0 || !ash_var_set(shell, "IFS", " \t\n", false)) {
        return false;
    }

    char* cwd = getcwd(NULL, 0);
    if (cwd != NULL) {
        const char* inherited_pwd = ash_var_get(shell, "PWD");
        struct stat cwd_stat;
        struct stat pwd_stat;
        bool inherited_pwd_valid = inherited_pwd != NULL &&
            inherited_pwd[0] == '/' &&
            stat(".", &cwd_stat) == 0 &&
            stat(inherited_pwd, &pwd_stat) == 0 &&
            cwd_stat.st_dev == pwd_stat.st_dev &&
            cwd_stat.st_ino == pwd_stat.st_ino;
        const char* logical = inherited_pwd_valid ? inherited_pwd : cwd;

        shell->cwd.physical = ash_strdup_text(shell, cwd);
        shell->cwd.logical = ash_strdup_text(shell, logical);
        const char* inherited_oldpwd = ash_var_get(shell, "OLDPWD");
        if (inherited_oldpwd != NULL) {
            shell->cwd.old_logical = ash_strdup_text(shell, inherited_oldpwd);
        }
        if (shell->cwd.physical == NULL || shell->cwd.logical == NULL ||
            (inherited_oldpwd != NULL && shell->cwd.old_logical == NULL)) {
            free(cwd);
            return false;
        }
        if (!inherited_pwd_valid && !ash_var_set(shell, "PWD", cwd, true)) {
            free(cwd);
            return false;
        }
        free(cwd);
    }

    if (!ash_var_exists(shell, "PATH")) {
        if (!ash_var_set(shell, "PATH", "/bin:/usr/bin", true)) {
            return false;
        }
    }

    return true;
}

static const char* ash_parameter_positional(const struct ash_shell* shell, long index) {
    if (index == 0) {
        return shell->positionals.argv0;
    }

    if (index < 0 || index > shell->positionals.count) {
        return "";
    }

    return shell->positionals.values[index - 1];
}

static const char* ash_parameter_named(const struct ash_shell* shell, const char* name, size_t len) {
    const char* value = ash_var_get_len(shell, name, len);
    return (value != NULL) ? value : "";
}

static bool ash_append_special_parameter(struct ash_shell* shell, char parameter, struct ash_string* out) {
    char numbuf[32];

    switch (parameter) {
        case '?':
            snprintf(numbuf, sizeof(numbuf), "%d", shell->last_status);
            return ash_string_append_text(shell, out, numbuf);
        case '$':
            snprintf(numbuf, sizeof(numbuf), "%ld", (long)shell->shell_pid);
            return ash_string_append_text(shell, out, numbuf);
        case '#':
            snprintf(numbuf, sizeof(numbuf), "%d", shell->positionals.count);
            return ash_string_append_text(shell, out, numbuf);
        case '-': {
            char letters[16];
            ash_shell_option_letters(shell, letters, sizeof(letters));
            return ash_string_append_text(shell, out, letters);
        }
        case '!':
            if (shell->last_async_pid <= 0) {
                return true;
            }
            snprintf(numbuf, sizeof(numbuf), "%ld", (long)shell->last_async_pid);
            return ash_string_append_text(shell, out, numbuf);
        case '0':
            return ash_string_append_text(shell, out, ash_parameter_positional(shell, 0));
        default:
            return false;
    }
}

static void ash_expand_parameter(struct ash_shell* shell, const char* input, size_t* pos, struct ash_string* out, bool* error_out) {
    size_t i = *pos;
    i++;

    char ch = input[i];
    if (ch == '\0') {
        if (!ash_string_append_char(shell, out, '$')) {
            *error_out = true;
        }
        *pos = i;
        return;
    }

    if (strchr("?$#-!", ch) != NULL) {
        if (!ash_append_special_parameter(shell, ch, out)) {
            *error_out = true;
            return;
        }
        *pos = i + 1u;
        return;
    }

    if (ch == '{') {
        i++;

        if (strchr("?$#-!0", input[i]) != NULL) {
            char special = input[i];
            i++;
            if (input[i] != '}') {
                ash_diag(shell, "bad substitution");
                *error_out = true;
                return;
            }
            if (!ash_append_special_parameter(shell, special, out)) {
                *error_out = true;
                return;
            }
            i++;
            *pos = i;
            return;
        }

        size_t start = i;
        while (ash_is_name_char((unsigned char)input[i])) {
            i++;
        }

        if (i == start || input[i] != '}') {
            ash_diag(shell, "bad substitution");
            *error_out = true;
            return;
        }

        if (!ash_string_append_text(shell, out, ash_parameter_named(shell, input + start, i - start))) {
            *error_out = true;
            return;
        }
        i++;
        *pos = i;
        return;
    }

    if (isdigit((unsigned char)ch)) {
        long value = 0;
        while (isdigit((unsigned char)input[i])) {
            value = value * 10 + (long)(input[i] - '0');
            i++;
        }

        if (!ash_string_append_text(shell, out, ash_parameter_positional(shell, value))) {
            *error_out = true;
            return;
        }
        *pos = i;
        return;
    }

    if (!ash_is_name_start((unsigned char)ch)) {
        if (!ash_string_append_char(shell, out, '$')) {
            *error_out = true;
        }
        *pos = i;
        return;
    }

    size_t start = i;
    while (ash_is_name_char((unsigned char)input[i])) {
        i++;
    }

    if (!ash_string_append_text(shell, out, ash_parameter_named(shell, input + start, i - start))) {
        *error_out = true;
        return;
    }
    *pos = i;
}

static int ash_hex_value(unsigned char ch) {
    if (ch >= '0' && ch <= '9') {
        return (int)(ch - '0');
    }
    if (ch >= 'a' && ch <= 'f') {
        return (int)(ch - 'a') + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return (int)(ch - 'A') + 10;
    }
    return -1;
}

static bool ash_append_dollar_single(
    struct ash_shell* shell,
    struct ash_string* output,
    const char* text,
    size_t length
) {
    for (size_t i = 0u; i < length; i++) {
        unsigned char ch = (unsigned char)text[i];
        if (ch != '\\' || i + 1u == length) {
            if (!ash_string_append_char(shell, output, (char)ch)) {
                return false;
            }
            continue;
        }

        unsigned char escaped = (unsigned char)text[++i];
        char decoded = '\0';
        switch (escaped) {
            case 'a': decoded = '\a'; break;
            case 'b': decoded = '\b'; break;
            case 'e':
            case 'E': decoded = 0x1b; break;
            case 'f': decoded = '\f'; break;
            case 'n': decoded = '\n'; break;
            case 'r': decoded = '\r'; break;
            case 't': decoded = '\t'; break;
            case 'v': decoded = '\v'; break;
            case '\\': decoded = '\\'; break;
            case '\'': decoded = '\''; break;
            case '"': decoded = '"'; break;
            case '\n': continue;
            case 'c':
                if (i + 1u < length) {
                    decoded = (char)((unsigned char)text[++i] & 0x1fu);
                }
                else {
                    decoded = 'c';
                }
                break;
            case 'x': {
                int value = 0;
                size_t digits = 0u;
                while (digits < 2u && i + 1u < length) {
                    int digit = ash_hex_value((unsigned char)text[i + 1u]);
                    if (digit < 0) {
                        break;
                    }
                    value = value * 16 + digit;
                    i++;
                    digits++;
                }
                if (digits == 0u) {
                    if (!ash_string_append_char(shell, output, '\\')) {
                        return false;
                    }
                    decoded = 'x';
                }
                else {
                    decoded = (char)(unsigned char)value;
                }
                break;
            }
            default:
                if (escaped >= '0' && escaped <= '7') {
                    unsigned int value = (unsigned int)(escaped - '0');
                    size_t digits = 1u;
                    while (digits < 3u && i + 1u < length &&
                           text[i + 1u] >= '0' && text[i + 1u] <= '7') {
                        value = value * 8u + (unsigned int)(text[++i] - '0');
                        digits++;
                    }
                    decoded = (char)(unsigned char)value;
                }
                else {
                    if (!ash_string_append_char(shell, output, '\\')) {
                        return false;
                    }
                    decoded = (char)escaped;
                }
                break;
        }

        if (!ash_string_append_char(shell, output, decoded)) {
            return false;
        }
    }
    return true;
}

static bool ash_expand_lexed_word(
    struct ash_shell* shell,
    const struct ash_word* word,
    char** output_word
) {
    struct ash_string output;
    ash_string_init(&output);

    for (size_t i = 0u; i < word->count; i++) {
        const struct ash_word_part* part = &word->parts[i];
        if (part->kind == ASH_WORD_PARAMETER) {
            size_t position = 0u;
            bool expansion_error = false;
            ash_expand_parameter(
                shell,
                part->text,
                &position,
                &output,
                &expansion_error
            );
            if (expansion_error) {
                ash_string_destroy(&output);
                return false;
            }
            continue;
        }

        if (part->kind == ASH_WORD_TEXT &&
            part->quote == ASH_QUOTE_DOLLAR_SINGLE) {
            if (!ash_append_dollar_single(
                    shell,
                    &output,
                    part->text,
                    part->length
                )) {
                ash_string_destroy(&output);
                return false;
            }
            continue;
        }

        if (!ash_string_append_span(shell, &output, part->text, part->length)) {
            ash_string_destroy(&output);
            return false;
        }
    }

    *output_word = ash_string_take(shell, &output);
    return *output_word != NULL;
}

static bool ash_parse_lexed_io_number(
    struct ash_shell* shell,
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

static int ash_tokenize(struct ash_shell* shell, const char* input, struct ash_exec_tokens* tokens) {
    struct ash_lexer lexer;
    ash_lexer_init(&lexer, ash_input_source_name(shell), input, strlen(input));
    int pending_io_number = -1;

    while (true) {
        struct ash_token lexical_token;
        enum ash_lexer_result result = ash_lexer_next(&lexer, &lexical_token);
        if (result == ASH_LEXER_INCOMPLETE || result == ASH_LEXER_ERROR) {
            const char* diagnostic =
                (lexer.error != NULL) ? lexer.error : "lexical error";
            if (strcmp(diagnostic, "unterminated parameter expansion") == 0) {
                diagnostic = "bad substitution";
            }
            ash_diag(shell, "%s", diagnostic);
            ash_token_destroy(&lexical_token);
            return 1;
        }

        if (result == ASH_LEXER_END) {
            struct ash_exec_token eof_token = {
                .kind = ASH_TOK_EOF,
                .text = NULL,
                .redir_fd = -1,
            };
            ash_token_destroy(&lexical_token);
            return ash_tokens_push(shell, tokens, eof_token) ? 0 : 1;
        }

        if (lexical_token.kind == ASH_TOKEN_IO_NUMBER) {
            bool valid = ash_parse_lexed_io_number(
                shell,
                lexical_token.io_number,
                &pending_io_number
            );
            ash_token_destroy(&lexical_token);
            if (!valid) {
                return 1;
            }
            continue;
        }

        struct ash_exec_token token = {
            .text = NULL,
            .redir_fd = -1,
        };
        switch (lexical_token.kind) {
            case ASH_TOKEN_WORD:
                if (pending_io_number >= 0 ||
                    !ash_expand_lexed_word(shell, &lexical_token.word, &token.text)) {
                    ash_token_destroy(&lexical_token);
                    if (pending_io_number >= 0) {
                        ash_diag(shell, "redirection operator expected after IO number");
                    }
                    return 1;
                }
                token.kind = ASH_TOK_WORD;
                break;
            case ASH_TOKEN_NEWLINE:
            case ASH_TOKEN_SEMI:
                token.kind = ASH_TOK_SEMI;
                break;
            case ASH_TOKEN_PIPE:
                token.kind = ASH_TOK_PIPE;
                break;
            case ASH_TOKEN_LESS:
                token.kind = ASH_TOK_REDIR_IN;
                token.redir_fd = (pending_io_number >= 0) ? pending_io_number : 0;
                pending_io_number = -1;
                break;
            case ASH_TOKEN_GREAT:
                token.kind = ASH_TOK_REDIR_OUT;
                token.redir_fd = (pending_io_number >= 0) ? pending_io_number : 1;
                pending_io_number = -1;
                break;
            case ASH_TOKEN_DGREAT:
                token.kind = ASH_TOK_REDIR_APPEND;
                token.redir_fd = (pending_io_number >= 0) ? pending_io_number : 1;
                pending_io_number = -1;
                break;
            default:
                ash_diag(
                    shell,
                    "unexpected token: %s",
                    ash_token_kind_name(lexical_token.kind)
                );
                ash_token_destroy(&lexical_token);
                return 1;
        }

        ash_token_destroy(&lexical_token);
        if (!ash_tokens_push(shell, tokens, token)) {
            free(token.text);
            return 1;
        }
    }
}

static int ash_parse_command(const struct ash_exec_tokens* tokens, size_t* index, struct ash_command* command, struct ash_shell* shell) {
    bool saw_command_word = false;

    while (*index < tokens->len) {
        const struct ash_exec_token* token = &tokens->items[*index];

        if (token->kind == ASH_TOK_WORD) {
            if (!saw_command_word && ash_is_assignment_word(token->text)) {
                if (!ash_command_push_assignment(shell, command, token->text)) {
                    return 1;
                }
            }
            else {
                saw_command_word = true;
                if (!ash_command_push_word(shell, command, token->text)) {
                    return 1;
                }
            }
            (*index)++;
            continue;
        }

        if (token->kind == ASH_TOK_REDIR_IN || token->kind == ASH_TOK_REDIR_OUT || token->kind == ASH_TOK_REDIR_APPEND) {
            enum ash_redir_kind redir_kind = ASH_REDIR_IN;
            if (token->kind == ASH_TOK_REDIR_OUT) {
                redir_kind = ASH_REDIR_OUT;
            }
            else if (token->kind == ASH_TOK_REDIR_APPEND) {
                redir_kind = ASH_REDIR_APPEND;
            }

            int redir_fd = token->redir_fd;
            (*index)++;
            if (*index >= tokens->len || tokens->items[*index].kind != ASH_TOK_WORD) {
                ash_diag(shell, "redirection requires a target");
                return 1;
            }

            if (!ash_command_push_redir(shell, command, redir_fd, redir_kind, tokens->items[*index].text)) {
                return 1;
            }
            (*index)++;
            continue;
        }

        break;
    }

    return 0;
}

static int ash_parse_pipeline(const struct ash_exec_tokens* tokens, size_t* index, struct ash_pipeline* pipeline, struct ash_shell* shell) {
    while (true) {
        struct ash_command command;
        ash_command_init(&command);

        if (ash_parse_command(tokens, index, &command, shell) != 0) {
            ash_command_destroy(&command);
            return 1;
        }

        if (ash_command_is_empty(&command)) {
            ash_command_destroy(&command);
            ash_diag(shell, "syntax error near unexpected token");
            return 1;
        }

        if (!ash_pipeline_push_command(shell, pipeline, &command)) {
            ash_command_destroy(&command);
            return 1;
        }

        if (*index >= tokens->len || tokens->items[*index].kind != ASH_TOK_PIPE) {
            break;
        }

        (*index)++;
        if (*index >= tokens->len || tokens->items[*index].kind == ASH_TOK_PIPE || tokens->items[*index].kind == ASH_TOK_SEMI || tokens->items[*index].kind == ASH_TOK_EOF) {
            ash_diag(shell, "syntax error near unexpected token '|'");
            return 1;
        }
    }

    return 0;
}

static int ash_apply_redirections(const struct ash_shell* shell, const struct ash_command* command, struct ash_saved_fds* saved) {
    for (size_t i = 0; i < command->redir_count; i++) {
        const struct ash_redir* redir = &command->redirs[i];

        if (saved != NULL && !ash_saved_fds_has_target(saved, redir->fd)) {
            int dup_fd = bx_fd_dup_cloexec(redir->fd);
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

        int open_flags = 0;
        mode_t mode = 0666;
        switch (redir->kind) {
            case ASH_REDIR_IN:
                open_flags = O_RDONLY;
                break;
            case ASH_REDIR_OUT:
                open_flags = O_WRONLY | O_CREAT | O_TRUNC;
                break;
            case ASH_REDIR_APPEND:
                open_flags = O_WRONLY | O_CREAT | O_APPEND;
                break;
        }

        int fd = bx_fd_open_cloexec(redir->target, open_flags, mode);
        if (fd < 0) {
            ash_exec_error(shell, redir->target, errno);
            return 1;
        }

        if (bx_fd_dup2_exact(fd, redir->fd) < 0) {
            int err = errno;
            close(fd);
            ash_exec_error(shell, "dup2", err);
            return 1;
        }

        close(fd);
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

        struct ash_string candidate;
        ash_string_init(&candidate);

        if (dir_len == 0u) {
            if (!ash_string_append_text(shell, &candidate, "./")) {
                ash_string_destroy(&candidate);
                return 126;
            }
        }
        else {
            if (!ash_string_append_span(shell, &candidate, segment, dir_len) ||
                !ash_string_append_char(shell, &candidate, '/')) {
                ash_string_destroy(&candidate);
                return 126;
            }
        }
        if (!ash_string_append_text(shell, &candidate, command)) {
            ash_string_destroy(&candidate);
            return 126;
        }

        char* candidate_path = ash_string_take(shell, &candidate);
        if (candidate_path == NULL) {
            ash_string_destroy(&candidate);
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
        case ASH_BUILTIN_INVALID:
            break;
    }

    return 1;
}

static int ash_execute_in_child(struct ash_shell* shell, const struct ash_command* command) {
    if (ash_apply_redirections(shell, command, NULL) != 0) {
        return 1;
    }

    if (command->word_count == 0u) {
        if (ash_apply_command_assignments_shell(shell, command) != 0) {
            return 1;
        }
        return 0;
    }

    struct ash_command_resolution resolution = ash_command_resolve_builtin(command->words[0]);
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

        struct ash_command_resolution resolution = ash_command_resolve_builtin(command->words[0]);
        if (ash_command_resolution_is_builtin(&resolution)) {
            return ash_execute_single_command_parent(shell, command, resolution.target.builtin);
        }

        return ash_execute_single_command_forked(shell, command);
    }

    return ash_execute_pipeline_forked(shell, pipeline);
}

static int ash_parse_and_execute(struct ash_shell* shell, const struct ash_exec_tokens* tokens, bool* parser_error_out) {
    size_t index = 0;
    int status = shell->last_status;
    if (parser_error_out != NULL) {
        *parser_error_out = false;
    }

    while (index < tokens->len) {
        while (index < tokens->len && tokens->items[index].kind == ASH_TOK_SEMI) {
            index++;
        }

        if (index >= tokens->len || tokens->items[index].kind == ASH_TOK_EOF) {
            break;
        }

        struct ash_pipeline pipeline;
        ash_pipeline_init(&pipeline);

        if (ash_parse_pipeline(tokens, &index, &pipeline, shell) != 0) {
            ash_pipeline_destroy(&pipeline);
            if (parser_error_out != NULL) {
                *parser_error_out = true;
            }
            return 2;
        }

        status = ash_execute_pipeline(shell, &pipeline);
        ash_pipeline_destroy(&pipeline);

        shell->last_status = status;

        if (shell->should_exit) {
            break;
        }

        if (index < tokens->len && tokens->items[index].kind == ASH_TOK_SEMI) {
            index++;
        }
    }

    return status;
}

static int ash_execute_segment(struct ash_shell* shell, const char* input, bool* parser_error_out) {
    struct ash_exec_tokens tokens;
    ash_tokens_init(&tokens);
    if (parser_error_out != NULL) {
        *parser_error_out = false;
    }

    int rc = ash_tokenize(shell, input, &tokens);
    if (rc != 0) {
        ash_tokens_destroy(&tokens);
        shell->last_status = 2;
        if (parser_error_out != NULL) {
            *parser_error_out = true;
        }
        return 2;
    }

    rc = ash_parse_and_execute(shell, &tokens, parser_error_out);
    ash_tokens_destroy(&tokens);
    shell->last_status = rc;
    return rc;
}

static int ash_execute_buffer(struct ash_shell* shell, const char* input, bool* parser_error_out) {
    size_t start = 0;
    size_t i = 0;
    bool in_single = false;
    bool in_double = false;
    bool escape = false;
    int status = shell->last_status;
    if (parser_error_out != NULL) {
        *parser_error_out = false;
    }

    while (true) {
        char ch = input[i];
        bool at_end = (ch == '\0');
        bool separator = false;

        if (at_end || (!in_single && !in_double && !escape && (ch == ';' || ch == '\n'))) {
            separator = true;
        }

        if (separator) {
            size_t len = i - start;
            if (len > 0u) {
                char* segment = ash_slice_dup(shell, input + start, len);
                if (segment == NULL) {
                    return 2;
                }
                bool segment_parser_error = false;
                status = ash_execute_segment(shell, segment, &segment_parser_error);
                free(segment);
                if (segment_parser_error) {
                    if (parser_error_out != NULL) {
                        *parser_error_out = true;
                    }
                    return status;
                }
                if (shell->should_exit) {
                    return status;
                }
            }

            if (at_end) {
                break;
            }

            start = i + 1u;
            i++;
            continue;
        }

        if (escape) {
            escape = false;
            i++;
            continue;
        }

        if (!in_single && ch == '\\') {
            escape = true;
            i++;
            continue;
        }

        if (!in_double && ch == '\'') {
            in_single = !in_single;
            i++;
            continue;
        }

        if (!in_single && ch == '\"') {
            in_double = !in_double;
            i++;
            continue;
        }

        if (at_end) {
            break;
        }

        i++;
    }

    return status;
}

static const char* ash_default_prompt(void) {
    return (geteuid() == 0) ? "# " : "$ ";
}

static void ash_print_prompt(struct ash_shell* shell) {
    const char* prompt = ash_var_get(shell, "PS1");
    if (prompt == NULL) {
        prompt = ash_default_prompt();
    }

    fputs(prompt, stdout);
    fflush(stdout);
}

static int ash_execute_input(struct ash_shell* shell, bool prompt) {
    char* line = NULL;
    size_t cap = 0;
    int status = shell->last_status;

    while (!shell->should_exit) {
        if (prompt) {
            ash_print_prompt(shell);
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
            break;
        }

        bool parser_error = false;
        status = ash_execute_buffer(shell, line, &parser_error);
        if (parser_error && !shell->interactive) {
            break;
        }
    }

    free(line);
    return status;
}

static void ash_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [-i] [-s] [-c command] [script [arg ...]]\n", progname);
    fprintf(stream, "\n");
    fprintf(stream, "Minimal rescue shell applet.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -c command   run command string\n");
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
        .options = read_stdin ? ASH_SHELL_OPTION_STDIN : 0u,
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
