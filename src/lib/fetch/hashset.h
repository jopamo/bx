#ifndef BX_FETCH_HASHSET_H
#define BX_FETCH_HASHSET_H

/* BX_FETCH_HEADER_OWNER: util */
/* BX_FETCH_HEADER_CONSUMERS: util, core, crawl */

/*
 * Layering contract:
 * - Generic utility container used by crawl/core; does not depend on higher
 *   policy or network state.
 *
 * Ownership and lifetime:
 * - bx_fetch_hashset_add() copies keys internally; callers retain input ownership.
 * - bx_fetch_hashset_contains() borrows key pointers.
 * - bx_fetch_hashset_remove() releases a matching copied key.
 * - bx_fetch_hashset_free() releases all copied keys and internal storage.
 */

#include <stdbool.h>
#include <stddef.h>

typedef struct HashSet HashSet;

HashSet* bx_fetch_hashset_new(size_t size);
void bx_fetch_hashset_free(HashSet* hs);
bool bx_fetch_hashset_add(HashSet* hs, const char* key);
bool bx_fetch_hashset_contains(HashSet* hs, const char* key);
bool bx_fetch_hashset_remove(HashSet* hs, const char* key);

#endif  // BX_FETCH_HASHSET_H
