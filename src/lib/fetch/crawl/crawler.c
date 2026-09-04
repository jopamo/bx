#define _GNU_SOURCE
#include "lib/fetch/crawler.h"
#include "lib/fetch/hashset.h"
#include "lib/fetch/resource_limits.h"
#include "lib/fetch/url.h"
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

BxFetchFrontier* bx_fetch_frontier_new(void) {
    BxFetchFrontier* f = calloc(1, sizeof(BxFetchFrontier));
    if (f) {
        f->seen_urls = bx_fetch_hashset_new(1024);  // Initial size
        if (!f->seen_urls) {
            free(f);
            return NULL;
        }
    }
    return f;
}

void bx_fetch_frontier_free(BxFetchFrontier* f) {
    if (!f)
        return;
    BxFetchFrontierNode* n = f->head;
    while (n) {
        BxFetchFrontierNode* next = n->next;
        bx_fetch_crawl_item_free(n->item);
        free(n);
        n = next;
    }
    bx_fetch_hashset_free(f->seen_urls);
    free(f);
}

int bx_fetch_frontier_add(BxFetchFrontier* f, BxFetchCrawlItem* item) {
    if (!f || !item || !item->url) {
        bx_fetch_crawl_item_free(item);
        return -1;
    }

    size_t ignored = 0;
    if (!bx_fetch_resource_bounded_strlen(item->url, BX_FETCH_URL_MAX_BYTES, &ignored) || (item->referrer && !bx_fetch_resource_bounded_strlen(item->referrer, BX_FETCH_URL_MAX_BYTES, &ignored))) {
        errno = EFBIG;
        bx_fetch_crawl_item_free(item);
        return -1;
    }

    char* canonical = bx_fetch_url_canonicalize(item->url);
    if (!canonical) {
        bx_fetch_crawl_item_free(item);
        return -1;
    }
    free(item->url);
    item->url = canonical;
    return bx_fetch_frontier_add_canonical(f, item);
}

int bx_fetch_frontier_add_canonical(BxFetchFrontier* f, BxFetchCrawlItem* item) {
    if (!f || !item || !item->url) {
        bx_fetch_crawl_item_free(item);
        return -1;
    }

    size_t url_len = 0;
    size_t referrer_len = 0;
    if (!bx_fetch_resource_bounded_strlen(item->url, BX_FETCH_URL_MAX_BYTES, &url_len) ||
        (item->referrer && !bx_fetch_resource_bounded_strlen(item->referrer, BX_FETCH_URL_MAX_BYTES, &referrer_len))) {
        errno = EFBIG;
        bx_fetch_crawl_item_free(item);
        return -1;
    }

    if (bx_fetch_hashset_contains(f->seen_urls, item->url)) {
        bx_fetch_crawl_item_free(item);
        return 0;  // Already seen, not an error
    }

    /*
     * A newly seen URL is copied into the permanent dedupe set and retained
     * once more while queued. The optional referrer is queue-owned only.
     */
    if (url_len > SIZE_MAX - url_len || referrer_len > SIZE_MAX - (url_len * 2u)) {
        errno = EFBIG;
        bx_fetch_crawl_item_free(item);
        return -1;
    }
    size_t reservation_bytes = url_len * 2u + referrer_len;
    if (!bx_fetch_resource_can_reserve(f->seen_count, f->retained_url_bytes, 1u, reservation_bytes, BX_FETCH_URL_STATE_MAX_ENTRIES, BX_FETCH_URL_STATE_MAX_BYTES)) {
        errno = EFBIG;
        bx_fetch_crawl_item_free(item);
        return -1;
    }

    BxFetchFrontierNode* n = calloc(1, sizeof(BxFetchFrontierNode));
    if (!n) {
        bx_fetch_crawl_item_free(item);
        return -1;
    }

    if (!bx_fetch_hashset_add(f->seen_urls, item->url)) {
        free(n);
        bx_fetch_crawl_item_free(item);
        errno = ENOMEM;
        return -1;
    }

    n->item = item;
    n->accounted_bytes = url_len + referrer_len;
    if (f->tail) {
        f->tail->next = n;
    }
    else {
        f->head = n;
    }
    f->tail = n;
    f->count++;
    f->seen_count++;
    f->retained_url_bytes += reservation_bytes;
    return 0;
}

bool bx_fetch_frontier_is_seen(BxFetchFrontier* f, const char* url) {
    if (!f || !url) {
        errno = EINVAL;
        return false;
    }
    size_t ignored = 0;
    if (!bx_fetch_resource_bounded_strlen(url, BX_FETCH_URL_MAX_BYTES, &ignored)) {
        errno = EFBIG;
        return false;
    }
    char* canonical = bx_fetch_url_canonicalize(url);
    bool seen = canonical ? bx_fetch_frontier_is_seen_canonical(f, canonical) : false;
    free(canonical);
    return seen;
}

bool bx_fetch_frontier_is_seen_canonical(BxFetchFrontier* f, const char* canonical_url) {
    if (!f || !canonical_url)
        return false;
    return bx_fetch_hashset_contains(f->seen_urls, canonical_url);
}

BxFetchCrawlItem* bx_fetch_frontier_next(BxFetchFrontier* f) {
    if (!f || !f->head)
        return NULL;
    BxFetchFrontierNode* n = f->head;
    BxFetchCrawlItem* item = n->item;
    f->head = n->next;
    if (!f->head)
        f->tail = NULL;
    if (f->retained_url_bytes >= n->accounted_bytes) {
        f->retained_url_bytes -= n->accounted_bytes;
    }
    else {
        f->retained_url_bytes = 0;
    }
    free(n);
    f->count--;
    return item;
}

BxFetchCrawlItem* bx_fetch_crawl_item_new(const char* url, int depth, const char* referrer) {
    size_t ignored = 0;
    if ((url && !bx_fetch_resource_bounded_strlen(url, BX_FETCH_URL_MAX_BYTES, &ignored)) || (referrer && !bx_fetch_resource_bounded_strlen(referrer, BX_FETCH_URL_MAX_BYTES, &ignored))) {
        errno = EFBIG;
        return NULL;
    }

    BxFetchCrawlItem* item = calloc(1, sizeof(BxFetchCrawlItem));
    if (!item)
        return NULL;

    if (url)
        item->url = strdup(url);
    item->depth = depth;
    if (referrer)
        item->referrer = strdup(referrer);

    if ((url && !item->url) || (referrer && !item->referrer)) {
        bx_fetch_crawl_item_free(item);
        return NULL;
    }

    return item;
}

void bx_fetch_crawl_item_free(BxFetchCrawlItem* item) {
    if (!item)
        return;
    free(item->url);
    free(item->referrer);
    free(item);
}
