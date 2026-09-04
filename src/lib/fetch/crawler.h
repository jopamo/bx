#ifndef BX_FETCH_CRAWLER_H
#define BX_FETCH_CRAWLER_H

/* BX_FETCH_HEADER_OWNER: crawl */
/* BX_FETCH_HEADER_CONSUMERS: crawl, core */

/*
 * Layering contract:
 * - Crawl frontier ownership stays in crawl/core orchestration code.
 * - Policy/fs/net layers consume URLs passed from core, not frontier internals.
 *
 * Ownership and lifetime:
 * - BxFetchCrawlItem instances are heap-owned and freed with bx_fetch_crawl_item_free().
 * - bx_fetch_frontier_add() always consumes the passed BxFetchCrawlItem (on success, duplicate,
 *   or error); callers must not reuse it after the call.
 * - Items own immutable prepared targets so dequeue/scheduling does not
 *   reparse canonical URL strings.
 * - bx_fetch_frontier_next() transfers ownership of the returned BxFetchCrawlItem to the caller.
 */

#include <stdbool.h>
#include <stddef.h>
#include "hashset.h"
#include "resource_limits.h"
#include "url.h"

typedef struct {
    BxFetchPreparedUrl* target;
    int depth;
} BxFetchCrawlItem;

BxFetchCrawlItem* bx_fetch_crawl_item_new(const char* url, int depth);
BxFetchCrawlItem* bx_fetch_crawl_item_new_prepared(const BxFetchPreparedUrl* target, int depth);
void bx_fetch_crawl_item_free(BxFetchCrawlItem* item);

typedef struct BxFetchFrontierNode {
    BxFetchCrawlItem* item;
    size_t accounted_bytes;
    struct BxFetchFrontierNode* next;
} BxFetchFrontierNode;

typedef struct BxFetchFrontier {
    BxFetchFrontierNode* head;
    BxFetchFrontierNode* tail;
    int count;
    size_t seen_count;
    size_t retained_url_bytes;
    BxFetchHashSet* seen_urls;
} BxFetchFrontier;

BxFetchFrontier* bx_fetch_frontier_new(void);
void bx_fetch_frontier_free(BxFetchFrontier* f);
/*
 * Consumes `item` in all return paths; returns 0 for inserted or duplicate URL.
 * Returns -1 with errno EFBIG when the shared URL-state contract is exhausted.
 */
int bx_fetch_frontier_add(BxFetchFrontier* f, BxFetchCrawlItem* item);
bool bx_fetch_frontier_is_seen(BxFetchFrontier* f, const char* url);
bool bx_fetch_frontier_is_seen_prepared(BxFetchFrontier* f, const BxFetchPreparedUrl* target);
/* Returns next owned item, or NULL if frontier is empty. */
BxFetchCrawlItem* bx_fetch_frontier_next(BxFetchFrontier* f);

#endif  // BX_FETCH_CRAWLER_H
