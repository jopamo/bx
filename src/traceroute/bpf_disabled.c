#include <errno.h>
#include <stddef.h>

#include "traceroute.h"

int bpf_init(const char* obj_path) {
    (void)obj_path;
    errno = ENOTSUP;
    return -1;
}

int bpf_decode_event(void* data, size_t data_sz) {
    (void)data;
    (void)data_sz;
    errno = ENOTSUP;
    return -1;
}

void bpf_poll(int fd, int revents) {
    (void)fd;
    (void)revents;
}

void bpf_print_histograms(void) {}

void bpf_cleanup(void) {}
