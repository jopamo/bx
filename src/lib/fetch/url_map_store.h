#ifndef BX_FETCH_URL_MAP_STORE_H
#define BX_FETCH_URL_MAP_STORE_H

/* BX_FETCH_HEADER_OWNER: store */
/* BX_FETCH_HEADER_CONSUMERS: store, core */

/*
 * Layering contract:
 * - Persistent URL->local-path map IO is owned by the store layer.
 * - Core consumes this via callback-driven load/save and does not parse file
 *   encodings directly.
 * - Saved URL keys are canonicalized with authority userinfo removed even if
 *   a caller supplies a credential-bearing request identity.
 *
 * Ownership and lifetime:
 * - bx_fetch_url_map_store_load()/save() borrow `cfg`.
 * - Entry arrays and callback userdata remain caller-owned.
 */

#include "lib/fetch/config.h"
#include <stddef.h>

typedef struct {
    const char* url;
    const char* local_path;
} BxFetchUrlMapEntry;

/*
 * Callback contract:
 * - `url` and `local_path` are borrowed and valid only during callback execution.
 * - Return 0 to continue loading; non-zero aborts load with failure.
 */
typedef int (*BxFetchUrlMapLoadFn)(void* userdata, const char* url, const char* local_path);

int bx_fetch_url_map_store_load(const struct bx_fetch_config* cfg, BxFetchUrlMapLoadFn cb, void* userdata);
int bx_fetch_url_map_store_save(const struct bx_fetch_config* cfg, const BxFetchUrlMapEntry* entries, size_t entry_count);

#endif  // BX_FETCH_URL_MAP_STORE_H
