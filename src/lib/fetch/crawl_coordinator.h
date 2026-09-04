#ifndef BX_FETCH_CRAWL_COORDINATOR_H
#define BX_FETCH_CRAWL_COORDINATOR_H

/* BX_FETCH_HEADER_OWNER: core */
/* BX_FETCH_HEADER_CONSUMERS: core, applet */

/*
 * Owns the crawl frontier, filter, and scheduler as one run lifecycle.
 * Frontends provide output naming and transport callbacks; they do not drain
 * or deduplicate the frontier themselves.
 */

#include "crawler.h"
#include "filter.h"
#include "html.h"
#include "scheduler.h"

typedef struct BxFetchCrawlCoordinator BxFetchCrawlCoordinator;

typedef enum {
    BX_FETCH_CRAWL_PHASE_COLLECTING = 0,
    BX_FETCH_CRAWL_PHASE_RUNNING,
    BX_FETCH_CRAWL_PHASE_FINISHED,
    BX_FETCH_CRAWL_PHASE_CANCELLED,
    BX_FETCH_CRAWL_PHASE_FAILED,
} BxFetchCrawlPhase;

typedef enum {
    BX_FETCH_CRAWL_ENQUEUED = 0,
    BX_FETCH_CRAWL_SKIPPED_KIND,
    BX_FETCH_CRAWL_SKIPPED_DEPTH,
    BX_FETCH_CRAWL_SKIPPED_DUPLICATE,
    BX_FETCH_CRAWL_REJECTED,
    BX_FETCH_CRAWL_ERROR,
} BxFetchCrawlEnqueueStatus;

typedef struct {
    BxFetchCrawlEnqueueStatus status;
    BxFetchFilterDecision filter_decision;
} BxFetchCrawlEnqueueResult;

/*
 * Returns 0 with an owned non-NULL output path to schedule, >0 to skip, or
 * <0 on failure. The coordinator frees any returned path in every case.
 */
typedef int (*BxFetchCrawlPlanOutputFn)(void* userdata, const BxFetchPreparedUrl* target, int depth, char** output_path_out);

BxFetchCrawlCoordinator* bx_fetch_crawl_coordinator_new(const struct bx_fetch_config* cfg,
                                                        BxFetchCrawlPlanOutputFn plan_output,
                                                        BxFetchSchedulerDispatchFn dispatch,
                                                        BxFetchSchedulerPollFn transport_poll,
                                                        void* userdata,
                                                        const BxFetchSchedulerObserver* scheduler_observer);
void bx_fetch_crawl_coordinator_free(BxFetchCrawlCoordinator* coordinator);

BxFetchCrawlEnqueueResult bx_fetch_crawl_coordinator_add_seed(BxFetchCrawlCoordinator* coordinator, const char* url);
BxFetchCrawlEnqueueResult bx_fetch_crawl_coordinator_add_discovered(BxFetchCrawlCoordinator* coordinator,
                                                                    const BxFetchPreparedUrl* base,
                                                                    const char* reference,
                                                                    BxFetchHtmlLinkKind kind,
                                                                    int parent_depth);

int bx_fetch_crawl_coordinator_run(BxFetchCrawlCoordinator* coordinator);
void bx_fetch_crawl_coordinator_cancel(BxFetchCrawlCoordinator* coordinator);
BxFetchCrawlPhase bx_fetch_crawl_coordinator_phase(const BxFetchCrawlCoordinator* coordinator);

#endif  // BX_FETCH_CRAWL_COORDINATOR_H
