#ifndef BX_LIB_WORKQUEUE_PROFILE_H
#define BX_LIB_WORKQUEUE_PROFILE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <pthread.h>

enum bx_workqueue_profile_wait_side {
    BX_WORKQUEUE_PROFILE_PRODUCER_WAIT = 0,
    BX_WORKQUEUE_PROFILE_CONSUMER_WAIT,
};

enum bx_workqueue_profile_wakeup_side {
    BX_WORKQUEUE_PROFILE_WAKE_PRODUCER = 0,
    BX_WORKQUEUE_PROFILE_WAKE_CONSUMER,
};

struct bx_workqueue_profile_counts {
    uint_fast64_t items_submitted;
    uint_fast64_t items_completed;
    uint_fast64_t producer_wait_samples;
    uint_fast64_t consumer_wait_samples;
    uint_fast64_t producer_wait_ns;
    uint_fast64_t consumer_wait_ns;
    uint_fast64_t producer_wakeups;
    uint_fast64_t consumer_wakeups;
    uint_fast64_t broadcast_wakeups;
    uint_fast64_t max_depth;
};

struct bx_workqueue_profile_sink {
    bool enabled;
    const char *name;
    const char *kind;
    pthread_mutex_t lock;
    struct bx_workqueue_profile_counts counts;
};

bool bx_workqueue_profile_env_enabled(void);
void bx_workqueue_profile_sink_init(struct bx_workqueue_profile_sink *sink,
                                    const char *name,
                                    const char *kind,
                                    bool enabled);
void bx_workqueue_profile_sink_init_from_env(struct bx_workqueue_profile_sink *sink,
                                             const char *name,
                                             const char *kind);
void bx_workqueue_profile_sink_reset(struct bx_workqueue_profile_sink *sink);
void bx_workqueue_profile_sink_destroy(struct bx_workqueue_profile_sink *sink);
bool bx_workqueue_profile_sink_enabled(const struct bx_workqueue_profile_sink *sink);
uint_fast64_t bx_workqueue_profile_wait_begin(const struct bx_workqueue_profile_sink *sink);
void bx_workqueue_profile_wait_end(struct bx_workqueue_profile_sink *sink,
                                   enum bx_workqueue_profile_wait_side side,
                                   uint_fast64_t start_ns);
void bx_workqueue_profile_note_wakeup(struct bx_workqueue_profile_sink *sink,
                                      enum bx_workqueue_profile_wakeup_side side,
                                      bool broadcast);
void bx_workqueue_profile_note_submit(struct bx_workqueue_profile_sink *sink);
void bx_workqueue_profile_note_complete(struct bx_workqueue_profile_sink *sink);
void bx_workqueue_profile_note_depth(struct bx_workqueue_profile_sink *sink,
                                     uint_fast64_t depth);
void bx_workqueue_profile_snapshot(struct bx_workqueue_profile_sink *sink,
                                   struct bx_workqueue_profile_counts *out);
uint_fast64_t bx_workqueue_profile_total_wakeups(
    const struct bx_workqueue_profile_counts *counts);
uint_fast64_t bx_workqueue_profile_wakeups_per_1000_completed(
    const struct bx_workqueue_profile_counts *counts);
void bx_workqueue_profile_report(FILE *stream, struct bx_workqueue_profile_sink *sink);
void bx_workqueue_profile_report_stderr(struct bx_workqueue_profile_sink *sink);

#endif /* BX_LIB_WORKQUEUE_PROFILE_H */
