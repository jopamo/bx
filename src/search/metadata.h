#ifndef BX_SEARCH_METADATA_H
#define BX_SEARCH_METADATA_H

#include <stdbool.h>
#include <sys/stat.h>
#include "walk.h"

bool bx_walk_numeric_match(unsigned long long actual, long long expected, int cmp);
bool bx_walk_mode_matches_perm(mode_t mode, mode_t bits, int kind);
bool bx_walk_size_matches(off_t size, long long expected, int cmp, unsigned long long unit);
bool bx_walk_entry_is_empty(struct walk_entry *entry);
bool bx_walk_entry_matches_type(struct walk_entry *entry, char type_filter);

#endif
