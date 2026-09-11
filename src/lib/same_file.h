#ifndef BX_COMMON_SAME_FILE_H
#define BX_COMMON_SAME_FILE_H

#include <stdbool.h>
#include <sys/stat.h>

bool bx_same_file(const struct stat* a, const struct stat* b);
bool bx_paths_name_same_directory_entry(const char* a, const char* b);

#endif /* BX_COMMON_SAME_FILE_H */
