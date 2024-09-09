#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <sys/stat.h>

#include "common/stat_ops.h"

bool bx_stat_is_dir_path(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int bx_stat_collect_dest_state(const char *path, struct bx_dest_state *state) {
    memset(state, 0, sizeof(*state));

    if (lstat(path, &state->lst) != 0) {
        if (errno == ENOENT) {
            return 0;
        }
        return -1;
    }
    state->exists_lstat = true;

    if (stat(path, &state->st) == 0) {
        state->exists_stat = true;
        return 0;
    }

    if (errno == ENOENT && S_ISLNK(state->lst.st_mode)) {
        state->dangling_symlink = true;
        return 0;
    }

    return -1;
}

int bx_stat_timespec_compare(const struct timespec *a, const struct timespec *b) {
    if (a->tv_sec < b->tv_sec) {
        return -1;
    }
    if (a->tv_sec > b->tv_sec) {
        return 1;
    }
    if (a->tv_nsec < b->tv_nsec) {
        return -1;
    }
    if (a->tv_nsec > b->tv_nsec) {
        return 1;
    }
    return 0;
}
