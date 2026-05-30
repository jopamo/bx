#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lib/compiler.h"
#include "lib/output_alloc_counter.h"

struct bx_output_alloc_counter_values {
    uint_fast64_t alloc_calls;
    uint_fast64_t alloc_bytes;
    uint_fast64_t realloc_calls;
    uint_fast64_t realloc_bytes;
};

struct bx_output_alloc_counter_shard {
    struct bx_output_alloc_counter_values values;
    uint64_t generation;
    struct bx_output_alloc_counter_shard *next;
};

struct bx_output_alloc_counters {
    bool enabled;
    const char *applet_name;
    uint64_t generation;
    pthread_mutex_t lock;
    struct bx_output_alloc_counter_shard *shards;
};

static struct bx_output_alloc_counters current_output_alloc_counters = {
    .generation = 1u,
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

static _Thread_local struct bx_output_alloc_counter_shard *thread_output_alloc_counter_shard;
static _Thread_local uint64_t thread_output_alloc_counter_generation;

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
    return current_output_alloc_counters.enabled;
}

static struct bx_output_alloc_counter_values *bx_output_alloc_counter_thread_values(void) {
    struct bx_output_alloc_counter_shard *shard;

    if (thread_output_alloc_counter_generation == current_output_alloc_counters.generation &&
        thread_output_alloc_counter_shard != NULL) {
        return &thread_output_alloc_counter_shard->values;
    }

    shard = calloc(1u, sizeof(*shard));
    if (shard == NULL)
        return NULL;

    shard->generation = current_output_alloc_counters.generation;
    pthread_mutex_lock(&current_output_alloc_counters.lock);
    shard->next = current_output_alloc_counters.shards;
    current_output_alloc_counters.shards = shard;
    pthread_mutex_unlock(&current_output_alloc_counters.lock);

    thread_output_alloc_counter_shard = shard;
    thread_output_alloc_counter_generation = shard->generation;
    return &shard->values;
}

static void bx_output_alloc_counter_shards_free(
    struct bx_output_alloc_counter_shard *shard) {
    while (shard != NULL) {
        struct bx_output_alloc_counter_shard *next = shard->next;
        free(shard);
        shard = next;
    }
}

static void bx_output_alloc_counter_reduce(
    struct bx_output_alloc_counter_values *out) {
    memset(out, 0, sizeof(*out));

    pthread_mutex_lock(&current_output_alloc_counters.lock);
    for (struct bx_output_alloc_counter_shard *shard =
             current_output_alloc_counters.shards;
         shard != NULL;
         shard = shard->next) {
        out->alloc_calls += shard->values.alloc_calls;
        out->alloc_bytes += shard->values.alloc_bytes;
        out->realloc_calls += shard->values.realloc_calls;
        out->realloc_bytes += shard->values.realloc_bytes;
    }
    pthread_mutex_unlock(&current_output_alloc_counters.lock);
}

void bx_output_alloc_counter_reset(void) {
    struct bx_output_alloc_counter_shard *old_shards;

    current_output_alloc_counters.enabled = false;
    current_output_alloc_counters.applet_name = NULL;

    pthread_mutex_lock(&current_output_alloc_counters.lock);
    current_output_alloc_counters.generation++;
    old_shards = current_output_alloc_counters.shards;
    current_output_alloc_counters.shards = NULL;
    pthread_mutex_unlock(&current_output_alloc_counters.lock);

    bx_output_alloc_counter_shards_free(old_shards);
    thread_output_alloc_counter_shard = NULL;
    thread_output_alloc_counter_generation =
        current_output_alloc_counters.generation;
}

void bx_output_alloc_counter_begin_from_env(const char *applet_name) {
    const char *value;

    bx_output_alloc_counter_reset();

    value = getenv("BX_OUTPUT_ALLOC_COUNTERS");
    if (value == NULL || value[0] == '\0' || strcmp(value, "0") == 0)
        return;

    current_output_alloc_counters.applet_name =
        bx_output_alloc_counter_applet_label(applet_name);
    current_output_alloc_counters.enabled = true;
}

void bx_output_alloc_counter_note_alloc(size_t bytes) {
    struct bx_output_alloc_counter_values *values;

    if (BX_LIKELY(!bx_output_alloc_counter_enabled()))
        return;

    values = bx_output_alloc_counter_thread_values();
    if (values == NULL)
        return;
    values->alloc_calls++;
    values->alloc_bytes += (uint_fast64_t)bytes;
}

void bx_output_alloc_counter_note_cstring_alloc(const char *text) {
    size_t bytes;
    struct bx_output_alloc_counter_values *values;

    if (BX_LIKELY(!bx_output_alloc_counter_enabled()))
        return;

    bytes = text != NULL ? strlen(text) + 1u : 0u;
    values = bx_output_alloc_counter_thread_values();
    if (values == NULL)
        return;
    values->alloc_calls++;
    values->alloc_bytes += (uint_fast64_t)bytes;
}

void bx_output_alloc_counter_note_realloc(size_t bytes) {
    struct bx_output_alloc_counter_values *values;

    if (BX_LIKELY(!bx_output_alloc_counter_enabled()))
        return;

    values = bx_output_alloc_counter_thread_values();
    if (values == NULL)
        return;
    values->realloc_calls++;
    values->realloc_bytes += (uint_fast64_t)bytes;
}

void bx_output_alloc_counter_report_stderr(void) {
    struct bx_output_alloc_counter_values snapshot;
    const char *applet_name;

    if (!bx_output_alloc_counter_enabled())
        return;

    applet_name = current_output_alloc_counters.applet_name;
    if (applet_name == NULL || applet_name[0] == '\0')
        applet_name = "unknown";

    bx_output_alloc_counter_reduce(&snapshot);

    fprintf(stderr,
            "bx-output-alloc-counters: applet=%s output_allocs=%" PRIuFAST64
            " output_alloc_bytes=%" PRIuFAST64
            " output_reallocs=%" PRIuFAST64
            " output_realloc_bytes=%" PRIuFAST64 "\n",
            applet_name,
            snapshot.alloc_calls,
            snapshot.alloc_bytes,
            snapshot.realloc_calls,
            snapshot.realloc_bytes);
}
