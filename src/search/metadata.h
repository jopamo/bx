#ifndef BX_SEARCH_METADATA_H
#define BX_SEARCH_METADATA_H

#include <stdbool.h>
#include <sys/stat.h>
#include "fswalk/walk.h"

enum bx_walk_type_match_state {
    BX_WALK_TYPE_MATCH_NO = 0,
    BX_WALK_TYPE_MATCH_YES,
    BX_WALK_TYPE_MATCH_DEFER_METADATA,
};

bool bx_walk_numeric_match(unsigned long long actual, long long expected, int cmp);
bool bx_walk_mode_matches_perm(mode_t mode, mode_t bits, int kind);
bool bx_walk_size_matches(off_t size, long long expected, int cmp, unsigned long long unit);
bool bx_walk_type_filter_is_valid(char type_filter, bool allow_extended);
bool bx_walk_parse_named_type_filter(const char *text, char *type_filter);
bool bx_walk_parse_unsigned_id(const char *text, unsigned long long *value);
bool bx_walk_resolve_user(const char *text, uid_t *value);
bool bx_walk_resolve_group(const char *text, gid_t *value);
bool bx_walk_uid_has_passwd(uid_t uid);
bool bx_walk_gid_has_group(gid_t gid);
bool bx_walk_entry_is_empty(struct bx_walk_entry *entry);
enum bx_walk_type_match_state
bx_walk_entry_matches_type_without_metadata(struct bx_walk_entry *entry, char type_filter);
bool bx_walk_entry_matches_type(struct bx_walk_entry *entry, char type_filter);

#endif
