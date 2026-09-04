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
 * - CrawlItem instances are heap-owned and freed with bx_fetch_crawl_item_free().
 * - bx_fetch_frontier_add() always consumes the passed CrawlItem (on success, duplicate,
 *   or error); callers must not reuse it after the call.
 * - `*_canonical` entry points avoid reparsing URLs already canonicalized by
 *   core at a trust boundary.
 * - bx_fetch_frontier_next() transfers ownership of the returned CrawlItem to the caller.
 */

#include <stdbool.h>
#include <stddef.h>
#include "hashset.h"
#include "resource_limits.h"

typedef struct {
    char* url;
    int depth;
    char* referrer;
} CrawlItem;

CrawlItem* bx_fetch_crawl_item_new(const char* url, int depth, const char* referrer);
void bx_fetch_crawl_item_free(CrawlItem* item);

typedef struct FrontierNode {
    CrawlItem* item;
    size_t accounted_bytes;
    struct FrontierNode* next;
} FrontierNode;

typedef struct Frontier {
    FrontierNode* head;
    FrontierNode* tail;
    int count;
    size_t seen_count;
    size_t retained_url_bytes;
    HashSet* seen_urls;
} Frontier;

Frontier* bx_fetch_frontier_new(void);
void bx_fetch_frontier_free(Frontier* f);
/*
 * Consumes `item` in all return paths; returns 0 for inserted or duplicate URL.
 * Returns -1 with errno EFBIG when the shared URL-state contract is exhausted.
 */
int bx_fetch_frontier_add(Frontier* f, CrawlItem* item);
/* Internal fast path: `item->url` must already be a canonical request URL. */
int bx_fetch_frontier_add_canonical(Frontier* f, CrawlItem* item);
bool bx_fetch_frontier_is_seen(Frontier* f, const char* url);
bool bx_fetch_frontier_is_seen_canonical(Frontier* f, const char* canonical_url);
/* Returns next owned item, or NULL if frontier is empty. */
CrawlItem* bx_fetch_frontier_next(Frontier* f);

#endif  // BX_FETCH_CRAWLER_H
