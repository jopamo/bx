#include "lib/syscall_counter.h"

#if BX_ENABLE_SYSCALL_COUNTERS
#include <stdatomic.h>

static atomic_bool bx_syscall_counter_runtime_enabled;
static bx_syscall_counter_note_fn bx_syscall_counter_note;
static void* bx_syscall_counter_user;

bool bx_syscall_counter_enabled(void) {
    return atomic_load_explicit(&bx_syscall_counter_runtime_enabled, memory_order_relaxed);
}

void bx_syscall_counter_configure(bool enabled, bx_syscall_counter_note_fn note, void* user) {
    bx_syscall_counter_note = note;
    bx_syscall_counter_user = user;
    atomic_store_explicit(&bx_syscall_counter_runtime_enabled, enabled, memory_order_relaxed);
}

void bx_syscall_counter_note_enabled(const char* name, uint64_t count) {
    bx_syscall_counter_note_fn note = bx_syscall_counter_note;

    if (note != NULL && count != 0u) {
        note(name, count, bx_syscall_counter_user);
    }
}
#else
typedef int bx_syscall_counter_disabled_translation_unit;
#endif
