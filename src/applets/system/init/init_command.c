/*
 * BusyBox init command grammar, adapted from BusyBox init/init.c at
 * bee252057c7ac69909b8aafeafb8e414e34c7685.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define _GNU_SOURCE

#include "applets/system/init/init_internal.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "lib/argv_packer.h"

static bool bx_init_command_account(char **argv) {
    size_t bytes = bx_argv_bytes(argv);
    size_t limit = bx_argv_effective_char_limit(0);
    if (bytes == (size_t)-1 || (limit > 0u && bytes > limit)) {
        errno = E2BIG;
        return false;
    }
    return true;
}

static bool bx_init_command_shell(const char *command,
                                  bool login_shell,
                                  struct bx_init_command *result) {
    size_t length = strlen(command);
    if (length > SIZE_MAX - sizeof("exec ")) {
        errno = ENOMEM;
        return false;
    }

    char *script = malloc(sizeof("exec ") + length);
    char **argv = calloc(4u, sizeof(*argv));
    char *argv0 = strdup(login_shell ? "-" BX_INIT_DEFAULT_SHELL
                                     : BX_INIT_DEFAULT_SHELL);
    char *option = strdup("-c");
    char *executable = strdup(BX_INIT_DEFAULT_SHELL);
    if (script == NULL || argv == NULL || argv0 == NULL ||
        option == NULL || executable == NULL) {
        free(script);
        free(argv);
        free(argv0);
        free(option);
        free(executable);
        return false;
    }

    memcpy(script, "exec ", sizeof("exec ") - 1u);
    memcpy(script + sizeof("exec ") - 1u, command, length + 1u);
    argv[0] = argv0;
    argv[1] = option;
    argv[2] = script;

    if (!bx_init_command_account(argv)) {
        bx_argv_free(argv);
        free(executable);
        return false;
    }

    result->executable = executable;
    result->argv = argv;
    result->login_shell = login_shell;
    return true;
}

static bool bx_init_command_plain(const char *command_with_dash,
                                  bool login_shell,
                                  struct bx_init_command *result) {
    char *words = strdup(command_with_dash);
    if (words == NULL) {
        free(words);
        return false;
    }

    size_t count = 0u;
    char *scan = words;
    while (*scan != '\0') {
        scan += strspn(scan, " \t");
        if (*scan == '\0')
            break;
        count++;
        scan += strcspn(scan, " \t");
    }
    if (count > (SIZE_MAX / sizeof(char *)) - 1u) {
        free(words);
        errno = ENOMEM;
        return false;
    }

    char **argv = calloc(count + 1u, sizeof(*argv));
    if (argv == NULL) {
        free(words);
        return false;
    }

    size_t index = 0u;
    char *next = words;
    char *word;
    while ((word = strsep(&next, " \t")) != NULL) {
        if (word[0] == '\0')
            continue;
        argv[index] = strdup(word);
        if (argv[index] == NULL) {
            bx_argv_free(argv);
            free(words);
            return false;
        }
        index++;
    }

    char *executable = strdup(words + (login_shell ? 1u : 0u));
    free(words);
    if (executable == NULL) {
        bx_argv_free(argv);
        return false;
    }

    if (!bx_init_command_account(argv)) {
        bx_argv_free(argv);
        free(executable);
        return false;
    }

    result->executable = executable;
    result->argv = argv;
    result->login_shell = login_shell;
    return true;
}

bool bx_init_command_build(const char *command, struct bx_init_command *result) {
    static const char shell_metacharacters[] = "~`!$^&*()=|\\{}[];\"'<>?";

    if (command == NULL || result == NULL) {
        errno = EINVAL;
        return false;
    }
    memset(result, 0, sizeof(*result));

    bool login_shell = command[0] == '-';
    const char *executable = command + (login_shell ? 1u : 0u);
    if (strpbrk(executable, shell_metacharacters) != NULL)
        return bx_init_command_shell(executable, login_shell, result);
    return bx_init_command_plain(command, login_shell, result);
}

void bx_init_command_destroy(struct bx_init_command *command) {
    if (command == NULL)
        return;
    free(command->executable);
    bx_argv_free(command->argv);
    memset(command, 0, sizeof(*command));
}
