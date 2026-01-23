#include <stdbool.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "applets/archive/archive_temp.h"
#include "bx/libbx.h"

struct bx_archive_temp_entry {
    char* path;
    struct bx_archive_temp_entry* next;
};

static struct bx_archive_temp_entry* bx_archive_temp_head = NULL;
static bool bx_archive_temp_atexit_installed = false;
static bool bx_archive_temp_signal_handlers_installed = false;
static volatile sig_atomic_t bx_archive_temp_last_signal = 0;

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
    if (bx_archive_temp_last_signal == 0) {
        bx_archive_temp_last_signal = signo;
    }
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

    if (bx_archive_temp_signal_handlers_installed) {
        return true;
    }

    memset(&action, 0, sizeof(action));
    action.sa_handler = bx_archive_temp_signal_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;

    for (i = 0u; i < (sizeof(bx_archive_temp_signal_slots) / sizeof(bx_archive_temp_signal_slots[0])); i++) {
        if (sigaction(bx_archive_temp_signal_slots[i].signo, &action, &bx_archive_temp_signal_slots[i].previous) != 0) {
            while (i > 0u) {
                i--;
                if (bx_archive_temp_signal_slots[i].have_previous) {
                    sigaction(bx_archive_temp_signal_slots[i].signo,
                              &bx_archive_temp_signal_slots[i].previous,
                              NULL);
                    bx_archive_temp_signal_slots[i].have_previous = false;
                }
            }
            return false;
        }
        bx_archive_temp_signal_slots[i].have_previous = true;
    }

    bx_archive_temp_signal_handlers_installed = true;
    return true;
}

int bx_archive_temp_pending_signal(void) {
    return (int)bx_archive_temp_last_signal;
}

void bx_archive_temp_clear_pending_signal(void) {
    bx_archive_temp_last_signal = 0;
}
