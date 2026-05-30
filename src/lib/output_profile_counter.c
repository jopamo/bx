#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lib/compiler.h"
#include "lib/output_profile_counter.h"

static uint_fast64_t bx_output_profile_now_ns(void) {
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0u;
    }
    return ((uint_fast64_t)ts.tv_sec * UINT64_C(1000000000)) + (uint_fast64_t)ts.tv_nsec;
}

bool bx_output_profile_env_enabled(void) {
    const char *value = getenv("BX_OUTPUT_PROFILE_COUNTERS");

    return value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
}

void bx_output_profile_sink_init(struct bx_output_profile_sink *sink,
                                 const char *name,
                                 bool enabled) {
    if (!sink) {
        return;
    }

    memset(sink, 0, sizeof(*sink));
    sink->enabled = enabled;
    sink->name = (name && name[0] != '\0') ? name : "unknown";
    (void)pthread_mutex_init(&sink->lock, NULL);
}

void bx_output_profile_sink_init_from_env(struct bx_output_profile_sink *sink,
                                          const char *name) {
    bx_output_profile_sink_init(sink, name, bx_output_profile_env_enabled());
}

void bx_output_profile_sink_reset(struct bx_output_profile_sink *sink) {
    if (!sink) {
        return;
    }

    pthread_mutex_lock(&sink->lock);
    memset(&sink->counts, 0, sizeof(sink->counts));
    pthread_mutex_unlock(&sink->lock);
}

void bx_output_profile_sink_destroy(struct bx_output_profile_sink *sink) {
    if (!sink) {
        return;
    }
    pthread_mutex_destroy(&sink->lock);
    memset(sink, 0, sizeof(*sink));
}

bool bx_output_profile_sink_enabled(const struct bx_output_profile_sink *sink) {
    return sink && sink->enabled;
}

#define BX_OUTPUT_PROFILE_NOTE(FIELD, VALUE)                       \
    do {                                                           \
        if (BX_LIKELY(!bx_output_profile_sink_enabled(sink))) {    \
            return;                                                \
        }                                                          \
        pthread_mutex_lock(&sink->lock);                           \
        sink->counts.FIELD += (VALUE);                             \
        pthread_mutex_unlock(&sink->lock);                         \
    } while (0)

void bx_output_profile_note_bytes(struct bx_output_profile_sink *sink, uint_fast64_t bytes) {
    BX_OUTPUT_PROFILE_NOTE(bytes, bytes);
}

void bx_output_profile_note_record(struct bx_output_profile_sink *sink) {
    BX_OUTPUT_PROFILE_NOTE(records, 1u);
}

void bx_output_profile_note_flush(struct bx_output_profile_sink *sink) {
    BX_OUTPUT_PROFILE_NOTE(flushes, 1u);
}

void bx_output_profile_note_short_write(struct bx_output_profile_sink *sink) {
    BX_OUTPUT_PROFILE_NOTE(short_writes, 1u);
}

void bx_output_profile_note_retry(struct bx_output_profile_sink *sink) {
    BX_OUTPUT_PROFILE_NOTE(retries, 1u);
}

void bx_output_profile_note_epipe(struct bx_output_profile_sink *sink) {
    BX_OUTPUT_PROFILE_NOTE(epipe, 1u);
}

void bx_output_profile_note_allocation(struct bx_output_profile_sink *sink, uint_fast64_t bytes) {
    if (BX_LIKELY(!bx_output_profile_sink_enabled(sink))) {
        return;
    }
    pthread_mutex_lock(&sink->lock);
    sink->counts.allocations++;
    sink->counts.allocation_bytes += bytes;
    pthread_mutex_unlock(&sink->lock);
}

void bx_output_profile_note_formatting_ns(struct bx_output_profile_sink *sink, uint_fast64_t ns) {
    BX_OUTPUT_PROFILE_NOTE(formatting_ns, ns);
}

uint_fast64_t bx_output_profile_format_begin(const struct bx_output_profile_sink *sink) {
    if (BX_LIKELY(!bx_output_profile_sink_enabled(sink))) {
        return 0u;
    }
    return bx_output_profile_now_ns();
}

void bx_output_profile_format_end(struct bx_output_profile_sink *sink, uint_fast64_t start_ns) {
    uint_fast64_t end_ns;

    if (start_ns == 0u || BX_LIKELY(!bx_output_profile_sink_enabled(sink))) {
        return;
    }
    end_ns = bx_output_profile_now_ns();
    if (end_ns > start_ns) {
        bx_output_profile_note_formatting_ns(sink, end_ns - start_ns);
    }
}

void bx_output_profile_snapshot(struct bx_output_profile_sink *sink,
                                struct bx_output_profile_counts *out) {
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!sink) {
        return;
    }
    pthread_mutex_lock(&sink->lock);
    *out = sink->counts;
    pthread_mutex_unlock(&sink->lock);
}

void bx_output_profile_report(FILE *stream, struct bx_output_profile_sink *sink) {
    struct bx_output_profile_counts snapshot;
    const char *name;

    if (!stream || !bx_output_profile_sink_enabled(sink)) {
        return;
    }

    bx_output_profile_snapshot(sink, &snapshot);
    name = (sink->name && sink->name[0] != '\0') ? sink->name : "unknown";
    fprintf(stream,
            "bx-output-profile-counters: sink=%s"
            " bytes=%" PRIuFAST64
            " records=%" PRIuFAST64
            " flushes=%" PRIuFAST64
            " short_writes=%" PRIuFAST64
            " retries=%" PRIuFAST64
            " epipe=%" PRIuFAST64
            " allocations=%" PRIuFAST64
            " allocation_bytes=%" PRIuFAST64
            " formatting_ns=%" PRIuFAST64 "\n",
            name,
            snapshot.bytes,
            snapshot.records,
            snapshot.flushes,
            snapshot.short_writes,
            snapshot.retries,
            snapshot.epipe,
            snapshot.allocations,
            snapshot.allocation_bytes,
            snapshot.formatting_ns);
}

void bx_output_profile_report_stderr(struct bx_output_profile_sink *sink) {
    bx_output_profile_report(stderr, sink);
}
