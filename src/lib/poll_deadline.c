#include "lib/poll_deadline.h"

#include <errno.h>
#include <limits.h>
#include <time.h>

static bool bx_poll_monotonic_milliseconds(uint64_t* milliseconds_out) {
    struct timespec now;

    if (milliseconds_out == NULL || clock_gettime(CLOCK_MONOTONIC, &now) != 0 ||
        now.tv_sec < 0 || now.tv_nsec < 0) {
        return false;
    }

    uint64_t seconds = (uint64_t)now.tv_sec;
    uint64_t milliseconds = (uint64_t)now.tv_nsec / 1000000u;
    if (seconds > (UINT64_MAX - milliseconds) / 1000u) {
        return false;
    }

    *milliseconds_out = seconds * 1000u + milliseconds;
    return true;
}

int bx_poll_retry_eintr(struct pollfd* fds, nfds_t nfds, int timeout_ms) {
    int result;

    do {
        result = poll(fds, nfds, timeout_ms);
    } while (result < 0 && errno == EINTR);

    return result;
}

void bx_poll_deadline_init(struct bx_poll_deadline* deadline, int timeout_ms) {
    uint64_t now_ms;

    if (deadline == NULL) {
        return;
    }

    deadline->expires_ms = 0;
    deadline->fallback_timeout_ms = timeout_ms;
    deadline->absolute = false;

    if (timeout_ms <= 0 || !bx_poll_monotonic_milliseconds(&now_ms)) {
        return;
    }

    uint64_t duration_ms = (uint64_t)timeout_ms;
    deadline->expires_ms =
        duration_ms > UINT64_MAX - now_ms ? UINT64_MAX : now_ms + duration_ms;
    deadline->absolute = true;
}

int bx_poll_deadline_wait(struct bx_poll_deadline* deadline, struct pollfd* fds, nfds_t nfds) {
    if (deadline == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (!deadline->absolute) {
        return bx_poll_retry_eintr(fds, nfds, deadline->fallback_timeout_ms);
    }

    for (;;) {
        uint64_t now_ms;
        if (!bx_poll_monotonic_milliseconds(&now_ms)) {
            return bx_poll_retry_eintr(fds, nfds, deadline->fallback_timeout_ms);
        }
        if (now_ms >= deadline->expires_ms) {
            return 0;
        }

        uint64_t remaining_ms = deadline->expires_ms - now_ms;
        int timeout_ms =
            remaining_ms > (uint64_t)INT_MAX ? INT_MAX : (int)remaining_ms;
        int result = poll(fds, nfds, timeout_ms);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        return result;
    }
}

int bx_poll_for_milliseconds(struct pollfd* fds, nfds_t nfds, int timeout_ms) {
    struct bx_poll_deadline deadline;

    bx_poll_deadline_init(&deadline, timeout_ms);
    return bx_poll_deadline_wait(&deadline, fds, nfds);
}
