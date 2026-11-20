#ifndef BX_LIB_SWAP_TABLE_H
#define BX_LIB_SWAP_TABLE_H

#ifdef __linux__
#include <stdbool.h>
#include <stddef.h>

struct bx_swap_table {
    char* paths[32]; /* Linux MAX_SWAPFILES */
    size_t len;
};

/* Snapshot /proc/swaps before the caller changes active swap. */
bool bx_swap_table_load(struct bx_swap_table* table);
void bx_swap_table_free(struct bx_swap_table* table);
#endif

#endif
