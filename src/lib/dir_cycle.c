#include "dir_cycle.h"

#include <stddef.h>

bool bx_dir_stack_contains(const struct bx_dir_stack* stack, const struct stat* st) {
    for (const struct bx_dir_stack* entry = stack; entry != NULL; entry = entry->parent) {
        if (entry->dev == st->st_dev && entry->ino == st->st_ino) {
            return true;
        }
    }
    return false;
}
