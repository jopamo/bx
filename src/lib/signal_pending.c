#define _GNU_SOURCE

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdlib.h>

#include "signal_pending.h"

struct bx_signal_pending {
    sigset_t recorded;
    unsigned int count;
    volatile sig_atomic_t any;
    volatile sig_atomic_t signals[NSIG];
};

_Static_assert(ATOMIC_POINTER_LOCK_FREE == 2, "signal recorder requires lock-free pointers");
static _Atomic(struct bx_signal_pending*) active;

static void bx_signal_pending_handler(int number) {
    int saved_errno = errno;
    struct bx_signal_pending* pending = atomic_load_explicit(&active, memory_order_relaxed);
    if (pending != NULL && number > 0 && number < NSIG) {
        pending->signals[number] = 1;
        pending->any = 1;
    }
    errno = saved_errno;
}

struct bx_signal_pending* bx_signal_pending_create(void) {
    struct bx_signal_pending* pending = calloc(1u, sizeof(*pending));
    if (pending != NULL)
        sigemptyset(&pending->recorded);
    return pending;
}

void bx_signal_pending_destroy(struct bx_signal_pending* pending) {
    if (atomic_load_explicit(&active, memory_order_relaxed) == pending)
        atomic_store_explicit(&active, NULL, memory_order_relaxed);
    free(pending);
}

int bx_signal_pending_set(struct bx_signal_pending* pending, int number, const struct sigaction* replacement) {
    if ((pending == NULL && replacement == NULL) || number <= 0 || number >= NSIG) {
        errno = EINVAL;
        return -1;
    }
    struct bx_signal_pending* previous = atomic_load_explicit(&active, memory_order_relaxed);
    if (previous != NULL && previous != pending) {
        errno = EBUSY;
        return -1;
    }
    if (pending == NULL)
        return sigaction(number, replacement, NULL);
    sigset_t blocked, original_mask;
    sigfillset(&blocked);
    if (sigprocmask(SIG_BLOCK, &blocked, &original_mask) < 0)
        return -1;
    struct sigaction recording = {.sa_handler = bx_signal_pending_handler};
    sigfillset(&recording.sa_mask);
    bool was_recorded = sigismember(&pending->recorded, number) == 1;
    sig_atomic_t was_pending = pending->signals[number];
    unsigned int original_count = pending->count;
    if (replacement == NULL)
        atomic_store_explicit(&active, pending, memory_order_relaxed);
    struct sigaction original;
    if (sigaction(number, replacement == NULL ? &recording : replacement, &original) < 0) {
        int error = errno;
        atomic_store_explicit(&active, previous, memory_order_relaxed);
        (void)sigprocmask(SIG_SETMASK, &original_mask, NULL);
        errno = error;
        return -1;
    }
    if (replacement == NULL) {
        sigaddset(&pending->recorded, number);
        if (!was_recorded)
            pending->count++;
    }
    else {
        sigdelset(&pending->recorded, number);
        if (was_recorded)
            pending->count--;
    }
    if (!was_recorded || replacement != NULL)
        pending->signals[number] = 0;
    if (pending->count == 0 && previous == pending)
        atomic_store_explicit(&active, NULL, memory_order_relaxed);
    if (sigprocmask(SIG_SETMASK, &original_mask, NULL) < 0) {
        int error = errno;
        (void)sigaction(number, &original, NULL);
        if (was_recorded)
            sigaddset(&pending->recorded, number);
        else
            sigdelset(&pending->recorded, number);
        pending->count = original_count;
        pending->signals[number] = was_pending;
        atomic_store_explicit(&active, previous, memory_order_relaxed);
        (void)sigprocmask(SIG_SETMASK, &original_mask, NULL);
        errno = error;
        return -1;
    }
    return 0;
}

int bx_signal_pending_take(struct bx_signal_pending* pending, int* number) {
    if (pending == NULL || number == NULL) {
        errno = EINVAL;
        return -1;
    }
    *number = 0;
    if (!pending->any || atomic_load_explicit(&active, memory_order_relaxed) != pending)
        return 0;
    sigset_t original_mask;
    if (sigprocmask(SIG_BLOCK, &pending->recorded, &original_mask) < 0)
        return -1;
    pending->any = 0;
    for (int i = 1; i < NSIG; i++) {
        if (!pending->signals[i])
            continue;
        if (*number == 0) {
            *number = i;
            pending->signals[i] = 0;
        }
        else {
            pending->any = 1;
        }
    }
    if (sigprocmask(SIG_SETMASK, &original_mask, NULL) < 0) {
        int error = errno;
        if (*number != 0) {
            pending->signals[*number] = 1;
            pending->any = 1;
            *number = 0;
        }
        (void)sigprocmask(SIG_SETMASK, &original_mask, NULL);
        errno = error;
        return -1;
    }
    return 0;
}

void bx_signal_pending_detach_after_fork(struct bx_signal_pending* pending) {
    if (pending == NULL)
        return;
    if (atomic_load_explicit(&active, memory_order_relaxed) == pending)
        atomic_store_explicit(&active, NULL, memory_order_relaxed);
    sigemptyset(&pending->recorded);
    pending->count = 0;
    pending->any = 0;
    for (int i = 1; i < NSIG; i++)
        pending->signals[i] = 0;
}
