#include <errno.h>

#include "traceroute.h"

int xdp_init(const char* ifname, const char* obj_path) {
    (void)ifname;
    (void)obj_path;
    errno = ENOTSUP;
    return -1;
}

void xdp_poll(int fd, int revents) {
    (void)fd;
    (void)revents;
}

void xdp_cleanup(void) {}
