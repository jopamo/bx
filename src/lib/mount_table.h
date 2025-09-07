#ifndef BX_MOUNT_TABLE_H
#define BX_MOUNT_TABLE_H

#include <stdbool.h>
#include <stddef.h>

struct bx_mount_entry {
    char* source;
    char* target;
    char* fstype;
    char* options;
};

struct bx_mount_table {
    struct bx_mount_entry* entries;
    size_t len;
    size_t cap;
};

bool bx_mount_table_load(struct bx_mount_table* table);
void bx_mount_table_free(struct bx_mount_table* table);
const struct bx_mount_entry* bx_mount_table_find_target(const struct bx_mount_table* table, const char* target);
bool bx_mount_table_is_target_or_child(const char* parent, const char* candidate);

#endif
