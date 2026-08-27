/*
 * BusyBox inittab policy, adapted from BusyBox init/init.c at
 * bee252057c7ac69909b8aafeafb8e414e34c7685.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define _GNU_SOURCE

#include "applets/system/init/init_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct bx_init_action_name {
    const char *name;
    uint8_t type;
};

static const struct bx_init_action_name bx_init_action_names[] = {
    {"sysinit", BX_INIT_SYSINIT},
    {"wait", BX_INIT_WAIT},
    {"once", BX_INIT_ONCE},
    {"respawn", BX_INIT_RESPAWN},
    {"askfirst", BX_INIT_ASKFIRST},
    {"ctrlaltdel", BX_INIT_CTRLALTDEL},
    {"shutdown", BX_INIT_SHUTDOWN},
    {"restart", BX_INIT_RESTART},
};

void bx_init_table_init(struct bx_init_table *table) {
    if (table != NULL)
        table->head = NULL;
}

void bx_init_table_destroy(struct bx_init_table *table) {
    if (table == NULL)
        return;

    struct bx_init_action *action = table->head;
    while (action != NULL) {
        struct bx_init_action *next = action->next;
        free(action);
        action = next;
    }
    table->head = NULL;
}

size_t bx_init_table_count(const struct bx_init_table *table) {
    size_t count = 0u;
    if (table == NULL)
        return 0u;
    for (const struct bx_init_action *action = table->head;
         action != NULL;
         action = action->next)
        count++;
    return count;
}

static struct bx_init_action **bx_init_table_end(struct bx_init_table *table) {
    struct bx_init_action **end = &table->head;
    while (*end != NULL)
        end = &(*end)->next;
    return end;
}

static bool bx_init_table_add(struct bx_init_table *table,
                              uint8_t action_type,
                              const char *command,
                              const char *terminal) {
    struct bx_init_action **link = &table->head;
    struct bx_init_action *action;

    while ((action = *link) != NULL) {
        if (strcmp(action->command, command) == 0 &&
            strcmp(action->terminal, terminal) == 0) {
            *link = action->next;
            action->next = NULL;
            action->action_type = action_type;
            *bx_init_table_end(table) = action;
            return true;
        }
        link = &action->next;
    }

    size_t command_len = strlen(command);
    if (command_len > SIZE_MAX - sizeof(*action) - 1u) {
        errno = ENOMEM;
        return false;
    }
    action = calloc(1u, sizeof(*action) + command_len + 1u);
    if (action == NULL)
        return false;

    action->action_type = action_type;
    memcpy(action->command, command, command_len + 1u);
    (void)snprintf(action->terminal, sizeof(action->terminal), "%s", terminal);
    *bx_init_table_end(table) = action;
    return true;
}

static bool bx_init_table_add_defaults(struct bx_init_table *table) {
    static const struct {
        uint8_t type;
        const char *command;
        const char *terminal;
    } defaults[] = {
        {BX_INIT_SYSINIT, BX_INIT_DEFAULT_SCRIPT, ""},
        {BX_INIT_ASKFIRST, "-" BX_INIT_DEFAULT_SHELL, ""},
        {BX_INIT_ASKFIRST, "-" BX_INIT_DEFAULT_SHELL, "/dev/tty2"},
        {BX_INIT_ASKFIRST, "-" BX_INIT_DEFAULT_SHELL, "/dev/tty3"},
        {BX_INIT_ASKFIRST, "-" BX_INIT_DEFAULT_SHELL, "/dev/tty4"},
        {BX_INIT_CTRLALTDEL, "reboot", ""},
        {BX_INIT_SHUTDOWN, "umount -a -r", ""},
        {BX_INIT_SHUTDOWN, "swapoff -a", ""},
        {BX_INIT_RESTART, "init", ""},
    };

    for (size_t i = 0u; i < sizeof(defaults) / sizeof(defaults[0]); i++) {
        if (!bx_init_table_add(table, defaults[i].type,
                               defaults[i].command, defaults[i].terminal))
            return false;
    }
    return true;
}

static int bx_init_action_type_parse(const char *text) {
    for (size_t i = 0u;
         i < sizeof(bx_init_action_names) / sizeof(bx_init_action_names[0]);
         i++) {
        if (strcmp(text, bx_init_action_names[i].name) == 0)
            return bx_init_action_names[i].type;
    }
    return -1;
}

static char *bx_init_read_logical_line(FILE *stream,
                                       char **physical,
                                       size_t *physical_capacity,
                                       unsigned *line_number) {
    char *logical = NULL;
    size_t logical_len = 0u;

    for (;;) {
        ssize_t length = getline(physical, physical_capacity, stream);
        if (length < 0) {
            if (logical != NULL)
                return logical;
            return NULL;
        }
        (*line_number)++;

        size_t part_len = (size_t)length;
        if (part_len > 0u && (*physical)[part_len - 1u] == '\n')
            part_len--;

        bool continued = part_len > 0u && (*physical)[part_len - 1u] == '\\';
        if (continued)
            part_len--;

        if (part_len > SIZE_MAX - logical_len - 1u) {
            free(logical);
            errno = ENOMEM;
            return NULL;
        }
        char *grown = realloc(logical, logical_len + part_len + 1u);
        if (grown == NULL) {
            free(logical);
            return NULL;
        }
        logical = grown;
        memcpy(logical + logical_len, *physical, part_len);
        logical_len += part_len;
        logical[logical_len] = '\0';

        if (!continued)
            return logical;
    }
}

static bool bx_init_parse_line(struct bx_init_table *table,
                               char *line,
                               unsigned line_number,
                               bx_init_config_diag_fn bad_entry,
                               void *diag_user) {
    char *tokens[4] = {NULL, NULL, NULL, NULL};

    if (line[0] == '\0' || line[0] == '#')
        return true;

    char *comment = strchr(line, '#');
    if (comment != NULL)
        *comment = '\0';
    if (line[0] == '\0')
        return true;

    char *cursor = line;
    for (size_t i = 0u; i < 3u; i++) {
        tokens[i] = cursor;
        char *colon = strchr(cursor, ':');
        if (colon == NULL) {
            if (bad_entry != NULL)
                bad_entry(diag_user, line_number);
            return true;
        }
        *colon = '\0';
        cursor = colon + 1;
    }
    tokens[3] = cursor;

    int action_type = bx_init_action_type_parse(tokens[2]);
    if (action_type < 0 || tokens[3][0] == '\0') {
        if (bad_entry != NULL)
            bad_entry(diag_user, line_number);
        return true;
    }

    char terminal[BX_INIT_TERMINAL_SIZE];
    terminal[0] = '\0';
    if (tokens[0][0] != '\0') {
        const char *tty = tokens[0];
        if (strncmp(tty, "/dev/", 5u) == 0)
            tty += 5;
        (void)snprintf(terminal, sizeof(terminal), "/dev/%s", tty);
    }

    return bx_init_table_add(table, (uint8_t)action_type, tokens[3], terminal);
}

static bool bx_init_table_parse_append(struct bx_init_table *table,
                                       const char *path,
                                       bx_init_config_diag_fn bad_entry,
                                       void *diag_user) {
    if (table == NULL || path == NULL) {
        errno = EINVAL;
        return false;
    }

    FILE *stream = fopen(path, "r");
    if (stream == NULL) {
        return bx_init_table_add_defaults(table);
    } else {
        char *physical = NULL;
        size_t physical_capacity = 0u;
        unsigned line_number = 0u;

        for (;;) {
            errno = 0;
            char *line = bx_init_read_logical_line(
                stream, &physical, &physical_capacity, &line_number);
            if (line == NULL) {
                /*
                 * BusyBox's config_read() treats an underlying read error as
                 * EOF.  Allocation failure is different: its xalloc path
                 * invokes init's fatal sleep hook after retaining all entries
                 * parsed so far.
                 */
                if (errno == ENOMEM) {
                    free(physical);
                    fclose(stream);
                    return false;
                }
                break;
            }
            bool ok = bx_init_parse_line(
                table, line, line_number, bad_entry, diag_user);
            free(line);
            if (!ok) {
                free(physical);
                fclose(stream);
                return false;
            }
        }
        free(physical);
        (void)fclose(stream);
    }

    return true;
}

bool bx_init_table_load(struct bx_init_table *table,
                        const char *path,
                        bx_init_config_diag_fn bad_entry,
                        void *diag_user) {
    if (table == NULL || path == NULL) {
        errno = EINVAL;
        return false;
    }

    bx_init_table_destroy(table);
    return bx_init_table_parse_append(
        table, path, bad_entry, diag_user);
}

bool bx_init_table_reload_in_place(struct bx_init_table *table,
                                   const char *path,
                                   bx_init_config_diag_fn bad_entry,
                                   void *diag_user) {
    if (table == NULL || path == NULL) {
        errno = EINVAL;
        return false;
    }

    for (struct bx_init_action *action = table->head;
         action != NULL;
         action = action->next)
        action->action_type = 0;

    return bx_init_table_parse_append(
        table, path, bad_entry, diag_user);
}
