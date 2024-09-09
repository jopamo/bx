#ifndef BX_COMMON_PATH_OPS_H
#define BX_COMMON_PATH_OPS_H

#include <stdbool.h>

char *bx_path_join(const char *left, const char *right);
char *bx_path_strip_trailing_slashes_dup(const char *path);
char *bx_path_basename_dup(const char *path);
char *bx_path_parents_layout_dup(const char *source_operand);
bool bx_path_is_dot_or_dotdot(const char *name);

#endif /* BX_COMMON_PATH_OPS_H */
