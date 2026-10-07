#include <errno.h>

#include "traceroute.h"

int bx_traceroute_xdp_init(const char* ifname, const char* obj_path) {
    (void)ifname;
    (void)obj_path;
    errno = ENOTSUP;
    return -1;
}

void bx_traceroute_xdp_poll(int fd, int revents) {
    (void)fd;
    (void)revents;
}

void bx_traceroute_xdp_cleanup(void) {}
