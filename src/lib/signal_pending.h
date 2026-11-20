#ifndef BX_LIB_SIGNAL_PENDING_H
#define BX_LIB_SIGNAL_PENDING_H

#include <signal.h>
#include <sys/types.h>
#include <time.h>

struct bx_signal_pending;

/* Single-threaded signal ownership. Only one recorder may catch at a time. */
struct bx_signal_pending* bx_signal_pending_create(void);
/* Restore caught dispositions before destroying their recorder. */
void bx_signal_pending_destroy(struct bx_signal_pending* pending);
/*
 * NULL replacement installs the recording handler without SA_RESTART.
 * Otherwise install the caller's disposition and discard that pending signal.
 * A NULL recorder is allowed only for a non-recording disposition while idle.
 * Return -1 with errno on failure, retaining the previous registration.
 */
int bx_signal_pending_set(struct bx_signal_pending* pending, int number, const struct sigaction* replacement);
/* Consume one recorded signal, or return number zero when none is pending. */
int bx_signal_pending_take(struct bx_signal_pending* pending, int* number);
/*
 * Wait for one child's terminal status without reaping it, or report a pending
 * caught signal without consuming it (return 1 and set number). Return 0 when
 * ready, -1 on error. ENOTSUP leaves nondefault/blocked/auto-reap SIGCHLD
 * policy untouched.
 */
int bx_signal_pending_wait_child(struct bx_signal_pending* pending, pid_t pid, int* number);
/* Wait for readable input, returning -1/EINTR for a caught signal without consuming it. */
/* Return 1 on timeout, 0 when readable or at EOF, -1 on error. */
int bx_signal_pending_wait_readable(struct bx_signal_pending* pending, int fd, const struct timespec* timeout);
/* Call only after the fork boundary has reset caught kernel dispositions. */
void bx_signal_pending_detach_after_fork(struct bx_signal_pending* pending);

#endif
