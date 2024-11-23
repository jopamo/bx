#ifndef BX_COMMON_STAT_OPS_H
#define BX_COMMON_STAT_OPS_H

#include <stdbool.h>
#include <sys/stat.h>
#include <time.h>

struct bx_dest_state {
    bool exists_lstat;
    bool exists_stat;
    bool dangling_symlink;
    struct stat lst;
    struct stat st;
};

bool bx_stat_is_dir_path(const char* path);
int bx_stat_collect_dest_state(const char* path, struct bx_dest_state* state);
int bx_stat_timespec_compare(const struct timespec* a, const struct timespec* b);

#endif /* BX_COMMON_STAT_OPS_H */
