#ifndef BX_FETCH_CLIENT_PROFILE_STORE_H
#define BX_FETCH_CLIENT_PROFILE_STORE_H
/* BX_FETCH_HEADER_OWNER: store */
/* BX_FETCH_HEADER_CONSUMERS: store, net */
#include "config.h"
#include "url.h"

typedef struct BxFetchProfileCache BxFetchProfileCache;
BxFetchProfileCache* bx_fetch_profile_cache_new(void);
void bx_fetch_profile_cache_free(BxFetchProfileCache* cache);
BxFetchClientProfile bx_fetch_profile_cache_get(BxFetchProfileCache* cache, const BxFetchPreparedUrl* target);
/* A preference is not authorization. Cache I/O failure leaves fetching usable. */
void bx_fetch_profile_cache_put(BxFetchProfileCache* cache, const BxFetchPreparedUrl* target, BxFetchClientProfile profile);
#endif
