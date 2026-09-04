#ifndef BX_FETCH_FILTER_H
#define BX_FETCH_FILTER_H

/* BX_FETCH_HEADER_OWNER: policy */
/* BX_FETCH_HEADER_CONSUMERS: policy, core */

/*
 * Layering contract:
 * - URL policy decisions are centralized in the policy layer.
 * - Callers consume decision enums/reasons rather than re-implementing filter
 *   rules in core.
 * - bx_fetch_filter_evaluate_transport_canonical_url() applies only protocol and
 *   credential-boundary policy; it does not apply crawl scope/content rules.
 *
 * Ownership and lifetime:
 * - bx_fetch_filter_new() borrows `cfg`; the EffectiveConfig must outlive Filter.
 * - bx_fetch_filter_free() releases all filter-owned allocations.
 * - Input URL pointers are borrowed; no input pointer ownership transfer.
 * - `*_canonical_*` entry points are internal fast paths whose URL argument
 *   was already canonicalized at a trust boundary.
 */

#include "config.h"
#include <stdbool.h>

typedef struct Filter Filter;

typedef enum {
    FILTER_DECISION_ACCEPT = 0,
    FILTER_DECISION_INVALID_URL,
    FILTER_DECISION_UNSUPPORTED_PROTOCOL,
    FILTER_DECISION_HTTPS_ONLY,
    FILTER_DECISION_URL_CREDENTIALS,
    FILTER_DECISION_DOMAIN_DENYLIST,
    FILTER_DECISION_DOMAIN_SCOPE,
    FILTER_DECISION_DIRECTORY_DENYLIST,
    FILTER_DECISION_DIRECTORY_SCOPE,
    FILTER_DECISION_SUFFIX_DENYLIST,
    FILTER_DECISION_REGEX_ALLOWLIST,
    FILTER_DECISION_SUFFIX_ALLOWLIST,
} FilterDecision;

Filter* bx_fetch_filter_new(const EffectiveConfig* cfg);
void bx_fetch_filter_free(Filter* f);

int bx_fetch_filter_add_seed_url(Filter* f, const char* url);
int bx_fetch_filter_add_canonical_seed_url(Filter* f, const char* canonical_url);
FilterDecision bx_fetch_filter_evaluate_url(Filter* f, const char* url);
FilterDecision bx_fetch_filter_evaluate_transport_canonical_url(Filter* f, const char* canonical_url);
FilterDecision bx_fetch_filter_evaluate_canonical_url(Filter* f, const char* canonical_url);
const char* bx_fetch_filter_decision_reason(FilterDecision decision);
bool bx_fetch_filter_url_accepted(Filter* f, const char* url);

#endif  // BX_FETCH_FILTER_H
