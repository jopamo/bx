#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "applets/shell/ash/control.h"
#include "applets/shell/ash/diagnostic.h"
#include "applets/shell/ash/expansion.h"
#include "applets/shell/ash/input.h"
#include "applets/shell/ash/input_execution.h"
#include "applets/shell/ash/lexer.h"
#include "applets/shell/ash/shell_context.h"
#include "applets/shell/ash/shell_policy.h"
#include "applets/shell/ash/startup.h"
#include "applets/shell/ash/variables.h"
#include "lib/path_ops.h"

static const char ash_system_profile[] = "/etc/profile";
static const char* const ash_home_profiles[] = {
    "~/.bash_profile",
    "~/.bash_login",
    "~/.profile",
};

enum ash_startup_file_result {
    ASH_STARTUP_FILE_MISSING = 0,
    ASH_STARTUP_FILE_COMPLETE,
    ASH_STARTUP_FILE_EXIT,
    ASH_STARTUP_FILE_FATAL,
};

static bool ash_startup_should_read_bashrc(
    const struct ash_shell* shell,
    const struct ash_startup_request* request
) {
    return ash_shell_policy_is_bash(&shell->policy) &&
        ash_shell_policy_allows_startup(&shell->policy) &&
        ash_shell_policy_has(
            &shell->policy,
            ASH_SHELL_POLICY_INTERACTIVE
        ) &&
        !ash_shell_policy_has(
            &shell->policy,
            ASH_SHELL_POLICY_LOGIN
        ) &&
        request->bashrc != ASH_BASHRC_SUPPRESSED;
}

static bool ash_startup_should_read_profiles(
    const struct ash_shell* shell,
    const struct ash_startup_request* request
) {
    return ash_shell_policy_is_bash(&shell->policy) &&
        ash_shell_policy_allows_startup(&shell->policy) &&
        ash_shell_policy_has(
            &shell->policy,
            ASH_SHELL_POLICY_LOGIN
        ) &&
        request->profiles != ASH_PROFILES_SUPPRESSED;
}

static char* ash_startup_expand_path(
    struct ash_shell* shell,
    const char* requested
) {
    const struct bx_path_tilde_context context = {
        .home = ash_var_get(shell, "HOME"),
        .current_directory = ash_var_get(shell, "PWD"),
        .previous_directory = ash_var_get(shell, "OLDPWD"),
    };
    char* path = bx_path_expand_tilde_dup(requested, &context);
    if (path == NULL) {
        if (errno == ENOMEM) {
            ash_diag_oom(shell);
        }
        else {
            ash_exec_error(
                shell,
                "startup file path",
                errno != 0 ? errno : EINVAL
            );
        }
    }
    return path;
}

static enum ash_startup_file_result ash_startup_execute_file(
    struct ash_shell* shell,
    const char* path
) {
    FILE* stream = fopen(path, "r");
    if (stream == NULL) {
        int error = errno;
        if (error == ENOMEM) {
            ash_diag_oom(shell);
            return ASH_STARTUP_FILE_FATAL;
        }
        if (error != ENOENT && error != ENOTDIR) {
            ash_exec_error(shell, path, error);
            return ASH_STARTUP_FILE_COMPLETE;
        }
        return ASH_STARTUP_FILE_MISSING;
    }

    struct stat metadata;
    if (fstat(fileno(stream), &metadata) != 0) {
        int error = errno;
        ash_exec_error(shell, path, error);
        fclose(stream);
        return ASH_STARTUP_FILE_COMPLETE;
    }
    if (S_ISDIR(metadata.st_mode)) {
        ash_exec_error(shell, path, EISDIR);
        fclose(stream);
        return ASH_STARTUP_FILE_COMPLETE;
    }

    errno = 0;
    int status = ash_input_execute_stream(
        shell,
        ASH_INPUT_SOURCED_FILE,
        path,
        stream,
        ASH_INPUT_TAKE_STREAM,
        false
    );
    int execution_error = errno;
    if (!shell->should_exit &&
        status == 2 &&
        (execution_error == ENOMEM ||
         execution_error == EOVERFLOW)) {
        return ASH_STARTUP_FILE_FATAL;
    }
    return shell->should_exit ?
        ASH_STARTUP_FILE_EXIT :
        ASH_STARTUP_FILE_COMPLETE;
}

static enum ash_startup_outcome ash_startup_file_outcome(
    enum ash_startup_file_result result
) {
    switch (result) {
        case ASH_STARTUP_FILE_MISSING:
        case ASH_STARTUP_FILE_COMPLETE:
            return ASH_STARTUP_CONTINUE;
        case ASH_STARTUP_FILE_EXIT:
            return ASH_STARTUP_EXIT;
        case ASH_STARTUP_FILE_FATAL:
            return ASH_STARTUP_FATAL;
    }
    return ASH_STARTUP_FATAL;
}

static enum ash_startup_outcome ash_startup_execute_profiles(
    struct ash_shell* shell
) {
    enum ash_startup_file_result result = ash_startup_execute_file(
        shell,
        ash_system_profile
    );
    if (result == ASH_STARTUP_FILE_EXIT ||
        result == ASH_STARTUP_FILE_FATAL) {
        return ash_startup_file_outcome(result);
    }

    for (size_t i = 0u;
         i < sizeof(ash_home_profiles) / sizeof(ash_home_profiles[0]);
         i++) {
        char* path = ash_startup_expand_path(
            shell,
            ash_home_profiles[i]
        );
        if (path == NULL) {
            return ASH_STARTUP_FATAL;
        }
        result = ash_startup_execute_file(
            shell,
            path
        );
        free(path);
        if (result != ASH_STARTUP_FILE_MISSING) {
            return ash_startup_file_outcome(result);
        }
    }
    return ASH_STARTUP_CONTINUE;
}

static enum ash_startup_outcome ash_startup_execute_bashrc(
    struct ash_shell* shell,
    const struct ash_startup_request* request
) {
    const char* requested = request->bashrc == ASH_BASHRC_EXPLICIT ?
        request->bashrc_path :
        "~/.bashrc";
    char* path = ash_startup_expand_path(shell, requested);
    if (path == NULL) {
        return ASH_STARTUP_FATAL;
    }
    enum ash_startup_file_result result = ash_startup_execute_file(
        shell,
        path
    );
    free(path);
    return ash_startup_file_outcome(result);
}

static enum ash_startup_outcome ash_startup_execute_env_file(struct ash_shell* shell, const char* name) {
    const char* requested = ash_var_get(shell, name);
    if (requested == NULL || requested[0] == '\0') {
        return ASH_STARTUP_CONTINUE;
    }

    struct ash_lexer lexer;
    ash_lexer_init(&lexer, name, requested, strlen(requested));
    struct ash_word word;
    errno = 0;
    enum ash_lexer_result lexed = ash_lexer_scan_expansion_string(&lexer, &word);
    char* path = NULL;
    bool expanded = lexed == ASH_LEXER_TOKEN && ash_expand_word(shell, &word, &path, NULL);
    int error = errno;
    ash_word_destroy(&word);
    if (!expanded) {
        free(path);
        if (error == ENOMEM || error == EOVERFLOW) {
            if (error == ENOMEM) {
                ash_diag_oom(shell);
            }
            else {
                ash_diag(shell, "%s expansion: %s", name, strerror(error));
            }
            return ASH_STARTUP_FATAL;
        }
        if (lexed != ASH_LEXER_TOKEN) {
            ash_diag(shell, "%s: bad substitution", requested);
        }
        /* Filename expansion errors return to startup, not the input owner. */
        int status = 1;
        (void)ash_control_consume_unit_discard(shell, &status);
        shell->last_status = 1;
        shell->should_exit = (shell->options & ASH_SHELL_OPTION_NOEXEC) != 0u && !ash_shell_policy_has(&shell->policy, ASH_SHELL_POLICY_INTERACTIVE);
        shell->requested_exit_status = shell->should_exit ? 1 : 0;
        return shell->should_exit ? ASH_STARTUP_EXIT : ASH_STARTUP_CONTINUE;
    }

    char* filename = ash_startup_expand_path(shell, path);
    free(path);
    if (filename == NULL) {
        return ASH_STARTUP_FATAL;
    }
    enum ash_startup_file_result result = filename[0] != '\0' ? ash_startup_execute_file(shell, filename) : ASH_STARTUP_FILE_COMPLETE;
    free(filename);
    return ash_startup_file_outcome(result);
}

enum ash_startup_outcome ash_startup_execute(
    struct ash_shell* shell,
    const struct ash_startup_request* request
) {
    if (shell == NULL || !ash_startup_request_valid(request)) {
        errno = EINVAL;
        if (shell != NULL) {
            ash_diag(shell, "invalid startup request");
        }
        return ASH_STARTUP_FATAL;
    }
    enum ash_startup_outcome outcome = ASH_STARTUP_CONTINUE;
    if (ash_startup_should_read_profiles(shell, request)) {
        outcome = ash_startup_execute_profiles(shell);
    }
    else if (ash_startup_should_read_bashrc(shell, request)) {
        outcome = ash_startup_execute_bashrc(shell, request);
    }
    if (outcome == ASH_STARTUP_CONTINUE &&
        ash_shell_policy_reads_bash_env(&shell->policy)) {
        outcome = ash_startup_execute_env_file(shell, "BASH_ENV");
    }
    else if (outcome == ASH_STARTUP_CONTINUE &&
             ash_shell_policy_reads_env(&shell->policy)) {
        outcome = ash_startup_execute_env_file(shell, "ENV");
    }
    return outcome;
}
