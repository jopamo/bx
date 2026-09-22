#define _GNU_SOURCE
#include "lib/fetch/error.h"
#include "lib/fetch/scheduler.h"
#include "lib/fetch/url.h"
#include "lib/time_parse.h"
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct QueuedURL {
    BxFetchPreparedUrl* target;
    char* output_path;
    int depth;
    int tries_done;
    bool has_retry_ready_time;
    struct timespec retry_ready_time;
    struct QueuedURL* next;
} QueuedURL;

typedef struct HostState {
    char* host;
    int count;
    bool has_next_request_time;
    struct timespec next_request_time;
    struct HostState* next_bucket;
} HostState;

struct BxFetchScheduler {
    const struct bx_fetch_config* cfg;
    BxFetchSchedulerDispatchFn dispatch;
    BxFetchSchedulerPollFn poll;
    void* userdata;
    BxFetchSchedulerObserver observer;

    QueuedURL* queue_head;
    QueuedURL* queue_tail;

    HostState** host_buckets;
    size_t host_bucket_count;
    size_t host_state_count;
    int active_total;
    int active_host_total;
    int active_hostless_total;

    int max_concurrent_global;
    int max_concurrent_per_host;
    bool had_transfer_error;
    bool invariant_failed;
    bool cancelled;
    uint64_t random_state;
    bool started;
    struct timespec started_at;
};

typedef struct {
    BxFetchScheduler* sched;
    BxFetchPreparedUrl* target;
    char* output_path;
    int depth;
    int tries_done;
} TransferInfo;

static HostState* get_host_state(const BxFetchScheduler* s, const char* host);
static bool host_state_note_dispatch(BxFetchScheduler* s, HostState* hs, const struct timespec* dispatched_at);

static bool scheduler_counts_invariant_holds(const BxFetchScheduler* s) {
    if (!s)
        return false;
    if (s->active_total < 0 || s->active_host_total < 0 || s->active_hostless_total < 0) {
        return false;
    }
    if (s->active_total != s->active_host_total + s->active_hostless_total) {
        return false;
    }
    if (s->active_total > s->max_concurrent_global) {
        return false;
    }
    return true;
}

static bool scheduler_record_invariant_failure(BxFetchScheduler* s) {
    if (!s)
        return false;
    s->had_transfer_error = true;
    s->invariant_failed = true;
    errno = EPROTO;
    return false;
}

static bool scheduler_require(BxFetchScheduler* s, bool condition) {
    if (condition)
        return true;
    return scheduler_record_invariant_failure(s);
}

static bool result_counts_as_scheduler_failure(int result) {
    switch ((BxFetchError)result) {
        case BX_FETCH_OK:
        case BX_FETCH_ERROR_HTTP:
        case BX_FETCH_ERROR_CANCELLED:
        case BX_FETCH_ERROR_UNSUPPORTED:
        case BX_FETCH_ERROR_RESOURCE_LIMIT:
            return false;
        default:
            return result != 0;
    }
}

static uint64_t scheduler_random_mix(uint64_t value) {
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdULL;
    value ^= value >> 33;
    value *= 0xc4ceb9fe1a85ec53ULL;
    value ^= value >> 33;
    return value;
}

static uint64_t scheduler_random_next(BxFetchScheduler* s) {
    uint64_t state = s ? s->random_state : 0;
    if (state == 0) {
        state = 0x9e3779b97f4a7c15ULL;
    }

    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    if (s) {
        s->random_state = state;
    }

    return state * 2685821657736338717ULL;
}

static uint64_t scheduler_random_bounded(BxFetchScheduler* s, uint64_t upper_bound) {
    if (upper_bound <= 1)
        return 0;

    uint64_t threshold = (uint64_t)(-upper_bound) % upper_bound;
    for (;;) {
        uint64_t value = scheduler_random_next(s);
        if (value >= threshold) {
            return value % upper_bound;
        }
    }
}

static uint64_t scheduler_initial_random_state(const struct bx_fetch_config* cfg) {
    uint64_t seed = 0x4d495241ULL;
    if (cfg && cfg->download.random_wait) {
        struct timespec now = {0};
        if (clock_gettime(CLOCK_MONOTONIC, &now) == 0) {
            seed ^= (uint64_t)(unsigned long long)now.tv_sec * 1000000007ULL;
            seed ^= (uint64_t)(unsigned long long)now.tv_nsec;
        }
    }

    seed = scheduler_random_mix(seed);
    return seed != 0 ? seed : 0x9e3779b97f4a7c15ULL;
}

static long long scheduler_sample_host_wait_ns(BxFetchScheduler* s) {
    if (!s || s->cfg->download.wait <= 0)
        return 0;

    long long base_wait_ns = (long long)s->cfg->download.wait * 1000000000LL;
    if (!s->cfg->download.random_wait) {
        return base_wait_ns;
    }

    long long min_wait_ns = base_wait_ns / 2;
    uint64_t jitter_ns = scheduler_random_bounded(s, (uint64_t)base_wait_ns + 1ULL);
    return min_wait_ns + (long long)jitter_ns;
}

/* -1: invalid clock/deadline, 0: ready, 1: waiting. */
static int queue_wait_remaining(const QueuedURL* q, const HostState* hs, const struct timespec* now, struct timespec* remaining) {
    const struct timespec* deadline = hs && hs->has_next_request_time ? &hs->next_request_time : NULL;
    if (q->has_retry_ready_time && (!deadline || bx_time_timespec_compare(&q->retry_ready_time, deadline) > 0))
        deadline = &q->retry_ready_time;
    if (!deadline)
        return 0;

    struct timespec delay;
    if (!bx_time_timespec_remaining(deadline, now, &delay))
        return -1;
    if (remaining)
        *remaining = delay;
    return delay.tv_sec > 0 || delay.tv_nsec > 0;
}

static int scheduler_next_wait_duration(BxFetchScheduler* s, const struct timespec* now, struct timespec* min_wait) {
    bool found = false;

    for (QueuedURL* q = s->queue_head; q; q = q->next) {
        HostState* hs = get_host_state(s, bx_fetch_prepared_url_host(q->target));
        if (hs && hs->count >= s->max_concurrent_per_host)
            continue;

        struct timespec remaining = {0};
        int waiting = queue_wait_remaining(q, hs, now, &remaining);
        if (!scheduler_require(s, waiting >= 0))
            return -1;
        if (!waiting)
            continue;

        if (!found || bx_time_timespec_compare(&remaining, min_wait) < 0) {
            *min_wait = remaining;
            found = true;
        }
    }

    return found;
}

static int scheduler_sleep(const struct timespec* duration) {
    struct timespec req = *duration;

    while (nanosleep(&req, &req) != 0) {
        if (errno != EINTR)
            return -1;
    }

    return 0;
}

static void free_queued_url(QueuedURL* q) {
    bx_fetch_prepared_url_free(q->target);
    free(q->output_path);
    free(q);
}

static int scheduler_add_owned_target_with_tries(BxFetchScheduler* s, BxFetchPreparedUrl* target, const char* output_path, int depth, int tries_done, const struct timespec* retry_ready_time) {
    if (!s || !target || !output_path || depth < 0 || tries_done < 0) {
        bx_fetch_prepared_url_free(target);
        errno = EINVAL;
        return -1;
    }
    if (s->cancelled) {
        bx_fetch_prepared_url_free(target);
        errno = ECANCELED;
        return -1;
    }
    if (retry_ready_time && tries_done == 0) {
        bx_fetch_prepared_url_free(target);
        errno = EINVAL;
        return -1;
    }
    if (s->cfg && s->cfg->download.tries > 0 && tries_done > s->cfg->download.tries) {
        bx_fetch_prepared_url_free(target);
        errno = EINVAL;
        return -1;
    }
    const char* host = bx_fetch_prepared_url_host(target);
    if (!host || host[0] == '\0') {
        bx_fetch_prepared_url_free(target);
        errno = EINVAL;
        return -1;
    }

    QueuedURL* q = calloc(1, sizeof(QueuedURL));
    if (!q) {
        bx_fetch_prepared_url_free(target);
        return -1;
    }

    q->target = target;
    q->depth = depth;
    q->output_path = strdup(output_path);
    if (!q->output_path) {
        free_queued_url(q);
        return -1;
    }
    q->tries_done = tries_done;
    if (retry_ready_time) {
        q->retry_ready_time = *retry_ready_time;
        q->has_retry_ready_time = true;
    }

    if (s->queue_tail) {
        s->queue_tail->next = q;
    }
    else {
        s->queue_head = q;
    }
    s->queue_tail = q;

    return 0;
}

static uint64_t host_hash(const char* host) {
    uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char* p = (const unsigned char*)host; p && *p; p++) {
        hash ^= *p;
        hash *= 1099511628211ULL;
    }
    return hash;
}

static HostState* get_host_state(const BxFetchScheduler* s, const char* host) {
    if (!s || !host || s->host_bucket_count == 0)
        return NULL;

    size_t bucket = (size_t)(host_hash(host) % s->host_bucket_count);
    HostState* hs = s->host_buckets[bucket];
    while (hs) {
        if (strcmp(hs->host, host) == 0)
            return hs;
        hs = hs->next_bucket;
    }

    return NULL;
}

static bool host_table_reserve(BxFetchScheduler* s, size_t minimum_states) {
    if (!s)
        return false;
    if (s->host_bucket_count > 0 && minimum_states <= s->host_bucket_count - s->host_bucket_count / 4) {
        return true;
    }

    size_t new_count = 16;
    if (s->host_bucket_count) {
        if (s->host_bucket_count > SIZE_MAX / 2) {
            errno = ENOMEM;
            return false;
        }
        new_count = s->host_bucket_count * 2;
    }
    while (minimum_states > new_count - new_count / 4) {
        if (new_count > SIZE_MAX / 2) {
            errno = ENOMEM;
            return false;
        }
        new_count *= 2;
    }
    if (new_count > SIZE_MAX / sizeof(*s->host_buckets)) {
        errno = ENOMEM;
        return false;
    }

    HostState** buckets = calloc(new_count, sizeof(*buckets));
    if (!buckets)
        return false;

    for (size_t i = 0; i < s->host_bucket_count; i++) {
        HostState* hs = s->host_buckets[i];
        while (hs) {
            HostState* next = hs->next_bucket;
            size_t bucket = (size_t)(host_hash(hs->host) % new_count);
            hs->next_bucket = buckets[bucket];
            buckets[bucket] = hs;
            hs = next;
        }
    }
    free(s->host_buckets);
    s->host_buckets = buckets;
    s->host_bucket_count = new_count;
    return true;
}

static bool host_state_note_dispatch(BxFetchScheduler* s, HostState* hs, const struct timespec* dispatched_at) {
    if (!s || !hs)
        return false;

    struct timespec effective_dispatch_time = {0};
    if (dispatched_at) {
        effective_dispatch_time = *dispatched_at;
    }
    else if (clock_gettime(CLOCK_MONOTONIC, &effective_dispatch_time) != 0) {
        return false;
    }

    long long wait_ns = scheduler_sample_host_wait_ns(s);
    if (!bx_time_timespec_add_nanoseconds(&effective_dispatch_time, (uint64_t)wait_ns, &hs->next_request_time))
        return false;
    hs->has_next_request_time = wait_ns > 0;
    return true;
}

static bool inc_host_active_count(BxFetchScheduler* s, const char* host, const struct timespec* dispatched_at) {
    if (!s)
        return false;

    if (!host) {
        s->active_hostless_total++;
        return true;
    }

    HostState* hs = get_host_state(s, host);
    if (hs) {
        hs->count++;
        if (!host_state_note_dispatch(s, hs, dispatched_at)) {
            hs->count--;
            return false;
        }
        s->active_host_total++;
        return true;
    }

    if (!host_table_reserve(s, s->host_state_count + 1))
        return false;

    hs = calloc(1, sizeof(HostState));
    if (!hs)
        return false;

    hs->host = strdup(host);
    if (!hs->host) {
        free(hs);
        return false;
    }

    hs->count = 1;
    if (!host_state_note_dispatch(s, hs, dispatched_at)) {
        free(hs->host);
        free(hs);
        return false;
    }
    size_t bucket = (size_t)(host_hash(host) % s->host_bucket_count);
    hs->next_bucket = s->host_buckets[bucket];
    s->host_buckets[bucket] = hs;
    s->host_state_count++;
    s->active_host_total++;
    return true;
}

static bool dec_host_active_count(BxFetchScheduler* s, const char* host) {
    if (!s)
        return false;

    if (!host) {
        if (s->active_hostless_total <= 0) {
            return false;
        }
        s->active_hostless_total--;
        return true;
    }

    HostState* hs = get_host_state(s, host);
    if (!hs || hs->count <= 0 || s->active_host_total <= 0) {
        return false;
    }

    hs->count--;
    s->active_host_total--;
    return true;
}

static BxFetchRecoveryDecision on_transfer_complete(void* userdata, int status, BxFetchError result, bool retryable_hint, int64_t retry_after_seconds) {
    TransferInfo* ti = userdata;
    BxFetchScheduler* s = ti ? ti->sched : NULL;
    BxFetchRecoveryDecision decision = {.reason = BX_FETCH_RECOVERY_SCHEDULER_STOPPED};
    if (!ti || !s)
        return decision;

    const char* host = bx_fetch_prepared_url_host(ti->target);
    bool completion_ok = scheduler_require(s, s->active_total > 0);
    if (completion_ok) {
        s->active_total--;
        completion_ok = scheduler_require(s, dec_host_active_count(s, host));
    }
    if (completion_ok) {
        completion_ok = scheduler_require(s, scheduler_counts_invariant_holds(s));
    }

    if (!completion_ok) {
        s->had_transfer_error = true;
    }

    if (completion_ok && !s->cancelled) {
        struct timespec now;
        double elapsed = 0;
        completion_ok = scheduler_require(s, clock_gettime(CLOCK_MONOTONIC, &now) == 0 &&
            bx_time_timespec_elapsed_seconds_double(&s->started_at, &now, &elapsed));
        if (completion_ok)
            decision = bx_fetch_recovery_plan(s->cfg, status, result, retryable_hint,
                                              ti->tries_done, retry_after_seconds, elapsed);
    }
    if (decision.reason == BX_FETCH_RECOVERY_RETRY) {
        if (!scheduler_require(s, ti->tries_done > 0)) {
            s->had_transfer_error = true;
        }
        struct timespec retry_ready_time = {0};
        const struct timespec* retry_ready_time_ptr = NULL;
        int retry_delay = decision.delay_seconds;
        decision.reason = BX_FETCH_RECOVERY_SCHEDULER_STOPPED;
        if (retry_delay > 0) {
            if (!scheduler_require(s, clock_gettime(CLOCK_MONOTONIC, &retry_ready_time) == 0)) {
                s->had_transfer_error = true;
            }
            else if (scheduler_require(s, bx_time_timespec_add_nanoseconds(&retry_ready_time, (uint64_t)retry_delay * UINT64_C(1000000000), &retry_ready_time))) {
                retry_ready_time_ptr = &retry_ready_time;
            }
        }
        if (!s->invariant_failed) {
            BxFetchPreparedUrl* retry_target = ti->target;
            ti->target = NULL;
            if (scheduler_add_owned_target_with_tries(s, retry_target, ti->output_path, ti->depth, ti->tries_done, retry_ready_time_ptr) == 0) {
                decision.reason = BX_FETCH_RECOVERY_RETRY;
                if (s->observer.on_retry) {
                    s->observer.on_retry(s->observer.userdata, retry_target, ti->tries_done + 1, s->cfg->download.tries, retry_delay);
                }
            }
            else {
                s->had_transfer_error = true;
            }
        }
        else {
            s->had_transfer_error = true;
        }
    }
    else if (completion_ok && result_counts_as_scheduler_failure(result)) {
        s->had_transfer_error = true;
    }

    bx_fetch_prepared_url_free(ti->target);
    free(ti->output_path);
    free(ti);
    return decision;
}

BxFetchScheduler* bx_fetch_scheduler_new(const struct bx_fetch_config* cfg,
                                         BxFetchSchedulerDispatchFn dispatch,
                                         BxFetchSchedulerPollFn poll,
                                         void* userdata,
                                         const BxFetchSchedulerObserver* observer) {
    if (!cfg || !dispatch || cfg->download.tries < 1 ||
        cfg->download.max_retry_time < 0 || cfg->download.waitretry < 0) {
        errno = EINVAL;
        return NULL;
    }

    BxFetchScheduler* s = calloc(1, sizeof(BxFetchScheduler));
    if (!s)
        return NULL;

    s->cfg = cfg;
    s->dispatch = dispatch;
    s->poll = poll;
    s->userdata = userdata;
    if (observer)
        s->observer = *observer;

    s->max_concurrent_global = cfg->download.max_threads > 0 ? cfg->download.max_threads : 1;
    s->max_concurrent_per_host = 2;
    s->random_state = scheduler_initial_random_state(cfg);

    return s;
}

void bx_fetch_scheduler_free(BxFetchScheduler* s) {
    if (!s)
        return;

    QueuedURL* q = s->queue_head;
    while (q) {
        QueuedURL* next = q->next;
        free_queued_url(q);
        q = next;
    }

    for (size_t i = 0; i < s->host_bucket_count; i++) {
        HostState* hs = s->host_buckets[i];
        while (hs) {
            HostState* next = hs->next_bucket;
            free(hs->host);
            free(hs);
            hs = next;
        }
    }
    free(s->host_buckets);

    free(s);
}

int bx_fetch_scheduler_add_url(BxFetchScheduler* s, const char* url, const char* output_path, int depth) {
    if (!s || !url || !output_path || depth < 0) {
        errno = EINVAL;
        return -1;
    }
    if (s->cancelled) {
        errno = ECANCELED;
        return -1;
    }
    BxFetchPreparedUrl* target = bx_fetch_url_prepare(url);
    if (!target)
        return -1;
    return scheduler_add_owned_target_with_tries(s, target, output_path, depth, 0, NULL);
}

int bx_fetch_scheduler_add_canonical_url(BxFetchScheduler* s, const char* canonical_url, const char* output_path, int depth) {
    if (!s || !canonical_url || !output_path || depth < 0) {
        errno = EINVAL;
        return -1;
    }
    if (s->cancelled) {
        errno = ECANCELED;
        return -1;
    }
    BxFetchPreparedUrl* target = bx_fetch_url_prepare_canonical(canonical_url);
    if (!target)
        return -1;
    return scheduler_add_owned_target_with_tries(s, target, output_path, depth, 0, NULL);
}

int bx_fetch_scheduler_add_prepared_url(BxFetchScheduler* s, const BxFetchPreparedUrl* target, const char* output_path, int depth) {
    if (!s || !target || !output_path || depth < 0) {
        errno = EINVAL;
        return -1;
    }
    if (s->cancelled) {
        errno = ECANCELED;
        return -1;
    }
    BxFetchPreparedUrl* clone = bx_fetch_prepared_url_clone(target);
    if (!clone)
        return -1;
    return scheduler_add_owned_target_with_tries(s, clone, output_path, depth, 0, NULL);
}

void bx_fetch_scheduler_cancel(BxFetchScheduler* s) {
    if (!s || s->cancelled)
        return;
    s->cancelled = true;

    QueuedURL* q = s->queue_head;
    s->queue_head = NULL;
    s->queue_tail = NULL;
    while (q) {
        QueuedURL* next = q->next;
        free_queued_url(q);
        q = next;
    }
}

bool bx_fetch_scheduler_was_cancelled(const BxFetchScheduler* s) {
    return s && s->cancelled;
}

int bx_fetch_scheduler_run(BxFetchScheduler* s) {
    if (!s) {
        errno = EINVAL;
        return -1;
    }
    bool had_dispatch_error = false;
    s->had_transfer_error = false;
    s->invariant_failed = false;
    if (!s->started) {
        if (clock_gettime(CLOCK_MONOTONIC, &s->started_at) != 0)
            return -1;
        s->started = true;
    }

    while (s->queue_head || s->active_total > 0) {
        if (!scheduler_require(s, scheduler_counts_invariant_holds(s))) {
            return -1;
        }

        QueuedURL* q = s->queue_head;
        QueuedURL* prev = NULL;
        bool started_any = false;

        struct timespec now;
        if (!scheduler_require(s, clock_gettime(CLOCK_MONOTONIC, &now) == 0)) {
            return -1;
        }

        while (q && s->active_total < s->max_concurrent_global) {
            const char* host = bx_fetch_prepared_url_host(q->target);
            HostState* hs = get_host_state(s, host);
            int host_active = hs ? hs->count : 0;
            int waiting = queue_wait_remaining(q, hs, &now, NULL);
            if (!scheduler_require(s, waiting >= 0))
                return -1;

            if (host_active < s->max_concurrent_per_host && !waiting) {
                if (!scheduler_require(s, q->tries_done >= 0)) {
                    return -1;
                }
                if (s->cfg->download.tries > 0 && !scheduler_require(s, q->tries_done < s->cfg->download.tries)) {
                    return -1;
                }

                TransferInfo* ti = calloc(1, sizeof(TransferInfo));
                if (!ti)
                    return -1;

                ti->sched = s;
                ti->target = bx_fetch_prepared_url_clone(q->target);
                ti->output_path = strdup(q->output_path);
                ti->depth = q->depth;
                ti->tries_done = q->tries_done + 1;

                if (!ti->target || !ti->output_path) {
                    bx_fetch_prepared_url_free(ti->target);
                    free(ti->output_path);
                    free(ti);
                    return -1;
                }

                const char* active_host = bx_fetch_prepared_url_host(ti->target);
                if (!inc_host_active_count(s, active_host, &now)) {
                    bx_fetch_prepared_url_free(ti->target);
                    free(ti->output_path);
                    free(ti);
                    scheduler_record_invariant_failure(s);
                    return -1;
                }
                s->active_total++;

                int dispatch_rc = s->dispatch(s->userdata, q->target, q->output_path, q->depth, ti->tries_done, s->cfg->download.tries, on_transfer_complete, ti);

                QueuedURL* next = q->next;
                if (prev) {
                    prev->next = next;
                }
                else {
                    s->queue_head = next;
                }
                if (!next) {
                    s->queue_tail = prev;
                }
                free_queued_url(q);
                q = next;
                started_any = true;

                if (dispatch_rc != 0) {
                    s->active_total--;
                    if (!scheduler_require(s, dec_host_active_count(s, active_host))) {
                        return -1;
                    }
                    bx_fetch_prepared_url_free(ti->target);
                    free(ti->output_path);
                    free(ti);
                    if (dispatch_rc < 0)
                        had_dispatch_error = true;
                }
                if (!scheduler_require(s, scheduler_counts_invariant_holds(s))) {
                    return -1;
                }
                continue;
            }

            prev = q;
            q = q->next;
        }

        if (s->active_total > 0) {
            if (!scheduler_require(s, scheduler_counts_invariant_holds(s))) {
                return -1;
            }
            if (!s->poll || s->poll(s->userdata) != 0)
                return -1;
        }
        else if (!started_any && s->queue_head) {
            struct timespec wait_duration = {0};
            int waiting = scheduler_next_wait_duration(s, &now, &wait_duration);
            if (waiting < 0)
                return -1;
            if (waiting) {
                /* Poll delayed work too, so the invocation owner can expire
                 * its deadline and drain the queue without another request. */
                if (s->cfg->download.max_retry_time > 0 &&
                    (wait_duration.tv_sec > 0 || wait_duration.tv_nsec > 100000000L)) {
                    wait_duration = (struct timespec){.tv_nsec = 100000000L};
                }
                if (scheduler_sleep(&wait_duration) != 0)
                    return -1;
                if (s->cfg->download.max_retry_time > 0 && s->poll && s->poll(s->userdata) != 0)
                    return -1;
                continue;
            }
            break;
        }
    }

    if (!scheduler_require(s, scheduler_counts_invariant_holds(s))) {
        return -1;
    }

    return (had_dispatch_error || s->had_transfer_error || s->invariant_failed) ? -1 : 0;
}
