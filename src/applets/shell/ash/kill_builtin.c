#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

#include "applets/shell/ash/command.h"
#include "applets/shell/ash/diagnostic.h"
#include "applets/shell/ash/kill_builtin.h"
#include "lib/signal_names.h"
#include "lib/size_parse.h"

static int ash_kill_usage(struct ash_shell* shell) {
    ash_diag(shell, "kill: usage: kill [-s SIGNAL | -n SIGNAL | -SIGNAL] [--] PID...; kill -l [SIGNAL...]");
    return 2;
}

static int ash_kill_list(struct ash_shell* shell, const struct ash_command* command) {
    if (command->word_count == 2u) {
        bx_signal_name_list(stdout);
        return ferror(stdout) ? 1 : 0;
    }
    int status = 0;
    for (size_t i = 2u; i < command->word_count; i++) {
        const char* text = command->words[i];
        int number;
        char name[32];
        if (!bx_signal_name_lookup(text, &number)) {
            ash_diag(shell, "kill: invalid signal '%s'", text);
            status = 1;
        }
        else if (isdigit((unsigned char)text[0])) {
            if (number >= 128)
                number -= 128;
            if (!bx_signal_name_format(number, name, sizeof(name))) {
                ash_diag(shell, "kill: invalid signal '%s'", text);
                status = 1;
            }
            else {
                puts(name);
            }
        }
        else {
            printf("%d\n", number);
        }
    }
    return ferror(stdout) ? 1 : status;
}

int ash_kill_builtin(struct ash_shell* shell, const struct ash_command* command) {
    size_t index = 1u;
    int number = SIGTERM;
    if (index < command->word_count && strcmp(command->words[index], "-l") == 0)
        return ash_kill_list(shell, command);

    if (index < command->word_count && command->words[index][0] == '-' && strcmp(command->words[index], "--") != 0) {
        const char* option = command->words[index++];
        const char* specification = option + 1;
        if (strcmp(option, "-s") == 0 || strcmp(option, "-n") == 0) {
            if (index == command->word_count)
                return ash_kill_usage(shell);
            specification = command->words[index++];
        }
        if (!bx_signal_name_lookup(specification, &number) || number < 0 || number >= NSIG) {
            ash_diag(shell, "kill: invalid signal '%s'", specification);
            return 2;
        }
    }
    if (index < command->word_count && strcmp(command->words[index], "--") == 0)
        index++;
    if (index == command->word_count)
        return ash_kill_usage(shell);

    int status = 0;
    for (; index < command->word_count; index++) {
        const char* text = command->words[index];
        intmax_t parsed;
        if (!bx_size_parse_signed_count(text, &parsed) || parsed < INT_MIN || parsed > INT_MAX) {
            ash_diag(shell, "kill: invalid pid '%s'", text);
            status = 1;
            continue;
        }
        /* Broadcasts must originate in this shell, not an applet child. */
        if (kill((pid_t)parsed, number) < 0) {
            ash_diag(shell, "kill: %s: %s", text, strerror(errno));
            status = 1;
        }
    }
    return status;
}
