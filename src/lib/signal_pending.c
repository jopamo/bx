#define _GNU_SOURCE

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <sys/wait.h>

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

static void bx_signal_pending_child_notification(int number) {
    (void)number;
}

int bx_signal_pending_wait_child(struct bx_signal_pending* pending, pid_t pid, int* number) {
    if (pid <= 0 || number == NULL) {
        errno = EINVAL;
        return -1;
    }
    *number = 0;
    if (pending == NULL || pending->count == 0 || atomic_load_explicit(&active, memory_order_relaxed) != pending) {
        errno = ENOTSUP;
        return -1;
    }
    sigset_t blocked = pending->recorded, original_mask;
    sigaddset(&blocked, SIGCHLD);
    if (sigprocmask(SIG_BLOCK, &blocked, &original_mask) < 0)
        return -1;
    struct sigaction original_child, notification = {.sa_handler = bx_signal_pending_child_notification};
    sigemptyset(&notification.sa_mask);
    bool installed = false;
    int result = -1, error = 0;
    if (sigaction(SIGCHLD, NULL, &original_child) < 0) {
        error = errno;
    }
    else if (original_child.sa_handler != SIG_DFL || (original_child.sa_flags & SA_NOCLDWAIT) != 0 || sigismember(&original_mask, SIGCHLD) == 1) {
        error = ENOTSUP;
    }
    else if (sigaction(SIGCHLD, &notification, NULL) < 0) {
        error = errno;
    }
    else {
        installed = true;
        for (;;) {
            for (int i = 1; i < NSIG; i++) {
                if (pending->signals[i]) {
                    *number = i;
                    result = 1;
                    break;
                }
            }
            if (result == 1)
                break;
            siginfo_t info = {0};
            if (waitid(P_PID, (id_t)pid, &info, WEXITED | WNOHANG | WNOWAIT) < 0) {
                if (errno == EINTR)
                    continue;
                error = errno;
                break;
            }
            if (info.si_pid != 0) {
                result = 0;
                break;
            }
            /* Delivery and the readiness check share one masked interval. */
            if (sigsuspend(&original_mask) < 0 && errno != EINTR) {
                error = errno;
                break;
            }
        }
    }
    if (installed && sigaction(SIGCHLD, &original_child, NULL) < 0) {
        if (error == 0)
            error = errno;
        (void)sigaction(SIGCHLD, &original_child, NULL);
    }
    if (sigprocmask(SIG_SETMASK, &original_mask, NULL) < 0) {
        if (error == 0)
            error = errno;
        (void)sigprocmask(SIG_SETMASK, &original_mask, NULL);
    }
    if (error != 0) {
        *number = 0;
        errno = error;
        return -1;
    }
    return result;
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
