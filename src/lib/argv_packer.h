#ifndef BX_LIB_ARGV_PACKER_H
#define BX_LIB_ARGV_PACKER_H

#include <stddef.h>

typedef size_t (*bx_argv_batch_bytes_fn)(void *user, int start, int count);
typedef size_t (*bx_argv_marker_count_fn)(const char *arg, void *user);
typedef size_t (*bx_argv_expand_bytes_fn)(const char *arg, const char *item, void *user);
typedef char *(*bx_argv_expand_arg_fn)(const char *arg, const char *item, void *user);

size_t bx_argv_environment_bytes(void);
size_t bx_argv_bytes(char *const *argv);
int bx_argv_parse_command(const char *command, char ***argv_out);
size_t bx_argv_bytes_with_items(const char *const *base_argv, int base_argc,
                                char **items, int start, int count);
char **bx_argv_build_with_item_expansion(const char *const *base_argv, int base_argc,
                                         char **items, int start, int count,
                                         int batch_mode,
                                         bx_argv_marker_count_fn marker_count_fn,
                                         bx_argv_expand_arg_fn expand_arg_fn,
                                         int *saw_marker,
                                         void *user);
size_t bx_argv_bytes_with_item_expansion(const char *const *base_argv, int base_argc,
                                         char **items, int start, int count,
                                         int batch_mode,
                                         bx_argv_marker_count_fn marker_count_fn,
                                         bx_argv_expand_bytes_fn expand_bytes_fn,
                                         int *saw_marker,
                                         void *user);
size_t bx_argv_effective_char_limit(int max_chars);
int bx_argv_select_batch_count_by_bytes(int item_count, int start,
                                        int max_args, int max_lines,
                                        size_t char_limit,
                                        bx_argv_batch_bytes_fn bytes_fn,
                                        void *user);
int bx_argv_select_batch_count(const char *const *base_argv, int base_argc,
                               char **items, const int *line_groups,
                               int item_count, int start,
                               int max_args, int max_lines,
                               size_t char_limit);
void bx_argv_free(char **argv);

#endif
