#include <errno.h>
#include <stddef.h>

#include "traceroute.h"

int bx_traceroute_bpf_init(const char* obj_path) {
    (void)obj_path;
    errno = ENOTSUP;
    return -1;
}

int bx_traceroute_bpf_decode_event(void* data, size_t data_sz) {
    (void)data;
    (void)data_sz;
    errno = ENOTSUP;
    return -1;
}

void bx_traceroute_bpf_poll(int fd, int revents) {
    (void)fd;
    (void)revents;
}

void bx_traceroute_bpf_print_histograms(void) {}

void bx_traceroute_bpf_cleanup(void) {}
