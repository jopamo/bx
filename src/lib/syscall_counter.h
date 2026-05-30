#ifndef BX_LIB_SYSCALL_COUNTER_H
#define BX_LIB_SYSCALL_COUNTER_H

#include <stdbool.h>
#include <stdint.h>

#include "lib/compiler.h"

#ifndef BX_ENABLE_SYSCALL_COUNTERS
#define BX_ENABLE_SYSCALL_COUNTERS 0
#endif

typedef void (*bx_syscall_counter_note_fn)(const char* name, uint64_t count, void* user);

#if BX_ENABLE_SYSCALL_COUNTERS
bool bx_syscall_counter_enabled(void);
void bx_syscall_counter_configure(bool enabled, bx_syscall_counter_note_fn note, void* user);
void bx_syscall_counter_note_enabled(const char* name, uint64_t count);

#define BX_SYSCALL_COUNTER_NOTE(name, count)                                  \
    do {                                                                     \
        if (BX_UNLIKELY(bx_syscall_counter_enabled())) {                     \
            bx_syscall_counter_note_enabled((name), (count));                \
        }                                                                    \
    } while (0)
#else
static inline bool bx_syscall_counter_enabled(void) {
    return false;
}

static inline void bx_syscall_counter_configure(
    bool enabled,
    bx_syscall_counter_note_fn note,
    void* user
) {
    (void)enabled;
    (void)note;
    (void)user;
}

static inline void bx_syscall_counter_note_enabled(const char* name, uint64_t count) {
    (void)name;
    (void)count;
}

#define BX_SYSCALL_COUNTER_NOTE(name, count) \
    do {                                     \
    } while (0)
#endif

#endif /* BX_LIB_SYSCALL_COUNTER_H */
