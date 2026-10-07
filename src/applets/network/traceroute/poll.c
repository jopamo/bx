/*
    Copyright (c)  2006, 2007		Dmitry Butskoy
                                        <dmitry@butskoy.name>
    License:  GPL v2 or any later

    See COPYING for the status of this software.
*/

#include <stdlib.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>

#include "lib/time_parse.h"
#include "traceroute.h"

void bx_traceroute_add_poll(struct bx_traceroute_ctx* ctx, int fd, int events) {
    unsigned int i;

    // Look for the first empty spot
    for (i = 0; i < ctx->poll.num_polls && ctx->poll.pfd[i].fd > 0; i++)
        ;

    if (i == ctx->poll.num_polls) {
        // Double the allocated size if more space is needed
        if (ctx->poll.num_polls == ctx->poll.max_polls) {
            unsigned int capacity = ctx->poll.max_polls ? ctx->poll.max_polls * 2 : 4;
            struct pollfd* pfd = realloc(ctx->poll.pfd, capacity * sizeof(*pfd));
            if (!pfd)
                bx_traceroute_error(ctx, "realloc");
            ctx->poll.pfd = pfd;
            ctx->poll.max_polls = capacity;
        }
        ctx->poll.num_polls++;
    }

    ctx->poll.pfd[i].fd = fd;
    ctx->poll.pfd[i].events = events;
}

void bx_traceroute_del_poll(struct bx_traceroute_ctx* ctx, int fd) {
    unsigned int i;

    // Look for the file descriptor to remove
    for (i = 0; i < ctx->poll.num_polls && ctx->poll.pfd[i].fd != fd; i++)
        ;

    if (i < ctx->poll.num_polls) {
        ctx->poll.pfd[i].fd = -1;  // Mark it as invalid
    }
}

static unsigned int cleanup_polls(struct bx_traceroute_ctx* ctx) {
    unsigned int i, j;

    // Compact the array to remove holes (invalid file descriptors)
    for (i = 0; i < ctx->poll.num_polls && ctx->poll.pfd[i].fd > 0; i++)
        ;

    if (i < ctx->poll.num_polls) {  // A hole has been found
        for (j = i + 1; j < ctx->poll.num_polls; j++) {
            if (ctx->poll.pfd[j].fd > 0) {
                ctx->poll.pfd[i++] = ctx->poll.pfd[j];
                ctx->poll.pfd[j].fd = -1;
            }
        }
    }

    return i;
}

void bx_traceroute_do_poll(struct bx_traceroute_ctx* ctx, double timeout, void (*callback)(struct bx_traceroute_ctx* ctx, int fd, int revents)) {
    unsigned int nfds, i;
    int n;

    nfds = cleanup_polls(ctx);  // Get the number of active file descriptors

    if (!nfds)
        return;

    int timeout_ms = 0;
    if (!bx_time_seconds_to_milliseconds_int_ceil(timeout, &timeout_ms)) {
        errno = EINVAL;
        bx_traceroute_error(ctx, "poll timeout");
    }

    // Poll the file descriptors with the specified timeout
    n = poll(ctx->poll.pfd, nfds, timeout_ms);
    if (n < 0) {
        if (errno == EINTR)
            return;
        bx_traceroute_error(ctx, "poll");
    }

    // Call the callback for each file descriptor that has events
    for (i = 0; n && i < nfds; i++) {
        if (ctx->poll.pfd[i].revents) {
            callback(ctx, ctx->poll.pfd[i].fd, ctx->poll.pfd[i].revents);
            n--;
        }
    }
}
