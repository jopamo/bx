#ifndef MIRA_HASHSET_H
#define MIRA_HASHSET_H

/* MIRA_HEADER_OWNER: util */
/* MIRA_HEADER_CONSUMERS: util, core, crawl */

/*
 * Layering contract:
 * - Generic utility container used by crawl/core; does not depend on higher
 *   policy or network state.
 *
 * Ownership and lifetime:
 * - hashset_add() copies keys internally; callers retain input ownership.
 * - hashset_contains() borrows key pointers.
 * - hashset_remove() releases a matching copied key.
 * - hashset_free() releases all copied keys and internal storage.
 */

#include <stdbool.h>
#include <stddef.h>

typedef struct HashSet HashSet;

HashSet *hashset_new(size_t size);
void hashset_free(HashSet *hs);
bool hashset_add(HashSet *hs, const char *key);
bool hashset_contains(HashSet *hs, const char *key);
bool hashset_remove(HashSet *hs, const char *key);

#endif // MIRA_HASHSET_H
