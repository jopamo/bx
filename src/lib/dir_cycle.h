#ifndef BX_DIR_CYCLE_H
#define BX_DIR_CYCLE_H

#include <stdbool.h>
#include <sys/stat.h>
#include <sys/types.h>

struct bx_dir_stack {
    dev_t dev;
    ino_t ino;
    struct bx_dir_stack* parent;
};

bool bx_dir_stack_contains(const struct bx_dir_stack* stack, const struct stat* st);

#endif
