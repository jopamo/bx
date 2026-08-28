#ifndef BX_LIB_PIDFILE_OPS_H
#define BX_LIB_PIDFILE_OPS_H

#include <stdbool.h>
#include <sys/types.h>

struct bx_pidfile {
    char *path;
    pid_t owner;
    dev_t device;
    ino_t inode;
    bool active;
};

void bx_pidfile_init(struct bx_pidfile *pidfile);
bool bx_pidfile_acquire(struct bx_pidfile *pidfile, const char *path);
void bx_pidfile_release(struct bx_pidfile *pidfile);

#endif
