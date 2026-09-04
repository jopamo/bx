#define _GNU_SOURCE
#include "lib/fetch/error.h"
#include "lib/fetch/http_status.h"
#include "lib/fetch/scheduler.h"
#include "lib/fetch/output_policy.h"
#include "lib/fetch/url.h"
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct QueuedURL {
    char* url;
    char* output_path;
    char* host;
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
    struct HostState* next;
} HostState;

struct Scheduler {
    const EffectiveConfig* cfg;
    SchedulerDispatchFn dispatch;
    SchedulerPollFn poll;
    void* userdata;

    QueuedURL* queue_head;
    QueuedURL* queue_tail;

    HostState* host_states;
    int active_total;
    int active_host_total;
    int active_hostless_total;

    int max_concurrent_global;
    int max_concurrent_per_host;
    bool had_transfer_error;
    bool invariant_failed;
    uint64_t random_state;
};

typedef struct {
    Scheduler* sched;
    char* url;
    char* output_path;
    char* host;
    int tries_done;
} TransferInfo;

static HostState* get_host_state(const Scheduler* s, const char* host);
static bool host_state_note_dispatch(Scheduler* s, HostState* hs, const struct timespec* dispatched_at);

static bool scheduler_counts_invariant_holds(const Scheduler* s) {
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

static bool scheduler_record_invariant_failure(Scheduler* s, const char* message) {
    if (!s)
        return false;
    s->had_transfer_error = true;
    if (!s->invariant_failed) {
        s->invariant_failed = true;
        fprintf(stderr, "mira: scheduler invariant failed: %s\n", message ? message : "unknown");
    }
    return false;
}

static bool scheduler_require(Scheduler* s, bool condition, const char* message) {
    if (condition)
        return true;
    return scheduler_record_invariant_failure(s, message);
}

static bool should_retry_http_status(const Scheduler* s, int status) {
    if (!s || !s->cfg->download.retry_on_http_error)
        return false;
    return bx_fetch_http_status_list_contains(s->cfg->download.retry_on_http_error, status);
}

static bool should_retry_result(const Scheduler* s, int status, int result) {
    switch ((MiraError)result) {
        case BX_FETCH_OK:
        case BX_FETCH_ERROR_CANCELLED:
        case BX_FETCH_ERROR_UNSUPPORTED:
        case BX_FETCH_ERROR_RESOURCE_LIMIT:
            return false;
        case BX_FETCH_ERROR_HTTP:
            return should_retry_http_status(s, status);
        default:
            return result != 0;
    }
}

static bool result_counts_as_scheduler_failure(int result) {
    switch ((MiraError)result) {
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

static long long timespec_diff_ns(const struct timespec* a, const struct timespec* b) {
    long long sec = (long long)a->tv_sec - (long long)b->tv_sec;
    long long nsec = (long long)a->tv_nsec - (long long)b->tv_nsec;
    return sec * 1000000000LL + nsec;
}

static struct timespec timespec_from_ns(long long ns) {
    struct timespec ts = {0};
    if (ns <= 0)
        return ts;

    ts.tv_sec = (time_t)(ns / 1000000000LL);
    ts.tv_nsec = (long)(ns % 1000000000LL);
    return ts;
}

static struct timespec timespec_add_ns(struct timespec base, long long ns) {
    if (ns <= 0)
        return base;

    base.tv_sec += (time_t)(ns / 1000000000LL);
    base.tv_nsec += (long)(ns % 1000000000LL);
    if (base.tv_nsec >= 1000000000L) {
        base.tv_sec++;
        base.tv_nsec -= 1000000000L;
    }
    return base;
}

static uint64_t scheduler_random_mix(uint64_t value) {
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdULL;
    value ^= value >> 33;
    value *= 0xc4ceb9fe1a85ec53ULL;
    value ^= value >> 33;
    return value;
}

static uint64_t scheduler_random_next(Scheduler* s) {
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

static uint64_t scheduler_random_bounded(Scheduler* s, uint64_t upper_bound) {
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

static uint64_t scheduler_initial_random_state(const EffectiveConfig* cfg) {
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

static long long scheduler_sample_host_wait_ns(Scheduler* s) {
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

static bool wait_remaining_for_host(const HostState* hs, const struct timespec* now, struct timespec* remaining) {
    if (!hs || !hs->has_next_request_time)
        return false;

    long long remaining_ns = timespec_diff_ns(&hs->next_request_time, now);
    if (remaining_ns <= 0)
        return false;

    if (remaining) {
        *remaining = timespec_from_ns(remaining_ns);
    }

    return true;
}

static bool wait_remaining_for_retry(const QueuedURL* q, const struct timespec* now, struct timespec* remaining) {
    if (!q || !q->has_retry_ready_time)
        return false;

    long long remaining_ns = timespec_diff_ns(&q->retry_ready_time, now);
    if (remaining_ns <= 0)
        return false;

    if (remaining) {
        remaining->tv_sec = (time_t)(remaining_ns / 1000000000LL);
        remaining->tv_nsec = (long)(remaining_ns % 1000000000LL);
    }

    return true;
}

static bool queue_wait_remaining(const Scheduler* s, const QueuedURL* q, const struct timespec* now, struct timespec* remaining) {
    struct timespec host_remaining = {0};
    struct timespec retry_remaining = {0};

    HostState* hs = get_host_state(s, q ? q->host : NULL);
    bool host_wait = wait_remaining_for_host(hs, now, &host_remaining);
    bool retry_wait = wait_remaining_for_retry(q, now, &retry_remaining);
    if (!host_wait && !retry_wait)
        return false;

    if (remaining) {
        if (!host_wait) {
            *remaining = retry_remaining;
        }
        else if (!retry_wait || timespec_diff_ns(&host_remaining, &retry_remaining) >= 0) {
            *remaining = host_remaining;
        }
        else {
            *remaining = retry_remaining;
        }
    }

    return true;
}

static bool scheduler_next_wait_duration(Scheduler* s, const struct timespec* now, struct timespec* min_wait) {
    bool found = false;

    for (QueuedURL* q = s->queue_head; q; q = q->next) {
        HostState* hs = get_host_state(s, q->host);
        if (hs && hs->count >= s->max_concurrent_per_host)
            continue;

        struct timespec remaining = {0};
        if (!queue_wait_remaining(s, q, now, &remaining))
            continue;

        if (!found || timespec_diff_ns(&remaining, min_wait) < 0) {
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
    free(q->url);
    free(q->output_path);
    free(q->host);
    free(q);
}

static int scheduler_add_url_with_tries(Scheduler* s, const char* url, const char* output_path, int tries_done, const struct timespec* retry_ready_time) {
    if (!s || !url || !output_path || tries_done < 0) {
        errno = EINVAL;
        return -1;
    }
    if (retry_ready_time && tries_done == 0) {
        errno = EINVAL;
        return -1;
    }
    if (s->cfg && s->cfg->download.tries > 0 && tries_done > s->cfg->download.tries) {
        errno = EINVAL;
        return -1;
    }

    QueuedURL* q = calloc(1, sizeof(QueuedURL));
    if (!q)
        return -1;

    q->url = strdup(url);
    q->output_path = strdup(output_path);
    if (!q->url || !q->output_path) {
        free_queued_url(q);
        return -1;
    }
    q->tries_done = tries_done;
    if (retry_ready_time) {
        q->retry_ready_time = *retry_ready_time;
        q->has_retry_ready_time = true;
    }

    MiraURL* mu = bx_fetch_url_parse(url);
    if (!mu || !mu->host || mu->host[0] == '\0') {
        bx_fetch_url_free(mu);
        free_queued_url(q);
        errno = EINVAL;
        return -1;
    }
    q->host = strdup(mu->host);
    bx_fetch_url_free(mu);
    if (!q->host) {
        free_queued_url(q);
        return -1;
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

static HostState* get_host_state(const Scheduler* s, const char* host) {
    if (!host)
        return NULL;

    HostState* hs = s->host_states;
    while (hs) {
        if (strcmp(hs->host, host) == 0)
            return hs;
        hs = hs->next;
    }

    return NULL;
}

static bool host_state_note_dispatch(Scheduler* s, HostState* hs, const struct timespec* dispatched_at) {
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
    if (wait_ns <= 0) {
        hs->has_next_request_time = false;
        hs->next_request_time = effective_dispatch_time;
        return true;
    }

    hs->next_request_time = timespec_add_ns(effective_dispatch_time, wait_ns);
    hs->has_next_request_time = true;
    return true;
}

static bool inc_host_active_count(Scheduler* s, const char* host, const struct timespec* dispatched_at) {
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
    hs->next = s->host_states;
    s->host_states = hs;
    s->active_host_total++;
    return true;
}

static bool dec_host_active_count(Scheduler* s, const char* host) {
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

static bool on_transfer_complete(void* userdata, const char* url, int status, int result, bool retryable_hint) {
    TransferInfo* ti = userdata;
    Scheduler* s = ti ? ti->sched : NULL;
    if (!ti || !s)
        return false;

    bool retried = false;
    bool completion_ok = scheduler_require(s, s->active_total > 0, "transfer completion observed with no active transfer");
    if (completion_ok) {
        s->active_total--;
        completion_ok = scheduler_require(s, dec_host_active_count(s, ti->host), "host active-count underflow on transfer completion");
    }
    if (completion_ok) {
        completion_ok = scheduler_require(s, scheduler_counts_invariant_holds(s), "active counters diverged after transfer completion");
    }

    if (!completion_ok) {
        s->had_transfer_error = true;
    }

    if (completion_ok && retryable_hint && should_retry_result(s, status, result) && ti->tries_done < s->cfg->download.tries) {
        if (!scheduler_require(s, ti->tries_done > 0, "retry candidate has invalid tries_done counter")) {
            s->had_transfer_error = true;
        }
        if (bx_fetch_output_is_verbose(s->cfg)) {
            char* display_url = bx_fetch_url_display_safe(url);
            fprintf(stderr, "  Retrying %s (try %d/%d)\n", display_url ? display_url : BX_FETCH_URL_DISPLAY_REDACTED, ti->tries_done + 1, s->cfg->download.tries);
            free(display_url);
        }
        struct timespec retry_ready_time = {0};
        const struct timespec* retry_ready_time_ptr = NULL;
        int retry_delay = ti->tries_done;
        if (retry_delay > s->cfg->download.waitretry) {
            retry_delay = s->cfg->download.waitretry;
        }
        if (retry_delay > 0) {
            if (!scheduler_require(s, clock_gettime(CLOCK_MONOTONIC, &retry_ready_time) == 0, "clock_gettime failed while scheduling retry delay")) {
                s->had_transfer_error = true;
            }
            else {
                retry_ready_time.tv_sec += retry_delay;
                retry_ready_time_ptr = &retry_ready_time;
            }
        }
        if (!s->invariant_failed && scheduler_add_url_with_tries(s, ti->url, ti->output_path, ti->tries_done, retry_ready_time_ptr) == 0) {
            retried = true;
        }
        else {
            s->had_transfer_error = true;
        }
    }
    else if (completion_ok && result_counts_as_scheduler_failure(result)) {
        s->had_transfer_error = true;
    }

    free(ti->url);
    free(ti->output_path);
    free(ti->host);
    free(ti);
    return retried;
}

Scheduler* bx_fetch_scheduler_new(const EffectiveConfig* cfg, SchedulerDispatchFn dispatch, SchedulerPollFn poll, void* userdata) {
    if (!cfg || !dispatch)
        return NULL;

    Scheduler* s = calloc(1, sizeof(Scheduler));
    if (!s)
        return NULL;

    s->cfg = cfg;
    s->dispatch = dispatch;
    s->poll = poll;
    s->userdata = userdata;

    s->max_concurrent_global = cfg->download.max_threads > 0 ? cfg->download.max_threads : 1;
    s->max_concurrent_per_host = 2;
    s->random_state = scheduler_initial_random_state(cfg);

    return s;
}

void bx_fetch_scheduler_free(Scheduler* s) {
    if (!s)
        return;

    QueuedURL* q = s->queue_head;
    while (q) {
        QueuedURL* next = q->next;
        free_queued_url(q);
        q = next;
    }

    HostState* hs = s->host_states;
    while (hs) {
        HostState* next = hs->next;
        free(hs->host);
        free(hs);
        hs = next;
    }

    free(s);
}

int bx_fetch_scheduler_add_url(Scheduler* s, const char* url, const char* output_path) {
    if (!s || !url || !output_path)
        return -1;
    char* canonical = bx_fetch_url_canonicalize(url);
    if (!canonical)
        return -1;
    int rc = bx_fetch_scheduler_add_canonical_url(s, canonical, output_path);
    free(canonical);
    return rc;
}

int bx_fetch_scheduler_add_canonical_url(Scheduler* s, const char* canonical_url, const char* output_path) {
    if (!s || !canonical_url || !output_path)
        return -1;
    return scheduler_add_url_with_tries(s, canonical_url, output_path, 0, NULL);
}

int bx_fetch_scheduler_run(Scheduler* s) {
    if (!s)
        return -1;
    bool had_dispatch_error = false;
    s->had_transfer_error = false;
    s->invariant_failed = false;

    while (s->queue_head || s->active_total > 0) {
        if (!scheduler_require(s, scheduler_counts_invariant_holds(s), "active counters diverged at scheduler loop entry")) {
            return -1;
        }

        QueuedURL* q = s->queue_head;
        QueuedURL* prev = NULL;
        bool started_any = false;

        struct timespec now;
        if (!scheduler_require(s, clock_gettime(CLOCK_MONOTONIC, &now) == 0, "clock_gettime failed while evaluating scheduler timing")) {
            return -1;
        }

        while (q && s->active_total < s->max_concurrent_global) {
            HostState* hs = get_host_state(s, q->host);
            int host_active = hs ? hs->count : 0;
            bool wait_ok = !queue_wait_remaining(s, q, &now, NULL);

            if (host_active < s->max_concurrent_per_host && wait_ok) {
                if (!scheduler_require(s, q->tries_done >= 0, "queued transfer has negative tries_done")) {
                    return -1;
                }
                if (s->cfg->download.tries > 0 && !scheduler_require(s, q->tries_done < s->cfg->download.tries, "queued transfer exceeded retry budget before dispatch")) {
                    return -1;
                }
                if (q->has_retry_ready_time && !scheduler_require(s, !wait_remaining_for_retry(q, &now, NULL), "retry dispatched before retry-ready time elapsed")) {
                    return -1;
                }

                TransferInfo* ti = malloc(sizeof(TransferInfo));
                if (!ti)
                    return -1;

                ti->sched = s;
                ti->url = strdup(q->url);
                ti->output_path = strdup(q->output_path);
                ti->host = q->host ? strdup(q->host) : NULL;
                ti->tries_done = q->tries_done + 1;

                if (!ti->url || !ti->output_path || (q->host && !ti->host)) {
                    free(ti->url);
                    free(ti->output_path);
                    free(ti->host);
                    free(ti);
                    return -1;
                }

                int dispatch_rc = s->dispatch(s->userdata, q->url, q->output_path, on_transfer_complete, ti);
                if (dispatch_rc == 0) {
                    s->active_total++;
                    bool host_count_ok = inc_host_active_count(s, q->host, &now);

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
                    if (!host_count_ok) {
                        scheduler_record_invariant_failure(s, "failed to update per-host active counters after dispatch");
                        return -1;
                    }
                    if (!scheduler_require(s, scheduler_counts_invariant_holds(s), "active counters diverged after dispatch")) {
                        return -1;
                    }
                    continue;
                }

                free(ti->url);
                free(ti->output_path);
                free(ti->host);
                free(ti);

                if (dispatch_rc > 0) {
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
                    continue;
                }

                had_dispatch_error = true;
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
                continue;
            }

            prev = q;
            q = q->next;
        }

        if (s->active_total > 0) {
            if (!scheduler_require(s, scheduler_counts_invariant_holds(s), "active counters diverged before scheduler poll")) {
                return -1;
            }
            if (!s->poll || s->poll(s->userdata) != 0)
                return -1;
        }
        else if (!started_any && s->queue_head) {
            struct timespec wait_duration = {0};
            if (scheduler_next_wait_duration(s, &now, &wait_duration)) {
                if (scheduler_sleep(&wait_duration) != 0)
                    return -1;
                continue;
            }
            break;
        }
    }

    if (!scheduler_require(s, scheduler_counts_invariant_holds(s), "active counters diverged at scheduler loop exit")) {
        return -1;
    }

    return (had_dispatch_error || s->had_transfer_error || s->invariant_failed) ? -1 : 0;
}
