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

static bool add_bounded_length(size_t* total, const char* value) {
    size_t length = 0;
    if (!total || !bx_fetch_resource_bounded_strlen(value, BX_FETCH_URL_MAX_BYTES, &length) || length > SIZE_MAX - *total) {
        errno = EFBIG;
        return false;
    }
    *total += length;
    return true;
}

static bool prepared_retained_bytes(const BxFetchPreparedUrl* target, size_t* bytes_out) {
    if (!target || !bytes_out) {
        errno = EINVAL;
        return false;
    }
    size_t bytes = 0;
    if (!add_bounded_length(&bytes, bx_fetch_prepared_url_transport(target)) || !add_bounded_length(&bytes, bx_fetch_prepared_url_display(target)) ||
        !add_bounded_length(&bytes, bx_fetch_prepared_url_scheme(target)) || !add_bounded_length(&bytes, bx_fetch_prepared_url_host(target))) {
        return false;
    }
    *bytes_out = bytes;
    return true;
}

int bx_fetch_frontier_add(BxFetchFrontier* f, BxFetchCrawlItem* item) {
    if (!f || !item || !item->target || item->depth < 0) {
        bx_fetch_crawl_item_free(item);
        errno = EINVAL;
        return -1;
    }

    const char* canonical_url = bx_fetch_prepared_url_transport(item->target);
    size_t url_length = 0;
    size_t item_bytes = 0;
    if (!bx_fetch_resource_bounded_strlen(canonical_url, BX_FETCH_URL_MAX_BYTES, &url_length) || !prepared_retained_bytes(item->target, &item_bytes))
        goto fail;

    if (bx_fetch_hashset_contains(f->seen_urls, canonical_url)) {
        bx_fetch_crawl_item_free(item);
        return 0;
    }

    if (url_length > SIZE_MAX - item_bytes) {
        errno = EFBIG;
        goto fail;
    }
    size_t reservation_bytes = url_length + item_bytes;
    if (!bx_fetch_resource_can_reserve(f->seen_count, f->retained_url_bytes, 1u, reservation_bytes, BX_FETCH_URL_STATE_MAX_ENTRIES, BX_FETCH_URL_STATE_MAX_BYTES)) {
        errno = EFBIG;
        goto fail;
    }

    BxFetchFrontierNode* node = calloc(1, sizeof(*node));
    if (!node)
        goto fail;
    if (!bx_fetch_hashset_add(f->seen_urls, canonical_url)) {
        free(node);
        errno = ENOMEM;
        goto fail;
    }

    node->item = item;
    node->accounted_bytes = item_bytes;
    if (f->tail)
        f->tail->next = node;
    else
        f->head = node;
    f->tail = node;
    f->count++;
    f->seen_count++;
    f->retained_url_bytes += reservation_bytes;
    return 0;

fail:
    bx_fetch_crawl_item_free(item);
    return -1;
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
    BxFetchPreparedUrl* prepared = bx_fetch_url_prepare(url);
    bool seen = prepared ? bx_fetch_frontier_is_seen_prepared(f, prepared) : false;
    bx_fetch_prepared_url_free(prepared);
    return seen;
}

bool bx_fetch_frontier_is_seen_prepared(BxFetchFrontier* f, const BxFetchPreparedUrl* target) {
    if (!f || !target)
        return false;
    return bx_fetch_hashset_contains(f->seen_urls, bx_fetch_prepared_url_transport(target));
}

BxFetchCrawlItem* bx_fetch_frontier_next(BxFetchFrontier* f) {
    if (!f || !f->head)
        return NULL;
    BxFetchFrontierNode* node = f->head;
    BxFetchCrawlItem* item = node->item;
    f->head = node->next;
    if (!f->head)
        f->tail = NULL;
    if (f->retained_url_bytes >= node->accounted_bytes)
        f->retained_url_bytes -= node->accounted_bytes;
    else
        f->retained_url_bytes = 0;
    free(node);
    f->count--;
    return item;
}

static BxFetchCrawlItem* crawl_item_from_owned_target(BxFetchPreparedUrl* target, int depth) {
    if (!target)
        return NULL;
    if (depth < 0) {
        bx_fetch_prepared_url_free(target);
        errno = EINVAL;
        return NULL;
    }
    BxFetchCrawlItem* item = calloc(1, sizeof(*item));
    if (!item) {
        bx_fetch_prepared_url_free(target);
        return NULL;
    }
    item->target = target;
    item->depth = depth;
    return item;
}

BxFetchCrawlItem* bx_fetch_crawl_item_new(const char* url, int depth) {
    if (!url || depth < 0) {
        errno = EINVAL;
        return NULL;
    }
    size_t ignored = 0;
    if (!bx_fetch_resource_bounded_strlen(url, BX_FETCH_URL_MAX_BYTES, &ignored)) {
        errno = EFBIG;
        return NULL;
    }
    BxFetchPreparedUrl* target = bx_fetch_url_prepare(url);
    return target ? crawl_item_from_owned_target(target, depth) : NULL;
}

BxFetchCrawlItem* bx_fetch_crawl_item_new_prepared(const BxFetchPreparedUrl* target, int depth) {
    if (!target || depth < 0) {
        errno = EINVAL;
        return NULL;
    }
    BxFetchPreparedUrl* clone = bx_fetch_prepared_url_clone(target);
    return clone ? crawl_item_from_owned_target(clone, depth) : NULL;
}

void bx_fetch_crawl_item_free(BxFetchCrawlItem* item) {
    if (!item)
        return;
    bx_fetch_prepared_url_free(item->target);
    free(item);
}
