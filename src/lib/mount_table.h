#ifndef BX_MOUNT_TABLE_H
#define BX_MOUNT_TABLE_H

#include <stdbool.h>
#include <stddef.h>

struct bx_mount_entry {
    char* source;
    char* target;
    char* fstype;
    char* options;
    size_t line;
};

struct bx_mount_table {
    struct bx_mount_entry* entries;
    size_t len;
    size_t cap;
};

char* bx_mount_unescape_field(const char* text);
bool bx_mount_table_load(struct bx_mount_table* table);
/* On failure, line is the offending fstab line, or zero for an open error. */
bool bx_mount_table_load_fstab(struct bx_mount_table* table, const char* path, size_t* line);
#ifdef __linux__
/* 1 for a visible mount root, 0 for an ordinary path, -1 on lookup failure. */
int bx_mount_is_mountpoint(const char* path);
#endif
void bx_mount_table_free(struct bx_mount_table* table);
const struct bx_mount_entry* bx_mount_table_find_target(const struct bx_mount_table* table, const char* target);
bool bx_mount_table_is_target_or_child(const char* parent, const char* candidate);

#endif
