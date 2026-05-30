#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lib/output_alloc_counter.h"

struct bx_output_alloc_counters {
    atomic_bool enabled;
    const char *applet_name;
    atomic_uint_fast64_t alloc_calls;
    atomic_uint_fast64_t alloc_bytes;
    atomic_uint_fast64_t realloc_calls;
    atomic_uint_fast64_t realloc_bytes;
};

static struct bx_output_alloc_counters current_output_alloc_counters;

static const char *bx_output_alloc_counter_applet_label(const char *name) {
    const char *slash;

    if (name == NULL || name[0] == '\0')
        return "unknown";

    slash = strrchr(name, '/');
    if (slash != NULL)
        name = slash + 1;
    if (name[0] == '-' && name[1] != '\0')
        name++;
    if (name[0] == '\0')
        return "unknown";
    return name;
}

bool bx_output_alloc_counter_enabled(void) {
    return atomic_load_explicit(&current_output_alloc_counters.enabled,
                                memory_order_relaxed);
}

void bx_output_alloc_counter_reset(void) {
    atomic_store_explicit(&current_output_alloc_counters.enabled, false,
                          memory_order_relaxed);
    current_output_alloc_counters.applet_name = NULL;
    atomic_store_explicit(&current_output_alloc_counters.alloc_calls, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_output_alloc_counters.alloc_bytes, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_output_alloc_counters.realloc_calls, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_output_alloc_counters.realloc_bytes, 0u,
                          memory_order_relaxed);
}

void bx_output_alloc_counter_begin_from_env(const char *applet_name) {
    const char *value;

    bx_output_alloc_counter_reset();

    value = getenv("BX_OUTPUT_ALLOC_COUNTERS");
    if (value == NULL || value[0] == '\0' || strcmp(value, "0") == 0)
        return;

    current_output_alloc_counters.applet_name =
        bx_output_alloc_counter_applet_label(applet_name);
    atomic_store_explicit(&current_output_alloc_counters.enabled, true,
                          memory_order_relaxed);
}

void bx_output_alloc_counter_note_alloc(size_t bytes) {
    if (!bx_output_alloc_counter_enabled())
        return;

    atomic_fetch_add_explicit(&current_output_alloc_counters.alloc_calls, 1u,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&current_output_alloc_counters.alloc_bytes,
                              (uint_fast64_t)bytes,
                              memory_order_relaxed);
}

void bx_output_alloc_counter_note_cstring_alloc(const char *text) {
    if (!bx_output_alloc_counter_enabled())
        return;

    bx_output_alloc_counter_note_alloc(text != NULL ? strlen(text) + 1u : 0u);
}

void bx_output_alloc_counter_note_realloc(size_t bytes) {
    if (!bx_output_alloc_counter_enabled())
        return;

    atomic_fetch_add_explicit(&current_output_alloc_counters.realloc_calls, 1u,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&current_output_alloc_counters.realloc_bytes,
                              (uint_fast64_t)bytes,
                              memory_order_relaxed);
}

void bx_output_alloc_counter_report_stderr(void) {
    uint_fast64_t alloc_calls;
    uint_fast64_t alloc_bytes;
    uint_fast64_t realloc_calls;
    uint_fast64_t realloc_bytes;
    const char *applet_name;

    if (!bx_output_alloc_counter_enabled())
        return;

    applet_name = current_output_alloc_counters.applet_name;
    if (applet_name == NULL || applet_name[0] == '\0')
        applet_name = "unknown";

    alloc_calls = atomic_load_explicit(&current_output_alloc_counters.alloc_calls,
                                       memory_order_relaxed);
    alloc_bytes = atomic_load_explicit(&current_output_alloc_counters.alloc_bytes,
                                       memory_order_relaxed);
    realloc_calls = atomic_load_explicit(&current_output_alloc_counters.realloc_calls,
                                         memory_order_relaxed);
    realloc_bytes = atomic_load_explicit(&current_output_alloc_counters.realloc_bytes,
                                         memory_order_relaxed);

    fprintf(stderr,
            "bx-output-alloc-counters: applet=%s output_allocs=%" PRIuFAST64
            " output_alloc_bytes=%" PRIuFAST64
            " output_reallocs=%" PRIuFAST64
            " output_realloc_bytes=%" PRIuFAST64 "\n",
            applet_name,
            alloc_calls,
            alloc_bytes,
            realloc_calls,
            realloc_bytes);
}
