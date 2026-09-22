#ifndef BX_FETCH_RATE_LIMIT_H
#define BX_FETCH_RATE_LIMIT_H

/* BX_FETCH_HEADER_OWNER: policy */
/* BX_FETCH_HEADER_CONSUMERS: policy, net */
#include <stdint.h>

/* Legacy provider reset headers are epoch seconds, paired with remaining=0.
 * Reject ambiguous or overflowing numbers rather than inventing a delay. */
int64_t bx_fetch_rate_limit_reset(const char* remaining, const char* reset, int64_t now);

#endif
