/*
 * BusyBox init-compatible definitions.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef BX_APPLETS_SYSTEM_INIT_INTERNAL_H
#define BX_APPLETS_SYSTEM_INIT_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define BX_INIT_TERMINAL_SIZE 32u
#define BX_INIT_DEFAULT_INITTAB "/etc/inittab"
#define BX_INIT_DEFAULT_SCRIPT "/etc/init.d/rcS"
#define BX_INIT_DEFAULT_SHELL "/bin/sh"

#ifndef BX_INIT_FEATURE_KILL_REMOVED
#define BX_INIT_FEATURE_KILL_REMOVED 0
#endif
#ifndef BX_INIT_FEATURE_KILL_DELAY
#define BX_INIT_FEATURE_KILL_DELAY 0
#endif

enum bx_init_action_type {
    BX_INIT_SYSINIT = 0x01,
    BX_INIT_WAIT = 0x02,
    BX_INIT_ONCE = 0x04,
    BX_INIT_RESPAWN = 0x08,
    BX_INIT_ASKFIRST = 0x10,
    BX_INIT_CTRLALTDEL = 0x20,
    BX_INIT_SHUTDOWN = 0x40,
    BX_INIT_RESTART = 0x80,
};

struct bx_init_action {
    struct bx_init_action *next;
    pid_t pid;
    uint8_t action_type;
    char terminal[BX_INIT_TERMINAL_SIZE];
    char command[];
};

struct bx_init_table {
    struct bx_init_action *head;
};

typedef void (*bx_init_config_diag_fn)(void *user, unsigned line_number);

void bx_init_table_init(struct bx_init_table *table);
void bx_init_table_destroy(struct bx_init_table *table);
bool bx_init_table_load(struct bx_init_table *table,
                        const char *path,
                        bx_init_config_diag_fn bad_entry,
                        void *diag_user);
bool bx_init_table_reload_in_place(struct bx_init_table *table,
                                   const char *path,
                                   bx_init_config_diag_fn bad_entry,
                                   void *diag_user);
size_t bx_init_table_count(const struct bx_init_table *table);

struct bx_init_command {
    char *executable;
    char **argv;
    bool login_shell;
};

bool bx_init_command_build(const char *command, struct bx_init_command *result);
void bx_init_command_destroy(struct bx_init_command *command);

int bx_init_run(int argc, char **argv, const char *progname);

#endif
