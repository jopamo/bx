#include <stdbool.h>
#include <errno.h>
#include <stdint.h>
#include <stdatomic.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/eventfd.h>

#include "applets/archive/archive_temp.h"
#include "bx/libbx.h"
#include "lib/fd_ops.h"

struct bx_archive_temp_entry {
    char* path;
    struct bx_archive_temp_entry* next;
};

static struct bx_archive_temp_entry* bx_archive_temp_head = NULL;
static bool bx_archive_temp_atexit_installed = false;
static bool bx_archive_temp_signal_handlers_installed = false;
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "archive signal state requires lock-free int atomics");
static atomic_int bx_archive_temp_last_signal = 0;
/* The notification fd lives as long as the installed process-wide handlers. */
static int bx_archive_temp_wake_fd = -1;
static struct bx_cancel_state bx_archive_temp_cancel;

struct bx_archive_temp_signal_slot {
    int signo;
    struct sigaction previous;
    bool have_previous;
};

static struct bx_archive_temp_signal_slot bx_archive_temp_signal_slots[] = {
#ifdef SIGHUP
    {.signo = SIGHUP},
#endif
#ifdef SIGINT
    {.signo = SIGINT},
#endif
#ifdef SIGTERM
    {.signo = SIGTERM},
#endif
};

static void bx_archive_temp_signal_handler(int signo) {
    int error = errno;
    int previous = 0;
    (void)atomic_compare_exchange_strong_explicit(&bx_archive_temp_last_signal, &previous, signo, memory_order_relaxed, memory_order_relaxed);
    uint64_t wake = 1;
    if (bx_archive_temp_wake_fd >= 0) {
        while (write(bx_archive_temp_wake_fd, &wake, sizeof(wake)) < 0 && errno == EINTR) {
        }
    }
    errno = error;
}

static struct bx_archive_temp_entry* bx_archive_temp_find(const char* path) {
    struct bx_archive_temp_entry* entry;

    for (entry = bx_archive_temp_head; entry != NULL; entry = entry->next) {
        if (strcmp(entry->path, path) == 0) {
            return entry;
        }
    }
    return NULL;
}

bool bx_archive_temp_track(const char* path) {
    struct bx_archive_temp_entry* entry;

    if (path == NULL || *path == '\0') {
        return false;
    }
    if (bx_archive_temp_find(path) != NULL) {
        return true;
    }

    if (!bx_archive_temp_atexit_installed) {
        if (atexit(bx_archive_temp_cleanup_all) != 0) {
            return false;
        }
        bx_archive_temp_atexit_installed = true;
    }

    entry = xmalloc(sizeof(*entry));
    entry->path = xstrdup(path);
    entry->next = bx_archive_temp_head;
    bx_archive_temp_head = entry;
    return true;
}

void bx_archive_temp_untrack(const char* path) {
    struct bx_archive_temp_entry** link = &bx_archive_temp_head;

    if (path == NULL || *path == '\0') {
        return;
    }

    while (*link != NULL) {
        struct bx_archive_temp_entry* entry = *link;

        if (strcmp(entry->path, path) == 0) {
            *link = entry->next;
            free(entry->path);
            free(entry);
            return;
        }
        link = &entry->next;
    }
}

void bx_archive_temp_cleanup_all(void) {
    struct bx_archive_temp_entry* entry = bx_archive_temp_head;

    bx_archive_temp_head = NULL;
    while (entry != NULL) {
        struct bx_archive_temp_entry* next = entry->next;

        unlink(entry->path);
        free(entry->path);
        free(entry);
        entry = next;
    }
}

bool bx_archive_temp_install_signal_cleanup(void) {
    struct sigaction action;
    size_t i;

    bx_cancel_state_init(&bx_archive_temp_cancel);
    bx_archive_temp_clear_pending_signal();

    if (bx_archive_temp_signal_handlers_installed) {
        return true;
    }

    bx_archive_temp_wake_fd = bx_fd_eventfd_cloexec(0, EFD_NONBLOCK);
    if (bx_archive_temp_wake_fd < 0)
        return false;
    memset(&action, 0, sizeof(action));
    action.sa_handler = bx_archive_temp_signal_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;

    for (i = 0u; i < (sizeof(bx_archive_temp_signal_slots) / sizeof(bx_archive_temp_signal_slots[0])); i++) {
        if (sigaction(bx_archive_temp_signal_slots[i].signo, &action, &bx_archive_temp_signal_slots[i].previous) != 0) {
            int error = errno;
            while (i > 0u) {
                i--;
                if (bx_archive_temp_signal_slots[i].have_previous) {
                    sigaction(bx_archive_temp_signal_slots[i].signo,
                              &bx_archive_temp_signal_slots[i].previous,
                              NULL);
                    bx_archive_temp_signal_slots[i].have_previous = false;
                }
            }
            bx_fd_cleanup(&bx_archive_temp_wake_fd);
            errno = error;
            return false;
        }
        bx_archive_temp_signal_slots[i].have_previous = true;
    }

    bx_archive_temp_signal_handlers_installed = true;
    return true;
}

int bx_archive_temp_pending_signal(void) {
    int signo = atomic_load_explicit(&bx_archive_temp_last_signal, memory_order_relaxed);
    if (signo != 0) {
        (void)bx_cancel_state_mark_requested(&bx_archive_temp_cancel);
        (void)bx_cancel_state_mark_observed(&bx_archive_temp_cancel);
        (void)bx_cancel_state_mark_draining(&bx_archive_temp_cancel);
    }
    return signo;
}

void bx_archive_temp_clear_pending_signal(void) {
    int error = errno;
    sigset_t blocked, previous;
    sigemptyset(&blocked);
    for (size_t i = 0; i < sizeof(bx_archive_temp_signal_slots) / sizeof(bx_archive_temp_signal_slots[0]); ++i)
        sigaddset(&blocked, bx_archive_temp_signal_slots[i].signo);
    if (sigprocmask(SIG_BLOCK, &blocked, &previous) != 0)
        return;
    atomic_store_explicit(&bx_archive_temp_last_signal, 0, memory_order_relaxed);
    uint64_t wake;
    if (bx_archive_temp_wake_fd >= 0) {
        while (read(bx_archive_temp_wake_fd, &wake, sizeof(wake)) < 0 && errno == EINTR) {
        }
    }
    (void)sigprocmask(SIG_SETMASK, &previous, NULL);
    errno = error;
}

int bx_archive_temp_signal_fd(void) {
    return bx_archive_temp_wake_fd;
}

struct bx_cancel_state* bx_archive_temp_cancel_state(void) {
    return &bx_archive_temp_cancel;
}
