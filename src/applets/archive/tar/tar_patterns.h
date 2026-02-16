#ifndef BX_APPLETS_ARCHIVE_TAR_TAR_PATTERNS_H
#define BX_APPLETS_ARCHIVE_TAR_TAR_PATTERNS_H

#include <stdbool.h>
#include <stddef.h>

#include "applets/archive/archive_common.h"

struct bx_tar_match_policy {
    bool anchored;
    bool ignore_case;
    bool wildcards;
    bool wildcards_match_slash;
};

struct bx_tar_match_pattern {
    char* text;
    char* folded_text;
    size_t trimmed_len;
    bool wildcard_magic;
    bool has_slash;
    struct bx_tar_match_policy policy;
};

struct bx_tar_match_pattern_list {
    struct bx_tar_match_pattern* items;
    size_t len;
    size_t cap;
};

void bx_tar_match_policy_init_member_default(struct bx_tar_match_policy* policy);
void bx_tar_match_policy_init_exclude_default(struct bx_tar_match_policy* policy);

bool bx_tar_match_policy_set_anchored(struct bx_tar_match_policy* member_policy,
                                      struct bx_tar_match_policy* exclude_policy,
                                      bool enabled);
bool bx_tar_match_policy_set_ignore_case(struct bx_tar_match_policy* member_policy,
                                         struct bx_tar_match_policy* exclude_policy,
                                         bool enabled);
bool bx_tar_match_policy_set_wildcards(struct bx_tar_match_policy* member_policy,
                                       struct bx_tar_match_policy* exclude_policy,
                                       bool enabled);
bool bx_tar_match_policy_set_wildcards_match_slash(struct bx_tar_match_policy* member_policy,
                                                   struct bx_tar_match_policy* exclude_policy,
                                                   bool enabled);

void bx_tar_match_pattern_list_free(struct bx_tar_match_pattern_list* list);
bool bx_tar_match_pattern_list_append(struct bx_tar_match_pattern_list* list,
                                      const char* text,
                                      const struct bx_tar_match_policy* policy);

bool bx_tar_match_member_name(const char* pattern,
                              const struct bx_tar_match_policy* policy,
                              bool recurse,
                              const char* name);

bool bx_tar_match_exclude_pattern(const char* pattern,
                                  const struct bx_tar_match_policy* policy,
                                  const char* archive_path);

bool bx_tar_path_excluded(const struct bx_tar_match_pattern_list* patterns,
                          const char* archive_path);

#endif /* BX_APPLETS_ARCHIVE_TAR_TAR_PATTERNS_H */
