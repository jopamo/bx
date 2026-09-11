#include "lib/same_file.h"
#include "lib/path_ops.h"

#include <stdlib.h>
#include <string.h>

bool bx_same_file(const struct stat* a, const struct stat* b) {
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino;
}

bool bx_paths_name_same_directory_entry(const char* a, const char* b) {
    char* a_base = bx_path_basename_dup(a);
    char* b_base = bx_path_basename_dup(b);
    bool same = strcmp(a_base, b_base) == 0;
    free(a_base);
    free(b_base);
    if (!same) {
        return false;
    }

    char* a_parent = bx_path_parent_dir_stripped_dup(a);
    char* b_parent = bx_path_parent_dir_stripped_dup(b);
    struct stat a_stat;
    struct stat b_stat;
    /* If a parent cannot be inspected, do not assume replacement is safe. */
    if (stat(a_parent, &a_stat) == 0 && stat(b_parent, &b_stat) == 0) {
        same = bx_same_file(&a_stat, &b_stat);
    }
    free(a_parent);
    free(b_parent);
    return same;
}
