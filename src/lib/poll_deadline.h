#ifndef BX_LIB_POLL_DEADLINE_H
#define BX_LIB_POLL_DEADLINE_H

#include <stdbool.h>
#include <poll.h>
#include <stdint.h>

struct bx_poll_deadline {
    uint64_t expires_ms;
    int fallback_timeout_ms;
    bool absolute;
};

void bx_poll_deadline_init(struct bx_poll_deadline* deadline, int timeout_ms);
int bx_poll_deadline_wait(struct bx_poll_deadline* deadline, struct pollfd* fds, nfds_t nfds);
int bx_poll_for_milliseconds(struct pollfd* fds, nfds_t nfds, int timeout_ms);
int bx_poll_retry_eintr(struct pollfd* fds, nfds_t nfds, int timeout_ms);

#endif
