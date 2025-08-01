#ifndef BX_COMMON_SAME_FILE_H
#define BX_COMMON_SAME_FILE_H

#include <stdbool.h>
#include <sys/stat.h>

bool bx_same_file(const struct stat* a, const struct stat* b);

#endif /* BX_COMMON_SAME_FILE_H */
