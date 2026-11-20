#define _GNU_SOURCE
#include "swap_table.h"

#ifdef __linux__
#include <errno.h>
#include <limits.h>
#include <linux/magic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/vfs.h>

#include "mount_table.h"

void bx_swap_table_free(struct bx_swap_table* table) {
    while (table->len)
        free(table->paths[--table->len]);
}

bool bx_swap_table_load(struct bx_swap_table* table) {
    table->len = 0;
    FILE* file = fopen("/proc/swaps", "re");
    if (!file)
        return false;
    struct statfs fs;
    int error = 0;
    if (fstatfs(fileno(file), &fs) < 0)
        error = errno;
    else if (fs.f_type != PROC_SUPER_MAGIC)
        error = EOPNOTSUPP;
    char line[4 * PATH_MAX + 128];
    if (!error && (!fgets(line, sizeof(line), file) ||
                   strncmp(line, "Filename\t", 9) || !strchr(line, '\n')))
        error = ferror(file) ? errno : EINVAL;
    while (!error && fgets(line, sizeof(line), file)) {
        size_t end = strcspn(line, " \t");
        if (line[0] != '/' || !line[end] || !strchr(line, '\n') ||
            table->len == sizeof(table->paths) / sizeof(table->paths[0])) {
            error = EINVAL;
            break;
        }
        line[end] = '\0';
        table->paths[table->len++] = bx_mount_unescape_field(line);
    }
    if (!error && ferror(file))
        error = errno;
    if (fclose(file) != 0 && !error)
        error = errno;
    if (error) {
        bx_swap_table_free(table);
        errno = error;
    }
    return !error;
}
#endif
