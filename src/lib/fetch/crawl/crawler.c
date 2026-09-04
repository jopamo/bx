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

Frontier *frontier_new(void) {
    Frontier *f = calloc(1, sizeof(Frontier));
    if (f) {
        f->seen_urls = hashset_new(1024); // Initial size
        if (!f->seen_urls) {
            free(f);
            return NULL;
        }
    }
    return f;
}

void frontier_free(Frontier *f) {
    if (!f) return;
    FrontierNode *n = f->head;
    while (n) {
        FrontierNode *next = n->next;
        crawl_item_free(n->item);
        free(n);
        n = next;
    }
    hashset_free(f->seen_urls);
    free(f);
}

int frontier_add(Frontier *f, CrawlItem *item) {
    if (!f || !item || !item->url) {
        crawl_item_free(item);
        return -1;
    }

    size_t ignored = 0;
    if (!mira_resource_bounded_strlen(
            item->url, MIRA_URL_MAX_BYTES, &ignored) ||
        (item->referrer &&
         !mira_resource_bounded_strlen(
             item->referrer, MIRA_URL_MAX_BYTES, &ignored))) {
        errno = EFBIG;
        crawl_item_free(item);
        return -1;
    }

    char *canonical = mira_url_canonicalize(item->url);
    if (!canonical) {
        crawl_item_free(item);
        return -1;
    }
    free(item->url);
    item->url = canonical;
    return frontier_add_canonical(f, item);
}

int frontier_add_canonical(Frontier *f, CrawlItem *item) {
    if (!f || !item || !item->url) {
        crawl_item_free(item);
        return -1;
    }

    size_t url_len = 0;
    size_t referrer_len = 0;
    if (!mira_resource_bounded_strlen(
            item->url, MIRA_URL_MAX_BYTES, &url_len) ||
        (item->referrer &&
         !mira_resource_bounded_strlen(
             item->referrer, MIRA_URL_MAX_BYTES, &referrer_len))) {
        errno = EFBIG;
        crawl_item_free(item);
        return -1;
    }

    if (hashset_contains(f->seen_urls, item->url)) {
        crawl_item_free(item);
        return 0; // Already seen, not an error
    }

    /*
     * A newly seen URL is copied into the permanent dedupe set and retained
     * once more while queued. The optional referrer is queue-owned only.
     */
    if (url_len > SIZE_MAX - url_len ||
        referrer_len > SIZE_MAX - (url_len * 2u)) {
        errno = EFBIG;
        crawl_item_free(item);
        return -1;
    }
    size_t reservation_bytes = url_len * 2u + referrer_len;
    if (!mira_resource_can_reserve(
            f->seen_count, f->retained_url_bytes,
            1u, reservation_bytes,
            MIRA_URL_STATE_MAX_ENTRIES, MIRA_URL_STATE_MAX_BYTES)) {
        errno = EFBIG;
        crawl_item_free(item);
        return -1;
    }

    FrontierNode *n = calloc(1, sizeof(FrontierNode));
    if (!n) {
        crawl_item_free(item);
        return -1;
    }

    if (!hashset_add(f->seen_urls, item->url)) {
        free(n);
        crawl_item_free(item);
        errno = ENOMEM;
        return -1;
    }

    n->item = item;
    n->accounted_bytes = url_len + referrer_len;
    if (f->tail) {
        f->tail->next = n;
    } else {
        f->head = n;
    }
    f->tail = n;
    f->count++;
    f->seen_count++;
    f->retained_url_bytes += reservation_bytes;
    return 0;
}

bool frontier_is_seen(Frontier *f, const char *url) {
    if (!f || !url) {
        errno = EINVAL;
        return false;
    }
    size_t ignored = 0;
    if (!mira_resource_bounded_strlen(
            url, MIRA_URL_MAX_BYTES, &ignored)) {
        errno = EFBIG;
        return false;
    }
    char *canonical = mira_url_canonicalize(url);
    bool seen = canonical ? frontier_is_seen_canonical(f, canonical) : false;
    free(canonical);
    return seen;
}

bool frontier_is_seen_canonical(Frontier *f, const char *canonical_url) {
    if (!f || !canonical_url) return false;
    return hashset_contains(f->seen_urls, canonical_url);
}

CrawlItem *frontier_next(Frontier *f) {
    if (!f || !f->head) return NULL;
    FrontierNode *n = f->head;
    CrawlItem *item = n->item;
    f->head = n->next;
    if (!f->head) f->tail = NULL;
    if (f->retained_url_bytes >= n->accounted_bytes) {
        f->retained_url_bytes -= n->accounted_bytes;
    } else {
        f->retained_url_bytes = 0;
    }
    free(n);
    f->count--;
    return item;
}

CrawlItem *crawl_item_new(const char *url, int depth, const char *referrer) {
    size_t ignored = 0;
    if ((url &&
         !mira_resource_bounded_strlen(
             url, MIRA_URL_MAX_BYTES, &ignored)) ||
        (referrer &&
         !mira_resource_bounded_strlen(
             referrer, MIRA_URL_MAX_BYTES, &ignored))) {
        errno = EFBIG;
        return NULL;
    }

    CrawlItem *item = calloc(1, sizeof(CrawlItem));
    if (!item) return NULL;

    if (url) item->url = strdup(url);
    item->depth = depth;
    if (referrer) item->referrer = strdup(referrer);

    if ((url && !item->url) || (referrer && !item->referrer)) {
        crawl_item_free(item);
        return NULL;
    }

    return item;
}

void crawl_item_free(CrawlItem *item) {
    if (!item) return;
    free(item->url);
    free(item->referrer);
    free(item);
}
