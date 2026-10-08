#ifndef BX_LIB_SIGNAL_PENDING_H
#define BX_LIB_SIGNAL_PENDING_H

#include <signal.h>

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
/* Call only after the fork boundary has reset caught kernel dispositions. */
void bx_signal_pending_detach_after_fork(struct bx_signal_pending* pending);

#endif
