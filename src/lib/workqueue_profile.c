#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lib/compiler.h"
#include "lib/workqueue_profile.h"

static uint_fast64_t bx_workqueue_profile_now_ns(void) {
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0u;
    }
    return ((uint_fast64_t)ts.tv_sec * UINT64_C(1000000000)) + (uint_fast64_t)ts.tv_nsec;
}

bool bx_workqueue_profile_env_enabled(void) {
    const char *value = getenv("BX_WORKQUEUE_PROFILE_COUNTERS");

    return value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
}

void bx_workqueue_profile_sink_init(struct bx_workqueue_profile_sink *sink,
                                    const char *name,
                                    const char *kind,
                                    bool enabled) {
    if (!sink) {
        return;
    }

    memset(sink, 0, sizeof(*sink));
    sink->enabled = enabled;
    sink->name = (name && name[0] != '\0') ? name : "unknown";
    sink->kind = (kind && kind[0] != '\0') ? kind : "unknown";
    (void)pthread_mutex_init(&sink->lock, NULL);
}

void bx_workqueue_profile_sink_init_from_env(struct bx_workqueue_profile_sink *sink,
                                             const char *name,
                                             const char *kind) {
    bx_workqueue_profile_sink_init(sink, name, kind, bx_workqueue_profile_env_enabled());
}

void bx_workqueue_profile_sink_reset(struct bx_workqueue_profile_sink *sink) {
    if (!sink) {
        return;
    }

    pthread_mutex_lock(&sink->lock);
    memset(&sink->counts, 0, sizeof(sink->counts));
    pthread_mutex_unlock(&sink->lock);
}

void bx_workqueue_profile_sink_destroy(struct bx_workqueue_profile_sink *sink) {
    if (!sink) {
        return;
    }

    pthread_mutex_destroy(&sink->lock);
    memset(sink, 0, sizeof(*sink));
}

bool bx_workqueue_profile_sink_enabled(const struct bx_workqueue_profile_sink *sink) {
    return sink && sink->enabled;
}

uint_fast64_t bx_workqueue_profile_wait_begin(const struct bx_workqueue_profile_sink *sink) {
    if (BX_LIKELY(!bx_workqueue_profile_sink_enabled(sink))) {
        return 0u;
    }
    return bx_workqueue_profile_now_ns();
}

void bx_workqueue_profile_wait_end(struct bx_workqueue_profile_sink *sink,
                                   enum bx_workqueue_profile_wait_side side,
                                   uint_fast64_t start_ns) {
    uint_fast64_t end_ns;
    uint_fast64_t delta_ns = 0u;

    if (start_ns == 0u || BX_LIKELY(!bx_workqueue_profile_sink_enabled(sink))) {
        return;
    }

    end_ns = bx_workqueue_profile_now_ns();
    if (end_ns > start_ns) {
        delta_ns = end_ns - start_ns;
    }

    pthread_mutex_lock(&sink->lock);
    if (side == BX_WORKQUEUE_PROFILE_PRODUCER_WAIT) {
        sink->counts.producer_wait_samples++;
        sink->counts.producer_wait_ns += delta_ns;
    } else {
        sink->counts.consumer_wait_samples++;
        sink->counts.consumer_wait_ns += delta_ns;
    }
    pthread_mutex_unlock(&sink->lock);
}

void bx_workqueue_profile_note_wakeup(struct bx_workqueue_profile_sink *sink,
                                      enum bx_workqueue_profile_wakeup_side side,
                                      bool broadcast) {
    if (BX_LIKELY(!bx_workqueue_profile_sink_enabled(sink))) {
        return;
    }

    pthread_mutex_lock(&sink->lock);
    if (side == BX_WORKQUEUE_PROFILE_WAKE_PRODUCER) {
        sink->counts.producer_wakeups++;
    } else {
        sink->counts.consumer_wakeups++;
    }
    if (broadcast) {
        sink->counts.broadcast_wakeups++;
    }
    pthread_mutex_unlock(&sink->lock);
}

void bx_workqueue_profile_note_submit(struct bx_workqueue_profile_sink *sink) {
    if (BX_LIKELY(!bx_workqueue_profile_sink_enabled(sink))) {
        return;
    }

    pthread_mutex_lock(&sink->lock);
    sink->counts.items_submitted++;
    pthread_mutex_unlock(&sink->lock);
}

void bx_workqueue_profile_note_complete(struct bx_workqueue_profile_sink *sink) {
    if (BX_LIKELY(!bx_workqueue_profile_sink_enabled(sink))) {
        return;
    }

    pthread_mutex_lock(&sink->lock);
    sink->counts.items_completed++;
    pthread_mutex_unlock(&sink->lock);
}

void bx_workqueue_profile_note_depth(struct bx_workqueue_profile_sink *sink,
                                     uint_fast64_t depth) {
    if (BX_LIKELY(!bx_workqueue_profile_sink_enabled(sink))) {
        return;
    }

    pthread_mutex_lock(&sink->lock);
    if (depth > sink->counts.max_depth) {
        sink->counts.max_depth = depth;
    }
    pthread_mutex_unlock(&sink->lock);
}

void bx_workqueue_profile_snapshot(struct bx_workqueue_profile_sink *sink,
                                   struct bx_workqueue_profile_counts *out) {
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

uint_fast64_t bx_workqueue_profile_total_wakeups(
    const struct bx_workqueue_profile_counts *counts) {
    if (!counts) {
        return 0u;
    }
    return counts->producer_wakeups + counts->consumer_wakeups;
}

uint_fast64_t bx_workqueue_profile_wakeups_per_1000_completed(
    const struct bx_workqueue_profile_counts *counts) {
    uint_fast64_t completed;
    uint_fast64_t wakeups;

    if (!counts) {
        return 0u;
    }
    completed = counts->items_completed != 0u ? counts->items_completed : counts->items_submitted;
    if (completed == 0u) {
        return 0u;
    }
    wakeups = bx_workqueue_profile_total_wakeups(counts);
    if (wakeups > UINT64_MAX / UINT64_C(1000)) {
        return UINT64_MAX;
    }
    return (wakeups * UINT64_C(1000)) / completed;
}

void bx_workqueue_profile_report(FILE *stream, struct bx_workqueue_profile_sink *sink) {
    struct bx_workqueue_profile_counts snapshot;
    const char *name;
    const char *kind;

    if (!stream || !bx_workqueue_profile_sink_enabled(sink)) {
        return;
    }

    bx_workqueue_profile_snapshot(sink, &snapshot);
    name = (sink->name && sink->name[0] != '\0') ? sink->name : "unknown";
    kind = (sink->kind && sink->kind[0] != '\0') ? sink->kind : "unknown";
    fprintf(stream,
            "bx-workqueue-profile-counters: queue=%s kind=%s"
            " submitted=%" PRIuFAST64
            " completed=%" PRIuFAST64
            " producer_wait_samples=%" PRIuFAST64
            " producer_wait_ns=%" PRIuFAST64
            " consumer_wait_samples=%" PRIuFAST64
            " consumer_wait_ns=%" PRIuFAST64
            " producer_wakeups=%" PRIuFAST64
            " consumer_wakeups=%" PRIuFAST64
            " broadcast_wakeups=%" PRIuFAST64
            " wakeups=%" PRIuFAST64
            " wakeups_per_1000_completed=%" PRIuFAST64
            " max_depth=%" PRIuFAST64 "\n",
            name,
            kind,
            snapshot.items_submitted,
            snapshot.items_completed,
            snapshot.producer_wait_samples,
            snapshot.producer_wait_ns,
            snapshot.consumer_wait_samples,
            snapshot.consumer_wait_ns,
            snapshot.producer_wakeups,
            snapshot.consumer_wakeups,
            snapshot.broadcast_wakeups,
            bx_workqueue_profile_total_wakeups(&snapshot),
            bx_workqueue_profile_wakeups_per_1000_completed(&snapshot),
            snapshot.max_depth);
}

void bx_workqueue_profile_report_stderr(struct bx_workqueue_profile_sink *sink) {
    bx_workqueue_profile_report(stderr, sink);
}
