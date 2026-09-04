#define _GNU_SOURCE
#include "lib/fetch/crawl_coordinator.h"
#include <errno.h>
#include <limits.h>
#include <stdlib.h>

struct BxFetchCrawlCoordinator {
    const struct bx_fetch_config* cfg;
    BxFetchFrontier* frontier;
    BxFetchFilter* filter;
    BxFetchScheduler* scheduler;
    BxFetchCrawlPlanOutputFn plan_output;
    BxFetchSchedulerDispatchFn dispatch;
    BxFetchSchedulerPollFn transport_poll;
    void* userdata;
    BxFetchCrawlPhase phase;
    int deferred_error;
};

static BxFetchCrawlEnqueueResult enqueue_result(BxFetchCrawlEnqueueStatus status, BxFetchFilterDecision decision) {
    return (BxFetchCrawlEnqueueResult){
        .status = status,
        .filter_decision = decision,
    };
}

static BxFetchCrawlEnqueueResult add_prepared(BxFetchCrawlCoordinator* coordinator, const BxFetchPreparedUrl* target, int depth, bool seed) {
    if (!coordinator || !target || depth < 0 || (coordinator->phase != BX_FETCH_CRAWL_PHASE_COLLECTING && coordinator->phase != BX_FETCH_CRAWL_PHASE_RUNNING)) {
        errno = coordinator && coordinator->phase == BX_FETCH_CRAWL_PHASE_CANCELLED ? ECANCELED : EINVAL;
        return enqueue_result(BX_FETCH_CRAWL_ERROR, FILTER_DECISION_ACCEPT);
    }

    const char* canonical_url = bx_fetch_prepared_url_transport(target);
    if (bx_fetch_frontier_is_seen_prepared(coordinator->frontier, target))
        return enqueue_result(BX_FETCH_CRAWL_SKIPPED_DUPLICATE, FILTER_DECISION_ACCEPT);
    if (seed && bx_fetch_filter_add_canonical_seed_url(coordinator->filter, canonical_url) != 0)
        return enqueue_result(BX_FETCH_CRAWL_ERROR, FILTER_DECISION_ACCEPT);

    BxFetchFilterDecision decision = bx_fetch_filter_evaluate_canonical_url(coordinator->filter, canonical_url);
    if (decision != FILTER_DECISION_ACCEPT)
        return enqueue_result(BX_FETCH_CRAWL_REJECTED, decision);

    BxFetchCrawlItem* item = bx_fetch_crawl_item_new_prepared(target, depth);
    if (!item || bx_fetch_frontier_add(coordinator->frontier, item) != 0)
        return enqueue_result(BX_FETCH_CRAWL_ERROR, FILTER_DECISION_ACCEPT);
    return enqueue_result(BX_FETCH_CRAWL_ENQUEUED, FILTER_DECISION_ACCEPT);
}

static int
coordinator_dispatch(void* userdata, const BxFetchPreparedUrl* target, const char* output_path, int depth, int attempt, int max_attempts, BxFetchSchedulerTransferDoneFn on_done, void* done_userdata) {
    BxFetchCrawlCoordinator* coordinator = userdata;
    return coordinator->dispatch(coordinator->userdata, target, output_path, depth, attempt, max_attempts, on_done, done_userdata);
}

static int drain_frontier(BxFetchCrawlCoordinator* coordinator) {
    BxFetchCrawlItem* item = NULL;
    while ((item = bx_fetch_frontier_next(coordinator->frontier)) != NULL) {
        char* output_path = NULL;
        int plan_result = coordinator->plan_output(coordinator->userdata, item->target, item->depth, &output_path);
        if (plan_result == 0 && !output_path) {
            bx_fetch_crawl_item_free(item);
            errno = EINVAL;
            return -1;
        }
        if (plan_result < 0) {
            int error_number = errno;
            free(output_path);
            bx_fetch_crawl_item_free(item);
            errno = error_number;
            return -1;
        }
        if (plan_result > 0) {
            free(output_path);
            bx_fetch_crawl_item_free(item);
            continue;
        }

        int schedule_result = bx_fetch_scheduler_add_prepared_url(coordinator->scheduler, item->target, output_path, item->depth);
        int error_number = errno;
        free(output_path);
        bx_fetch_crawl_item_free(item);
        if (schedule_result != 0) {
            errno = error_number;
            return -1;
        }
    }
    return 0;
}

static int coordinator_poll(void* userdata) {
    BxFetchCrawlCoordinator* coordinator = userdata;
    if (!coordinator->deferred_error && drain_frontier(coordinator) != 0) {
        coordinator->deferred_error = errno ? errno : EIO;
        bx_fetch_scheduler_cancel(coordinator->scheduler);
    }
    if (!coordinator->transport_poll || coordinator->transport_poll(coordinator->userdata) != 0)
        return -1;
    if (!coordinator->deferred_error && drain_frontier(coordinator) != 0) {
        coordinator->deferred_error = errno ? errno : EIO;
        bx_fetch_scheduler_cancel(coordinator->scheduler);
    }
    return 0;
}

BxFetchCrawlCoordinator* bx_fetch_crawl_coordinator_new(const struct bx_fetch_config* cfg,
                                                        BxFetchCrawlPlanOutputFn plan_output,
                                                        BxFetchSchedulerDispatchFn dispatch,
                                                        BxFetchSchedulerPollFn transport_poll,
                                                        void* userdata,
                                                        const BxFetchSchedulerObserver* scheduler_observer) {
    if (!cfg || !plan_output || !dispatch || cfg->recursive.level < 0) {
        errno = EINVAL;
        return NULL;
    }

    BxFetchCrawlCoordinator* coordinator = calloc(1, sizeof(*coordinator));
    if (!coordinator)
        return NULL;
    coordinator->cfg = cfg;
    coordinator->plan_output = plan_output;
    coordinator->dispatch = dispatch;
    coordinator->transport_poll = transport_poll;
    coordinator->userdata = userdata;
    coordinator->phase = BX_FETCH_CRAWL_PHASE_COLLECTING;
    coordinator->frontier = bx_fetch_frontier_new();
    coordinator->filter = bx_fetch_filter_new(cfg);
    coordinator->scheduler = bx_fetch_scheduler_new(cfg, coordinator_dispatch, coordinator_poll, coordinator, scheduler_observer);
    if (!coordinator->frontier || !coordinator->filter || !coordinator->scheduler) {
        bx_fetch_crawl_coordinator_free(coordinator);
        return NULL;
    }
    return coordinator;
}

void bx_fetch_crawl_coordinator_free(BxFetchCrawlCoordinator* coordinator) {
    if (!coordinator)
        return;
    bx_fetch_scheduler_free(coordinator->scheduler);
    bx_fetch_filter_free(coordinator->filter);
    bx_fetch_frontier_free(coordinator->frontier);
    free(coordinator);
}

BxFetchCrawlEnqueueResult bx_fetch_crawl_coordinator_add_seed_observed(BxFetchCrawlCoordinator* coordinator, const char* url, BxFetchPreparedUrl** target_out) {
    if (target_out)
        *target_out = NULL;
    if (!coordinator || !url || coordinator->phase != BX_FETCH_CRAWL_PHASE_COLLECTING) {
        errno = coordinator && coordinator->phase == BX_FETCH_CRAWL_PHASE_CANCELLED ? ECANCELED : EINVAL;
        return enqueue_result(BX_FETCH_CRAWL_ERROR, FILTER_DECISION_ACCEPT);
    }
    BxFetchPreparedUrl* target = bx_fetch_url_prepare(url);
    if (!target)
        return enqueue_result(BX_FETCH_CRAWL_ERROR, FILTER_DECISION_ACCEPT);
    BxFetchCrawlEnqueueResult result = add_prepared(coordinator, target, 0, true);
    if (target_out)
        *target_out = target;
    else
        bx_fetch_prepared_url_free(target);
    return result;
}

BxFetchCrawlEnqueueResult bx_fetch_crawl_coordinator_add_seed(BxFetchCrawlCoordinator* coordinator, const char* url) {
    return bx_fetch_crawl_coordinator_add_seed_observed(coordinator, url, NULL);
}

BxFetchCrawlEnqueueResult bx_fetch_crawl_coordinator_add_discovered(BxFetchCrawlCoordinator* coordinator,
                                                                    const BxFetchPreparedUrl* base,
                                                                    const char* reference,
                                                                    BxFetchHtmlLinkKind kind,
                                                                    int parent_depth) {
    if (!coordinator || !base || !reference || parent_depth < 0 || parent_depth == INT_MAX) {
        errno = EINVAL;
        return enqueue_result(BX_FETCH_CRAWL_ERROR, FILTER_DECISION_ACCEPT);
    }
    if (coordinator->phase != BX_FETCH_CRAWL_PHASE_COLLECTING && coordinator->phase != BX_FETCH_CRAWL_PHASE_RUNNING) {
        errno = coordinator->phase == BX_FETCH_CRAWL_PHASE_CANCELLED ? ECANCELED : EINVAL;
        return enqueue_result(BX_FETCH_CRAWL_ERROR, FILTER_DECISION_ACCEPT);
    }
    if ((kind == BX_FETCH_HTML_LINK_NAVIGATION && !coordinator->cfg->recursive.recursive) || (kind == BX_FETCH_HTML_LINK_REQUISITE && !coordinator->cfg->recursive.page_requisites)) {
        return enqueue_result(BX_FETCH_CRAWL_SKIPPED_KIND, FILTER_DECISION_ACCEPT);
    }
    if (coordinator->cfg->recursive.level != 0 && parent_depth >= coordinator->cfg->recursive.level)
        return enqueue_result(BX_FETCH_CRAWL_SKIPPED_DEPTH, FILTER_DECISION_ACCEPT);
    if (bx_fetch_url_has_explicit_scheme(reference) && bx_fetch_protocol_policy_evaluate_url(reference, coordinator->cfg->https.https_only) == BX_FETCH_PROTOCOL_DECISION_UNSUPPORTED) {
        return enqueue_result(BX_FETCH_CRAWL_REJECTED, FILTER_DECISION_UNSUPPORTED_PROTOCOL);
    }

    BxFetchPreparedUrl* target = bx_fetch_prepared_url_resolve(base, reference);
    if (!target) {
        if (errno == EPROTONOSUPPORT)
            return enqueue_result(BX_FETCH_CRAWL_REJECTED, FILTER_DECISION_UNSUPPORTED_PROTOCOL);
        return enqueue_result(BX_FETCH_CRAWL_ERROR, FILTER_DECISION_ACCEPT);
    }
    BxFetchCrawlEnqueueResult result = add_prepared(coordinator, target, parent_depth + 1, false);
    bx_fetch_prepared_url_free(target);
    return result;
}

int bx_fetch_crawl_coordinator_evaluate_target(const BxFetchCrawlCoordinator* coordinator, const BxFetchPreparedUrl* target, BxFetchFilterDecision* decision_out) {
    if (!coordinator || !target || !decision_out || (coordinator->phase != BX_FETCH_CRAWL_PHASE_COLLECTING && coordinator->phase != BX_FETCH_CRAWL_PHASE_RUNNING)) {
        errno = coordinator && coordinator->phase == BX_FETCH_CRAWL_PHASE_CANCELLED ? ECANCELED : EINVAL;
        return -1;
    }

    *decision_out = bx_fetch_filter_evaluate_canonical_url(coordinator->filter, bx_fetch_prepared_url_transport(target));
    return 0;
}

int bx_fetch_crawl_coordinator_run(BxFetchCrawlCoordinator* coordinator) {
    if (!coordinator || coordinator->phase != BX_FETCH_CRAWL_PHASE_COLLECTING) {
        errno = EINVAL;
        return -1;
    }
    coordinator->phase = BX_FETCH_CRAWL_PHASE_RUNNING;
    if (drain_frontier(coordinator) != 0 || bx_fetch_scheduler_run(coordinator->scheduler) != 0) {
        coordinator->phase = BX_FETCH_CRAWL_PHASE_FAILED;
        return -1;
    }
    if (coordinator->deferred_error) {
        int error_number = coordinator->deferred_error;
        coordinator->phase = BX_FETCH_CRAWL_PHASE_FAILED;
        errno = error_number;
        return -1;
    }
    if (coordinator->phase != BX_FETCH_CRAWL_PHASE_CANCELLED)
        coordinator->phase = BX_FETCH_CRAWL_PHASE_FINISHED;
    return 0;
}

void bx_fetch_crawl_coordinator_cancel(BxFetchCrawlCoordinator* coordinator) {
    if (!coordinator || coordinator->phase == BX_FETCH_CRAWL_PHASE_FINISHED || coordinator->phase == BX_FETCH_CRAWL_PHASE_FAILED || coordinator->phase == BX_FETCH_CRAWL_PHASE_CANCELLED) {
        return;
    }
    coordinator->phase = BX_FETCH_CRAWL_PHASE_CANCELLED;
    bx_fetch_scheduler_cancel(coordinator->scheduler);
    BxFetchCrawlItem* item = NULL;
    while ((item = bx_fetch_frontier_next(coordinator->frontier)) != NULL)
        bx_fetch_crawl_item_free(item);
}

BxFetchCrawlPhase bx_fetch_crawl_coordinator_phase(const BxFetchCrawlCoordinator* coordinator) {
    return coordinator ? coordinator->phase : BX_FETCH_CRAWL_PHASE_FAILED;
}
