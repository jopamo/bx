#ifndef MIRA_URL_MAP_STORE_H
#define MIRA_URL_MAP_STORE_H

/* MIRA_HEADER_OWNER: store */
/* MIRA_HEADER_CONSUMERS: store, core */

/*
 * Layering contract:
 * - Persistent URL->local-path map IO is owned by the store layer.
 * - Core consumes this via callback-driven load/save and does not parse file
 *   encodings directly.
 * - Saved URL keys are canonicalized with authority userinfo removed even if
 *   a caller supplies a credential-bearing request identity.
 *
 * Ownership and lifetime:
 * - url_map_store_load()/save() borrow `cfg`.
 * - Entry arrays and callback userdata remain caller-owned.
 */

#include "lib/fetch/config.h"
#include <stddef.h>

typedef struct {
    const char* url;
    const char* local_path;
} MiraUrlMapEntry;

/*
 * Callback contract:
 * - `url` and `local_path` are borrowed and valid only during callback execution.
 * - Return 0 to continue loading; non-zero aborts load with failure.
 */
typedef int (*MiraUrlMapLoadFn)(void* userdata, const char* url, const char* local_path);

int url_map_store_load(const EffectiveConfig* cfg, MiraUrlMapLoadFn cb, void* userdata);
int url_map_store_save(const EffectiveConfig* cfg, const MiraUrlMapEntry* entries, size_t entry_count);

#endif  // MIRA_URL_MAP_STORE_H
