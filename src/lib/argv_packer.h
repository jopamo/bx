#ifndef BX_LIB_ARGV_PACKER_H
#define BX_LIB_ARGV_PACKER_H

#include <stddef.h>

size_t bx_argv_environment_bytes(void);
size_t bx_argv_bytes(char **argv);
size_t bx_argv_bytes_with_items(char **base_argv, int base_argc,
                                char **items, int start, int count);
size_t bx_argv_effective_char_limit(int max_chars);
int bx_argv_select_batch_count(char **base_argv, int base_argc,
                               char **items, const int *line_groups,
                               int item_count, int start,
                               int max_args, int max_lines,
                               size_t char_limit);

#endif
