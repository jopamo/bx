#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "applets/archive/tar/tar_create.h"
#include "applets/archive/tar/tar_files_from.h"
#include "applets/archive/tar/tar_patterns.h"
#include "bx/libbx.h"
#include "lib/path_ops.h"

struct bx_tar_files_from_state {
    char* cwd;
    unsigned char separator;
    bool verbatim;
    bool unquote;
    bool recurse;
    struct bx_tar_match_policy member_policy;
    struct bx_tar_match_policy exclude_policy;
    struct bx_tar_match_pattern_list exclude_patterns;
    struct bx_archive_name_list exclude_ignore_files;
    struct bx_archive_name_list exclude_ignore_recursive_files;
    struct bx_archive_name_list exclude_tag_files;
    struct bx_archive_name_list exclude_tag_all_files;
    struct bx_archive_name_list exclude_tag_under_files;
    bool exclude_caches;
    bool exclude_caches_all;
    bool exclude_caches_under;
    bool exclude_vcs;
    bool exclude_vcs_ignores;
};

struct bx_tar_create_dir_policy {
    char* source_path;
    char* archive_path;
    struct bx_tar_match_pattern_list local_patterns;
    struct bx_tar_match_pattern_list recursive_patterns;
    struct bx_archive_name_list keep_names;
    bool exclude_under;
    bool exclude_except_keep;
};

struct bx_tar_create_filter_state {
    const struct bx_tar_match_pattern_list* exclude_patterns;
    const struct bx_archive_name_list* exclude_ignore_files;
    const struct bx_archive_name_list* exclude_ignore_recursive_files;
    const struct bx_archive_name_list* exclude_tag_files;
    const struct bx_archive_name_list* exclude_tag_all_files;
    const struct bx_archive_name_list* exclude_tag_under_files;
    const struct bx_tar_match_policy* exclude_policy;
    struct bx_tar_create_dir_policy* dir_policies;
    size_t dir_policies_len;
    size_t dir_policies_cap;
    bool exclude_caches;
    bool exclude_caches_all;
    bool exclude_caches_under;
    bool exclude_vcs;
    bool exclude_vcs_ignores;
    struct bx_diag_ctx* diag;
    bool* had_create_errors;
};

struct bx_tar_create_collect_ctx {
    bx_archive_fs_visit_fn visit_fn;
    void* visit_user_data;
    bool sort_children;
    bool had_create_errors;
    struct bx_diag_ctx* diag;
};

static void bx_tar_create_directive_list_free(struct bx_tar_create_directive_list* list) {
    size_t i;

    for (i = 0u; i < list->len; i++) {
        free(list->items[i].text);
    }
    free(list->items);
    list->items = NULL;
    list->len = 0u;
    list->cap = 0u;
}

static bool bx_tar_create_directive_list_append(struct bx_tar_create_directive_list* list,
                                                enum bx_tar_create_directive_kind kind,
                                                const char* text) {
    struct bx_tar_create_directive* slot;

    if (list->len == list->cap) {
        size_t next_cap = list->cap ? list->cap * 2u : 8u;
        list->items = xrealloc(list->items, next_cap * sizeof(*list->items));
        list->cap = next_cap;
    }

    slot = &list->items[list->len++];
    slot->kind = kind;
    slot->text = text ? xstrdup(text) : NULL;
    return true;
}

bool bx_tar_create_options_has_inputs(const struct bx_tar_create_options* options) {
    size_t i;

    for (i = 0u; i < options->directives.len; i++) {
        enum bx_tar_create_directive_kind kind = options->directives.items[i].kind;
        if (kind == BX_TAR_CREATE_DIRECTIVE_ADD_PATH || kind == BX_TAR_CREATE_DIRECTIVE_FILES_FROM) {
            return true;
        }
    }

    return false;
}

void bx_tar_create_options_cleanup(struct bx_tar_create_options* options) {
    bx_tar_create_directive_list_free(&options->directives);
}

bool bx_tar_create_options_add_exclude_pattern(struct bx_tar_create_options* options,
                                               const char* pattern) {
    return bx_tar_create_directive_list_append(
        &options->directives,
        BX_TAR_CREATE_DIRECTIVE_EXCLUDE_PATTERN,
        pattern
    );
}

bool bx_tar_create_options_add_exclude_from(struct bx_tar_create_options* options,
                                            const char* path) {
    return bx_tar_create_directive_list_append(
        &options->directives,
        BX_TAR_CREATE_DIRECTIVE_EXCLUDE_FROM,
        path
    );
}

bool bx_tar_create_options_add_add_file(struct bx_tar_create_options* options,
                                        const char* path) {
    return bx_tar_create_directive_list_append(
        &options->directives,
        BX_TAR_CREATE_DIRECTIVE_ADD_PATH,
        path
    );
}

bool bx_tar_create_options_add_chdir(struct bx_tar_create_options* options,
                                     const char* path) {
    return bx_tar_create_directive_list_append(
        &options->directives,
        BX_TAR_CREATE_DIRECTIVE_CHDIR,
        path
    );
}

bool bx_tar_create_options_set_recurse(struct bx_tar_create_options* options,
                                       bool enabled) {
    return bx_tar_create_directive_list_append(
        &options->directives,
        enabled
            ? BX_TAR_CREATE_DIRECTIVE_RECURSE_ON
            : BX_TAR_CREATE_DIRECTIVE_RECURSE_OFF,
        NULL
    );
}

bool bx_tar_create_options_add_files_from(struct bx_tar_create_options* options,
                                          const char* path) {
    return bx_tar_create_directive_list_append(
        &options->directives,
        BX_TAR_CREATE_DIRECTIVE_FILES_FROM,
        path
    );
}

bool bx_tar_create_options_set_files_from_null(struct bx_tar_create_options* options,
                                               bool enabled) {
    return bx_tar_create_directive_list_append(
        &options->directives,
        enabled
            ? BX_TAR_CREATE_DIRECTIVE_FILES_FROM_NULL_ON
            : BX_TAR_CREATE_DIRECTIVE_FILES_FROM_NULL_OFF,
        NULL
    );
}

bool bx_tar_create_options_set_files_from_verbatim(struct bx_tar_create_options* options,
                                                   bool enabled) {
    return bx_tar_create_directive_list_append(
        &options->directives,
        enabled
            ? BX_TAR_CREATE_DIRECTIVE_FILES_FROM_VERBATIM_ON
            : BX_TAR_CREATE_DIRECTIVE_FILES_FROM_VERBATIM_OFF,
        NULL
    );
}

bool bx_tar_create_options_set_files_from_unquote(struct bx_tar_create_options* options,
                                                  bool enabled) {
    return bx_tar_create_directive_list_append(
        &options->directives,
        enabled
            ? BX_TAR_CREATE_DIRECTIVE_FILES_FROM_UNQUOTE_ON
            : BX_TAR_CREATE_DIRECTIVE_FILES_FROM_UNQUOTE_OFF,
        NULL
    );
}

bool bx_tar_create_options_set_anchored(struct bx_tar_create_options* options,
                                        bool enabled) {
    return bx_tar_create_directive_list_append(
        &options->directives,
        enabled
            ? BX_TAR_CREATE_DIRECTIVE_ANCHORED_ON
            : BX_TAR_CREATE_DIRECTIVE_ANCHORED_OFF,
        NULL
    );
}

bool bx_tar_create_options_set_ignore_case(struct bx_tar_create_options* options,
                                           bool enabled) {
    return bx_tar_create_directive_list_append(
        &options->directives,
        enabled
            ? BX_TAR_CREATE_DIRECTIVE_IGNORE_CASE_ON
            : BX_TAR_CREATE_DIRECTIVE_IGNORE_CASE_OFF,
        NULL
    );
}

bool bx_tar_create_options_set_wildcards(struct bx_tar_create_options* options,
                                         bool enabled) {
    return bx_tar_create_directive_list_append(
        &options->directives,
        enabled
            ? BX_TAR_CREATE_DIRECTIVE_WILDCARDS_ON
            : BX_TAR_CREATE_DIRECTIVE_WILDCARDS_OFF,
        NULL
    );
}

bool bx_tar_create_options_set_wildcards_match_slash(struct bx_tar_create_options* options,
                                                     bool enabled) {
    return bx_tar_create_directive_list_append(
        &options->directives,
        enabled
            ? BX_TAR_CREATE_DIRECTIVE_WILDCARDS_MATCH_SLASH_ON
            : BX_TAR_CREATE_DIRECTIVE_WILDCARDS_MATCH_SLASH_OFF,
        NULL
    );
}

bool bx_tar_create_options_set_exclude_caches(struct bx_tar_create_options* options) {
    return bx_tar_create_directive_list_append(&options->directives,
                                               BX_TAR_CREATE_DIRECTIVE_EXCLUDE_CACHES,
                                               NULL);
}

bool bx_tar_create_options_set_exclude_caches_all(struct bx_tar_create_options* options) {
    return bx_tar_create_directive_list_append(&options->directives,
                                               BX_TAR_CREATE_DIRECTIVE_EXCLUDE_CACHES_ALL,
                                               NULL);
}

bool bx_tar_create_options_set_exclude_caches_under(struct bx_tar_create_options* options) {
    return bx_tar_create_directive_list_append(&options->directives,
                                               BX_TAR_CREATE_DIRECTIVE_EXCLUDE_CACHES_UNDER,
                                               NULL);
}

bool bx_tar_create_options_add_exclude_ignore(struct bx_tar_create_options* options,
                                              const char* path) {
    return bx_tar_create_directive_list_append(&options->directives,
                                               BX_TAR_CREATE_DIRECTIVE_EXCLUDE_IGNORE,
                                               path);
}

bool bx_tar_create_options_add_exclude_ignore_recursive(struct bx_tar_create_options* options,
                                                        const char* path) {
    return bx_tar_create_directive_list_append(&options->directives,
                                               BX_TAR_CREATE_DIRECTIVE_EXCLUDE_IGNORE_RECURSIVE,
                                               path);
}

bool bx_tar_create_options_add_exclude_tag(struct bx_tar_create_options* options,
                                           const char* path) {
    return bx_tar_create_directive_list_append(&options->directives,
                                               BX_TAR_CREATE_DIRECTIVE_EXCLUDE_TAG,
                                               path);
}

bool bx_tar_create_options_add_exclude_tag_all(struct bx_tar_create_options* options,
                                               const char* path) {
    return bx_tar_create_directive_list_append(&options->directives,
                                               BX_TAR_CREATE_DIRECTIVE_EXCLUDE_TAG_ALL,
                                               path);
}

bool bx_tar_create_options_add_exclude_tag_under(struct bx_tar_create_options* options,
                                                 const char* path) {
    return bx_tar_create_directive_list_append(&options->directives,
                                               BX_TAR_CREATE_DIRECTIVE_EXCLUDE_TAG_UNDER,
                                               path);
}

bool bx_tar_create_options_set_exclude_vcs(struct bx_tar_create_options* options) {
    return bx_tar_create_directive_list_append(&options->directives,
                                               BX_TAR_CREATE_DIRECTIVE_EXCLUDE_VCS,
                                               NULL);
}

bool bx_tar_create_options_set_exclude_vcs_ignores(struct bx_tar_create_options* options) {
    return bx_tar_create_directive_list_append(&options->directives,
                                               BX_TAR_CREATE_DIRECTIVE_EXCLUDE_VCS_IGNORES,
                                               NULL);
}

static char* bx_tar_create_resolve_input_path(const char* cwd, const char* path) {
    if (path[0] == '/' || cwd == NULL) {
        return xstrdup(path);
    }
    return bx_path_join(cwd, path);
}

static char* bx_tar_create_resolve_list_path(const char* cwd, const char* path) {
    (void)cwd;
    if (strcmp(path, "-") == 0) {
        return xstrdup(path);
    }
    return xstrdup(path);
}

static bool bx_tar_create_set_cwd(struct bx_tar_files_from_state* state, const char* path) {
    char* next_cwd = bx_tar_create_resolve_input_path(state->cwd, path);
    free(state->cwd);
    state->cwd = next_cwd;
    return true;
}

static void bx_tar_create_state_set_null(struct bx_tar_files_from_state* state, bool enabled) {
    state->separator = enabled ? '\0' : '\n';
    state->verbatim = enabled;
}

static bool bx_tar_create_append_exclude_name_list(struct bx_tar_match_pattern_list* dest,
                                                   const struct bx_archive_name_list* src,
                                                   const struct bx_tar_match_policy* policy) {
    size_t i;

    for (i = 0u; i < src->len; i++) {
        if (!bx_tar_match_pattern_list_append(dest, src->items[i], policy)) {
            return false;
        }
    }
    return true;
}

static bool bx_tar_create_add_exclude_from_path(struct bx_tar_files_from_state* state,
                                                const char* path,
                                                struct bx_diag_ctx* diag) {
    struct bx_archive_name_list loaded = {0};
    char* resolved = bx_tar_create_resolve_list_path(state->cwd, path);
    bool ok = bx_archive_name_list_read_path(resolved, '\n', &loaded, diag);

    free(resolved);
    if (!ok) {
        bx_archive_name_list_free(&loaded);
        return false;
    }
    ok = bx_tar_create_append_exclude_name_list(&state->exclude_patterns,
                                                &loaded,
                                                &state->exclude_policy);
    bx_archive_name_list_free(&loaded);
    return ok;
}

static void bx_tar_create_dir_policy_free(struct bx_tar_create_dir_policy* policy) {
    free(policy->source_path);
    free(policy->archive_path);
    bx_tar_match_pattern_list_free(&policy->local_patterns);
    bx_tar_match_pattern_list_free(&policy->recursive_patterns);
    bx_archive_name_list_free(&policy->keep_names);
    memset(policy, 0, sizeof(*policy));
}

static void bx_tar_create_filter_state_cleanup(struct bx_tar_create_filter_state* state) {
    while (state->dir_policies_len > 0u) {
        bx_tar_create_dir_policy_free(&state->dir_policies[--state->dir_policies_len]);
    }
    free(state->dir_policies);
    state->dir_policies = NULL;
    state->dir_policies_cap = 0u;
}

static bool bx_tar_create_name_list_append_unique(struct bx_archive_name_list* list,
                                                  const char* name) {
    size_t i;

    for (i = 0u; i < list->len; i++) {
        if (strcmp(list->items[i], name) == 0) {
            return true;
        }
    }
    return bx_archive_name_list_append(list, name);
}

static bool bx_tar_create_name_list_contains(const struct bx_archive_name_list* list,
                                             const char* name) {
    size_t i;

    for (i = 0u; i < list->len; i++) {
        if (strcmp(list->items[i], name) == 0) {
            return true;
        }
    }
    return false;
}

static bool bx_tar_create_dir_policy_append(struct bx_tar_create_filter_state* state,
                                            const char* source_path,
                                            const char* archive_path,
                                            struct bx_tar_create_dir_policy** policy_out) {
    struct bx_tar_create_dir_policy* slot;

    if (state->dir_policies_len == state->dir_policies_cap) {
        size_t next_cap = state->dir_policies_cap ? state->dir_policies_cap * 2u : 8u;
        state->dir_policies = xrealloc(state->dir_policies,
                                       next_cap * sizeof(*state->dir_policies));
        state->dir_policies_cap = next_cap;
    }

    slot = &state->dir_policies[state->dir_policies_len++];
    memset(slot, 0, sizeof(*slot));
    slot->source_path = xstrdup(source_path);
    slot->archive_path = xstrdup(archive_path);
    *policy_out = slot;
    return true;
}

static const char* bx_tar_create_relative_child_path(const char* dir_path,
                                                     const char* path) {
    size_t dir_len = strlen(dir_path);

    if (strncmp(dir_path, path, dir_len) != 0 || path[dir_len] != '/') {
        return NULL;
    }
    return path + dir_len + 1u;
}

static bool bx_tar_create_path_contains_sep(const char* path) {
    return strchr(path, '/') != NULL;
}

static bool bx_tar_create_path_exists(const char* dir_path,
                                      const char* name) {
    char* full_path = bx_path_join(dir_path, name);
    bool exists = (access(full_path, F_OK) == 0);

    free(full_path);
    return exists;
}

static bool bx_tar_create_load_pattern_file(const char* path,
                                            struct bx_tar_match_pattern_list* out,
                                            const struct bx_tar_match_policy* policy,
                                            struct bx_diag_ctx* diag) {
    struct bx_archive_name_list loaded = {0};
    bool ok = true;
    size_t i;

    if (access(path, F_OK) != 0) {
        if (errno == ENOENT) {
            return true;
        }
        bx_diag(diag, "%s: %s", path, strerror(errno));
        return false;
    }
    if (!bx_archive_name_list_read_path(path, '\n', &loaded, diag)) {
        return false;
    }

    for (i = 0u; i < loaded.len; i++) {
        if (!bx_tar_match_pattern_list_append(out, loaded.items[i], policy)) {
            ok = false;
            break;
        }
    }
    bx_archive_name_list_free(&loaded);
    return ok;
}

static bool bx_tar_create_record_ignore_file_patterns(struct bx_tar_create_dir_policy* policy,
                                                      const char* dir_path,
                                                      const struct bx_archive_name_list* names,
                                                      bool recursive,
                                                      const struct bx_tar_match_policy* match_policy,
                                                      struct bx_diag_ctx* diag) {
    size_t i;

    for (i = 0u; i < names->len; i++) {
        char* full_path = bx_path_join(dir_path, names->items[i]);
        bool ok = bx_tar_create_load_pattern_file(full_path,
                                                  recursive
                                                      ? &policy->recursive_patterns
                                                      : &policy->local_patterns,
                                                  match_policy,
                                                  diag);
        free(full_path);
        if (!ok) {
            return false;
        }
    }
    return true;
}

static bool bx_tar_create_is_vcs_dir_name(const char* name) {
    static const char* const names[] = {
        "CVS",
        "RCS",
        "SCCS",
        ".git",
        ".hg",
        ".svn",
        ".bzr",
        "_darcs",
    };
    size_t i;

    for (i = 0u; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strcmp(name, names[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool bx_tar_create_record_vcs_ignore_patterns(struct bx_tar_create_dir_policy* policy,
                                                     const char* dir_path,
                                                     const struct bx_tar_match_policy* match_policy,
                                                     struct bx_diag_ctx* diag) {
    static const struct {
        const char* name;
        bool recursive;
    } files[] = {
        {".cvsignore", false},
        {".gitignore", true},
        {".hgignore", true},
        {".bzrignore", true},
    };
    size_t i;

    for (i = 0u; i < sizeof(files) / sizeof(files[0]); i++) {
        char* full_path = bx_path_join(dir_path, files[i].name);
        bool ok = bx_tar_create_load_pattern_file(full_path,
                                                  files[i].recursive
                                                      ? &policy->recursive_patterns
                                                      : &policy->local_patterns,
                                                  match_policy,
                                                  diag);
        free(full_path);
        if (!ok) {
            return false;
        }
    }
    return true;
}

static bool bx_tar_create_policy_matches_relative(const struct bx_tar_create_dir_policy* policy,
                                                  const char* archive_path) {
    return strcmp(policy->archive_path, archive_path) == 0;
}

static bool bx_tar_create_check_ancestor_policies(const struct bx_tar_create_filter_state* state,
                                                  const char* source_path,
                                                  const char* archive_path) {
    size_t i;

    for (i = 0u; i < state->dir_policies_len; i++) {
        const struct bx_tar_create_dir_policy* policy = &state->dir_policies[i];
        const char* rel_source = bx_tar_create_relative_child_path(policy->source_path, source_path);
        const char* rel_archive;

        if (rel_source == NULL || bx_tar_create_policy_matches_relative(policy, archive_path)) {
            continue;
        }
        rel_archive = bx_tar_create_relative_child_path(policy->archive_path, archive_path);
        if (rel_archive == NULL) {
            continue;
        }
        if (policy->exclude_under) {
            return false;
        }
        if (policy->exclude_except_keep) {
            if (bx_tar_create_path_contains_sep(rel_source)
                || !bx_tar_create_name_list_contains(&policy->keep_names,
                                                     bx_path_basename_ptr(rel_source))) {
                return false;
            }
        }
        if (bx_tar_path_excluded(&policy->recursive_patterns, rel_archive)) {
            return false;
        }
        if (!bx_tar_create_path_contains_sep(rel_source)
            && bx_tar_path_excluded(&policy->local_patterns, rel_archive)) {
            return false;
        }
    }
    return true;
}

static bool bx_tar_create_dir_has_any_marker(const char* dir_path,
                                             const struct bx_archive_name_list* names,
                                             struct bx_archive_name_list* found_names) {
    size_t i;

    for (i = 0u; i < names->len; i++) {
        if (bx_tar_create_path_exists(dir_path, names->items[i])) {
            if (found_names != NULL
                && !bx_archive_name_list_append(found_names, names->items[i])) {
                return false;
            }
        }
    }
    return true;
}

static bool bx_tar_create_maybe_record_dir_policy(struct bx_tar_create_filter_state* state,
                                                  const char* source_path,
                                                  const char* archive_path) {
    struct bx_tar_create_dir_policy* policy = NULL;
    struct bx_archive_name_list keep_names = {0};
    bool exclude_all = false;
    bool exclude_under = false;
    bool exclude_except_keep = false;
    bool need_policy = false;
    bool ok = true;

    size_t i;

    if (state->exclude_caches_all && bx_tar_create_path_exists(source_path, "CACHEDIR.TAG")) {
        exclude_all = true;
    }
    for (i = 0u; !exclude_all && i < state->exclude_tag_all_files->len; i++) {
        if (bx_tar_create_path_exists(source_path, state->exclude_tag_all_files->items[i])) {
            exclude_all = true;
        }
    }
    if (exclude_all) {
        bx_archive_name_list_free(&keep_names);
        return false;
    }

    if (state->exclude_caches_under && bx_tar_create_path_exists(source_path, "CACHEDIR.TAG")) {
        exclude_under = true;
    }
    for (i = 0u; !exclude_under && i < state->exclude_tag_under_files->len; i++) {
        if (bx_tar_create_path_exists(source_path, state->exclude_tag_under_files->items[i])) {
            exclude_under = true;
        }
    }

    if (!exclude_under) {
        if (state->exclude_caches && bx_tar_create_path_exists(source_path, "CACHEDIR.TAG")) {
            exclude_except_keep = true;
            if (!bx_tar_create_name_list_append_unique(&keep_names, "CACHEDIR.TAG")) {
                ok = false;
            }
        }
        if (ok
            && !bx_tar_create_dir_has_any_marker(source_path,
                                                 state->exclude_tag_files,
                                                 &keep_names)) {
            ok = false;
        }
        exclude_except_keep = exclude_except_keep || keep_names.len > 0u;
    }

    if (!ok) {
        bx_archive_name_list_free(&keep_names);
        return false;
    }

    need_policy = exclude_under
        || exclude_except_keep
        || state->exclude_ignore_files->len > 0u
        || state->exclude_ignore_recursive_files->len > 0u
        || state->exclude_vcs_ignores;
    if (!need_policy) {
        bx_archive_name_list_free(&keep_names);
        return true;
    }

    if (!bx_tar_create_dir_policy_append(state, source_path, archive_path, &policy)) {
        bx_archive_name_list_free(&keep_names);
        return false;
    }
    policy->keep_names = keep_names;
    policy->exclude_under = exclude_under;
    policy->exclude_except_keep = exclude_except_keep;

    if (!bx_tar_create_record_ignore_file_patterns(policy,
                                                   source_path,
                                                   state->exclude_ignore_files,
                                                   false,
                                                   state->exclude_policy,
                                                   state->diag)
        || !bx_tar_create_record_ignore_file_patterns(policy,
                                                      source_path,
                                                      state->exclude_ignore_recursive_files,
                                                      true,
                                                      state->exclude_policy,
                                                      state->diag)) {
        return false;
    }
    if (state->exclude_vcs_ignores
        && !bx_tar_create_record_vcs_ignore_patterns(policy,
                                                     source_path,
                                                     state->exclude_policy,
                                                     state->diag)) {
        return false;
    }

    return true;
}

static bool bx_tar_create_include_path(const char* source_path,
                                       const char* archive_path,
                                       const struct stat* st,
                                       void* user_data) {
    struct bx_tar_create_filter_state* state = user_data;

    if (!bx_tar_create_check_ancestor_policies(state, source_path, archive_path)) {
        return false;
    }
    if (bx_tar_path_excluded(state->exclude_patterns, archive_path)) {
        return false;
    }
    if (state->exclude_vcs
        && S_ISDIR(st->st_mode)
        && bx_tar_create_is_vcs_dir_name(bx_path_basename_ptr(archive_path))) {
        return false;
    }
    if (S_ISDIR(st->st_mode)
        && !bx_tar_create_maybe_record_dir_policy(state, source_path, archive_path)) {
        return false;
    }
    return true;
}

static const char* bx_tar_create_error_verb(enum bx_archive_fs_error_op op) {
    switch (op) {
        case BX_ARCHIVE_FS_ERROR_LSTAT:
            return "stat";
        case BX_ARCHIVE_FS_ERROR_READLINK:
            return "readlink";
        case BX_ARCHIVE_FS_ERROR_OPENDIR:
            return "open";
        case BX_ARCHIVE_FS_ERROR_CLOSEDIR:
            return "close";
    }

    return "access";
}

static enum bx_archive_fs_error_action
bx_tar_create_handle_fs_error(const char* source_path,
                              enum bx_archive_fs_error_op op,
                              int errnum,
                              void* user_data) {
    struct bx_tar_create_filter_state* state = user_data;

    fprintf(stderr,
            "%s: %s: Cannot %s: %s\n",
            state->diag->progname,
            source_path,
            bx_tar_create_error_verb(op),
            strerror(errnum));
    *state->had_create_errors = true;
    return BX_ARCHIVE_FS_ERROR_SKIP;
}

static bool bx_tar_create_collect_entry(const struct bx_archive_fs_visit_entry* entry,
                                        void* user_data,
                                        struct bx_diag_ctx* diag) {
    struct bx_archive_fs_list* list = user_data;
    (void)diag;

    if (list->len == list->cap) {
        size_t next_cap = list->cap ? list->cap * 2u : 32u;
        list->entries = xrealloc(list->entries, next_cap * sizeof(*list->entries));
        list->cap = next_cap;
    }

    list->entries[list->len].source_path = xstrdup(entry->source_path);
    list->entries[list->len].archive_path = xstrdup(entry->archive_path);
    list->entries[list->len].st = *entry->st;
    list->entries[list->len].link_target = entry->link_target ? xstrdup(entry->link_target) : NULL;
    list->len++;
    return true;
}

static bool bx_tar_create_add_path(struct bx_tar_create_collect_ctx* ctx,
                                   const struct bx_tar_files_from_state* state,
                                   const char* name) {
    struct bx_tar_create_filter_state filter_state = {
        .exclude_patterns = &state->exclude_patterns,
        .exclude_ignore_files = &state->exclude_ignore_files,
        .exclude_ignore_recursive_files = &state->exclude_ignore_recursive_files,
        .exclude_tag_files = &state->exclude_tag_files,
        .exclude_tag_all_files = &state->exclude_tag_all_files,
        .exclude_tag_under_files = &state->exclude_tag_under_files,
        .exclude_policy = &state->exclude_policy,
        .exclude_caches = state->exclude_caches,
        .exclude_caches_all = state->exclude_caches_all,
        .exclude_caches_under = state->exclude_caches_under,
        .exclude_vcs = state->exclude_vcs,
        .exclude_vcs_ignores = state->exclude_vcs_ignores,
        .diag = ctx->diag,
        .had_create_errors = &ctx->had_create_errors,
    };
    char* source_path = bx_tar_create_resolve_input_path(state->cwd, name);
    bool ok = bx_archive_fs_visit_path_filtered(source_path,
                                                name,
                                                state->recurse,
                                                ctx->sort_children,
                                                bx_tar_create_include_path,
                                                &filter_state,
                                                bx_tar_create_handle_fs_error,
                                                &filter_state,
                                                ctx->visit_fn,
                                                ctx->visit_user_data,
                                                ctx->diag);

    bx_tar_create_filter_state_cleanup(&filter_state);
    free(source_path);
    return ok;
}

static bool bx_tar_create_note_list_option_error(struct bx_tar_create_collect_ctx* ctx,
                                                 const char* list_path,
                                                 size_t record_no,
                                                 const char* message) {
    bx_tar_files_from_report_option_error(ctx->diag, list_path, record_no, message);
    ctx->had_create_errors = true;
    return true;
}

static bool bx_tar_create_process_files_from(struct bx_tar_create_collect_ctx* ctx,
                                             struct bx_tar_files_from_state* state,
                                             const char* list_path,
                                             const char* display_path);

static bool bx_tar_create_process_option_record(struct bx_tar_create_collect_ctx* ctx,
                                                struct bx_tar_files_from_state* state,
                                                const char* record,
                                                const char* list_path,
                                                size_t record_no) {
    const char* arg = NULL;
    char* decoded = NULL;

    if (strcmp(record, "--null") == 0) {
        bx_tar_create_state_set_null(state, true);
        return true;
    }
    if (strcmp(record, "--no-null") == 0) {
        bx_tar_create_state_set_null(state, false);
        return true;
    }
    if (strcmp(record, "--verbatim-files-from") == 0) {
        state->verbatim = true;
        return true;
    }
    if (strcmp(record, "--no-verbatim-files-from") == 0) {
        state->verbatim = false;
        return true;
    }
    if (strcmp(record, "--unquote") == 0) {
        state->unquote = true;
        return true;
    }
    if (strcmp(record, "--no-unquote") == 0) {
        state->unquote = false;
        return true;
    }
    if (strcmp(record, "--no-recursion") == 0) {
        state->recurse = false;
        return true;
    }
    if (strcmp(record, "--recursion") == 0) {
        state->recurse = true;
        return true;
    }
    if (strcmp(record, "--anchored") == 0) {
        return bx_tar_match_policy_set_anchored(&state->member_policy,
                                                &state->exclude_policy,
                                                true);
    }
    if (strcmp(record, "--no-anchored") == 0) {
        return bx_tar_match_policy_set_anchored(&state->member_policy,
                                                &state->exclude_policy,
                                                false);
    }
    if (strcmp(record, "--ignore-case") == 0) {
        return bx_tar_match_policy_set_ignore_case(&state->member_policy,
                                                   &state->exclude_policy,
                                                   true);
    }
    if (strcmp(record, "--no-ignore-case") == 0) {
        return bx_tar_match_policy_set_ignore_case(&state->member_policy,
                                                   &state->exclude_policy,
                                                   false);
    }
    if (strcmp(record, "--wildcards") == 0) {
        return bx_tar_match_policy_set_wildcards(&state->member_policy,
                                                 &state->exclude_policy,
                                                 true);
    }
    if (strcmp(record, "--no-wildcards") == 0) {
        return bx_tar_match_policy_set_wildcards(&state->member_policy,
                                                 &state->exclude_policy,
                                                 false);
    }
    if (strcmp(record, "--wildcards-match-slash") == 0) {
        return bx_tar_match_policy_set_wildcards_match_slash(&state->member_policy,
                                                             &state->exclude_policy,
                                                             true);
    }
    if (strcmp(record, "--no-wildcards-match-slash") == 0) {
        return bx_tar_match_policy_set_wildcards_match_slash(&state->member_policy,
                                                             &state->exclude_policy,
                                                             false);
    }
    if (strcmp(record, "--exclude-caches") == 0) {
        state->exclude_caches = true;
        return true;
    }
    if (strcmp(record, "--exclude-caches-all") == 0) {
        state->exclude_caches_all = true;
        return true;
    }
    if (strcmp(record, "--exclude-caches-under") == 0) {
        state->exclude_caches_under = true;
        return true;
    }
    if (strcmp(record, "--exclude-vcs") == 0) {
        state->exclude_vcs = true;
        return true;
    }
    if (strcmp(record, "--exclude-vcs-ignores") == 0) {
        state->exclude_vcs_ignores = true;
        return true;
    }

    if (strncmp(record, "--exclude=", strlen("--exclude=")) == 0) {
        arg = record + strlen("--exclude=");
    }
    else if (strncmp(record, "--exclude", strlen("--exclude")) == 0) {
        const char* rest = record + strlen("--exclude");
        if (*rest == '\0') {
            return bx_tar_create_note_list_option_error(
                ctx,
                list_path,
                record_no,
                "unrecognized option"
            );
        }
        if (*rest != ' ' && *rest != '\t') {
            return bx_tar_create_note_list_option_error(
                ctx,
                list_path,
                record_no,
                "unrecognized option"
            );
        }
        arg = bx_tar_files_from_skip_inline_space(rest);
    }

    if (arg != NULL) {
        bool ok;

        decoded = bx_tar_files_from_decode_text(state->verbatim, state->unquote, arg);
        ok = bx_tar_match_pattern_list_append(&state->exclude_patterns,
                                              decoded,
                                              &state->exclude_policy);
        free(decoded);
        return ok;
    }

    arg = NULL;
    if (strncmp(record, "--exclude-from=", strlen("--exclude-from=")) == 0) {
        arg = record + strlen("--exclude-from=");
    }
    else if (strncmp(record, "--exclude-from", strlen("--exclude-from")) == 0) {
        const char* rest = record + strlen("--exclude-from");
        if (*rest == '\0') {
            return bx_tar_create_note_list_option_error(
                ctx,
                list_path,
                record_no,
                "unrecognized option"
            );
        }
        if (*rest != ' ' && *rest != '\t') {
            return bx_tar_create_note_list_option_error(
                ctx,
                list_path,
                record_no,
                "unrecognized option"
            );
        }
        arg = bx_tar_files_from_skip_inline_space(rest);
    }
    else if (record[0] == '-' && record[1] == 'X') {
        arg = record + 2u;
        if (*arg == ' ' || *arg == '\t') {
            arg = bx_tar_files_from_skip_inline_space(arg);
        }
        if (*arg == '\0') {
            return bx_tar_create_note_list_option_error(
                ctx,
                list_path,
                record_no,
                "unrecognized option"
            );
        }
    }

    if (arg != NULL) {
        bool ok;

        decoded = bx_tar_files_from_decode_text(state->verbatim, state->unquote, arg);
        ok = bx_tar_create_add_exclude_from_path(state, decoded, ctx->diag);
        free(decoded);
        return ok;
    }

    arg = NULL;
    if (strncmp(record, "--exclude-ignore=", strlen("--exclude-ignore=")) == 0) {
        arg = record + strlen("--exclude-ignore=");
    }
    else if (strncmp(record, "--exclude-ignore", strlen("--exclude-ignore")) == 0) {
        const char* rest = record + strlen("--exclude-ignore");
        if (*rest == '\0' || (*rest != ' ' && *rest != '\t')) {
            return bx_tar_create_note_list_option_error(ctx, list_path, record_no, "unrecognized option");
        }
        arg = bx_tar_files_from_skip_inline_space(rest);
    }
    if (arg != NULL) {
        bool ok;
        decoded = bx_tar_files_from_decode_text(state->verbatim, state->unquote, arg);
        ok = bx_archive_name_list_append(&state->exclude_ignore_files, decoded);
        free(decoded);
        return ok;
    }

    arg = NULL;
    if (strncmp(record, "--exclude-ignore-recursive=", strlen("--exclude-ignore-recursive=")) == 0) {
        arg = record + strlen("--exclude-ignore-recursive=");
    }
    else if (strncmp(record, "--exclude-ignore-recursive", strlen("--exclude-ignore-recursive")) == 0) {
        const char* rest = record + strlen("--exclude-ignore-recursive");
        if (*rest == '\0' || (*rest != ' ' && *rest != '\t')) {
            return bx_tar_create_note_list_option_error(ctx, list_path, record_no, "unrecognized option");
        }
        arg = bx_tar_files_from_skip_inline_space(rest);
    }
    if (arg != NULL) {
        bool ok;
        decoded = bx_tar_files_from_decode_text(state->verbatim, state->unquote, arg);
        ok = bx_archive_name_list_append(&state->exclude_ignore_recursive_files, decoded);
        free(decoded);
        return ok;
    }

    arg = NULL;
    if (strncmp(record, "--exclude-tag=", strlen("--exclude-tag=")) == 0) {
        arg = record + strlen("--exclude-tag=");
    }
    else if (strncmp(record, "--exclude-tag", strlen("--exclude-tag")) == 0) {
        const char* rest = record + strlen("--exclude-tag");
        if (*rest == '\0' || (*rest != ' ' && *rest != '\t')) {
            return bx_tar_create_note_list_option_error(ctx, list_path, record_no, "unrecognized option");
        }
        arg = bx_tar_files_from_skip_inline_space(rest);
    }
    if (arg != NULL) {
        bool ok;
        decoded = bx_tar_files_from_decode_text(state->verbatim, state->unquote, arg);
        ok = bx_archive_name_list_append(&state->exclude_tag_files, decoded);
        free(decoded);
        return ok;
    }

    arg = NULL;
    if (strncmp(record, "--exclude-tag-all=", strlen("--exclude-tag-all=")) == 0) {
        arg = record + strlen("--exclude-tag-all=");
    }
    else if (strncmp(record, "--exclude-tag-all", strlen("--exclude-tag-all")) == 0) {
        const char* rest = record + strlen("--exclude-tag-all");
        if (*rest == '\0' || (*rest != ' ' && *rest != '\t')) {
            return bx_tar_create_note_list_option_error(ctx, list_path, record_no, "unrecognized option");
        }
        arg = bx_tar_files_from_skip_inline_space(rest);
    }
    if (arg != NULL) {
        bool ok;
        decoded = bx_tar_files_from_decode_text(state->verbatim, state->unquote, arg);
        ok = bx_archive_name_list_append(&state->exclude_tag_all_files, decoded);
        free(decoded);
        return ok;
    }

    arg = NULL;
    if (strncmp(record, "--exclude-tag-under=", strlen("--exclude-tag-under=")) == 0) {
        arg = record + strlen("--exclude-tag-under=");
    }
    else if (strncmp(record, "--exclude-tag-under", strlen("--exclude-tag-under")) == 0) {
        const char* rest = record + strlen("--exclude-tag-under");
        if (*rest == '\0' || (*rest != ' ' && *rest != '\t')) {
            return bx_tar_create_note_list_option_error(ctx, list_path, record_no, "unrecognized option");
        }
        arg = bx_tar_files_from_skip_inline_space(rest);
    }
    if (arg != NULL) {
        bool ok;
        decoded = bx_tar_files_from_decode_text(state->verbatim, state->unquote, arg);
        ok = bx_archive_name_list_append(&state->exclude_tag_under_files, decoded);
        free(decoded);
        return ok;
    }

    if (strncmp(record, "--directory=", strlen("--directory=")) == 0) {
        arg = record + strlen("--directory=");
    }
    else if (strncmp(record, "--directory", strlen("--directory")) == 0) {
        const char* rest = record + strlen("--directory");
        if (*rest == '\0') {
            return bx_tar_create_note_list_option_error(
                ctx,
                list_path,
                record_no,
                "unrecognized option"
            );
        }
        if (*rest != ' ' && *rest != '\t') {
            return bx_tar_create_note_list_option_error(
                ctx,
                list_path,
                record_no,
                "unrecognized option"
            );
        }
        arg = bx_tar_files_from_skip_inline_space(rest);
    }
    else if (record[0] == '-' && record[1] == 'C') {
        arg = record + 2u;
        if (*arg == ' ' || *arg == '\t') {
            arg = bx_tar_files_from_skip_inline_space(arg);
        }
        if (*arg == '\0') {
            return bx_tar_create_note_list_option_error(
                ctx,
                list_path,
                record_no,
                "unrecognized option"
            );
        }
    }

    if (arg != NULL) {
        decoded = bx_tar_files_from_decode_text(state->verbatim, state->unquote, arg);
        bx_tar_create_set_cwd(state, decoded);
        free(decoded);
        return true;
    }

    arg = NULL;
    if (strncmp(record, "--files-from=", strlen("--files-from=")) == 0) {
        arg = record + strlen("--files-from=");
    }
    else if (strncmp(record, "--files-from", strlen("--files-from")) == 0) {
        const char* rest = record + strlen("--files-from");
        if (*rest == '\0') {
            return bx_tar_create_note_list_option_error(
                ctx,
                list_path,
                record_no,
                "unrecognized option"
            );
        }
        if (*rest != ' ' && *rest != '\t') {
            return bx_tar_create_note_list_option_error(
                ctx,
                list_path,
                record_no,
                "unrecognized option"
            );
        }
        arg = bx_tar_files_from_skip_inline_space(rest);
    }
    else if (record[0] == '-' && record[1] == 'T') {
        arg = record + 2u;
        if (*arg == ' ' || *arg == '\t') {
            arg = bx_tar_files_from_skip_inline_space(arg);
        }
        if (*arg == '\0') {
            return bx_tar_create_note_list_option_error(
                ctx,
                list_path,
                record_no,
                "unrecognized option"
            );
        }
    }

    if (arg != NULL) {
        char* resolved;
        bool ok;

        decoded = bx_tar_files_from_decode_text(state->verbatim, state->unquote, arg);
        resolved = bx_tar_create_resolve_list_path(state->cwd, decoded);
        ok = bx_tar_create_process_files_from(ctx, state, resolved, decoded);
        free(resolved);
        free(decoded);
        return ok;
    }

    arg = NULL;
    if (strncmp(record, "--add-file=", strlen("--add-file=")) == 0) {
        arg = record + strlen("--add-file=");
    }
    else if (strncmp(record, "--add-file", strlen("--add-file")) == 0) {
        const char* rest = record + strlen("--add-file");
        if (*rest == '\0') {
            return bx_tar_create_note_list_option_error(
                ctx,
                list_path,
                record_no,
                "unrecognized option"
            );
        }
        if (*rest != ' ' && *rest != '\t') {
            return bx_tar_create_note_list_option_error(
                ctx,
                list_path,
                record_no,
                "unrecognized option"
            );
        }
        arg = bx_tar_files_from_skip_inline_space(rest);
    }

    if (arg != NULL) {
        bool ok;

        decoded = bx_tar_files_from_decode_text(state->verbatim, state->unquote, arg);
        ok = bx_tar_create_add_path(ctx, state, decoded);
        free(decoded);
        return ok;
    }

    return bx_tar_create_note_list_option_error(ctx, list_path, record_no, "unrecognized option");
}

static bool bx_tar_create_process_record(struct bx_tar_create_collect_ctx* ctx,
                                         struct bx_tar_files_from_state* state,
                                         const char* record,
                                         const char* list_path,
                                         size_t record_no) {
    char* decoded;
    bool ok;

    if (record[0] == '\0') {
        return true;
    }
    if (!state->verbatim && record[0] == '-') {
        return bx_tar_create_process_option_record(ctx, state, record, list_path, record_no);
    }

    decoded = bx_tar_files_from_decode_text(state->verbatim, state->unquote, record);
    ok = bx_tar_create_add_path(ctx, state, decoded);
    free(decoded);
    return ok;
}

static bool bx_tar_create_process_files_from(struct bx_tar_create_collect_ctx* ctx,
                                             struct bx_tar_files_from_state* state,
                                             const char* list_path,
                                             const char* display_path) {
    struct bx_archive_buffer input = {0};
    size_t pos = 0u;
    size_t record_no = 0u;

    if (!bx_tar_files_from_read_buffer(list_path, &input, ctx->diag)) {
        return false;
    }

    while (pos <= input.len) {
        unsigned char separator = state->separator;
        size_t start = pos;
        bool at_end;
        char* record;
        size_t record_len;
        bool ok;

        while (pos < input.len && input.data[pos] != separator) {
            pos++;
        }
        at_end = (pos == input.len);
        record_len = pos - start;
        record = xmalloc(record_len + 1u);
        if (record_len > 0u) {
            memcpy(record, input.data + start, record_len);
        }
        record[record_len] = '\0';
        record_no++;

        ok = bx_tar_create_process_record(ctx, state, record, display_path, record_no);
        free(record);
        if (!ok) {
            bx_archive_buffer_free(&input);
            return false;
        }

        if (at_end) {
            break;
        }
        pos++;
    }

    bx_archive_buffer_free(&input);
    return true;
}

static bool bx_tar_create_apply_directive(struct bx_tar_create_collect_ctx* ctx,
                                          struct bx_tar_files_from_state* state,
                                          const struct bx_tar_create_directive* directive) {
    switch (directive->kind) {
        case BX_TAR_CREATE_DIRECTIVE_CHDIR:
            return bx_tar_create_set_cwd(state, directive->text);
        case BX_TAR_CREATE_DIRECTIVE_ADD_PATH:
            return bx_tar_create_add_path(ctx, state, directive->text);
        case BX_TAR_CREATE_DIRECTIVE_EXCLUDE_PATTERN:
            return bx_tar_match_pattern_list_append(&state->exclude_patterns,
                                                    directive->text,
                                                    &state->exclude_policy);
        case BX_TAR_CREATE_DIRECTIVE_EXCLUDE_FROM:
            return bx_tar_create_add_exclude_from_path(state, directive->text, ctx->diag);
        case BX_TAR_CREATE_DIRECTIVE_RECURSE_ON:
            state->recurse = true;
            return true;
        case BX_TAR_CREATE_DIRECTIVE_RECURSE_OFF:
            state->recurse = false;
            return true;
        case BX_TAR_CREATE_DIRECTIVE_FILES_FROM: {
            char* resolved = bx_tar_create_resolve_list_path(state->cwd, directive->text);
            bool ok = bx_tar_create_process_files_from(ctx, state, resolved, directive->text);
            free(resolved);
            return ok;
        }
        case BX_TAR_CREATE_DIRECTIVE_FILES_FROM_NULL_ON:
            bx_tar_create_state_set_null(state, true);
            return true;
        case BX_TAR_CREATE_DIRECTIVE_FILES_FROM_NULL_OFF:
            bx_tar_create_state_set_null(state, false);
            return true;
        case BX_TAR_CREATE_DIRECTIVE_FILES_FROM_VERBATIM_ON:
            state->verbatim = true;
            return true;
        case BX_TAR_CREATE_DIRECTIVE_FILES_FROM_VERBATIM_OFF:
            state->verbatim = false;
            return true;
        case BX_TAR_CREATE_DIRECTIVE_FILES_FROM_UNQUOTE_ON:
            state->unquote = true;
            return true;
        case BX_TAR_CREATE_DIRECTIVE_FILES_FROM_UNQUOTE_OFF:
            state->unquote = false;
            return true;
        case BX_TAR_CREATE_DIRECTIVE_ANCHORED_ON:
            return bx_tar_match_policy_set_anchored(&state->member_policy,
                                                    &state->exclude_policy,
                                                    true);
        case BX_TAR_CREATE_DIRECTIVE_ANCHORED_OFF:
            return bx_tar_match_policy_set_anchored(&state->member_policy,
                                                    &state->exclude_policy,
                                                    false);
        case BX_TAR_CREATE_DIRECTIVE_IGNORE_CASE_ON:
            return bx_tar_match_policy_set_ignore_case(&state->member_policy,
                                                       &state->exclude_policy,
                                                       true);
        case BX_TAR_CREATE_DIRECTIVE_IGNORE_CASE_OFF:
            return bx_tar_match_policy_set_ignore_case(&state->member_policy,
                                                       &state->exclude_policy,
                                                       false);
        case BX_TAR_CREATE_DIRECTIVE_WILDCARDS_ON:
            return bx_tar_match_policy_set_wildcards(&state->member_policy,
                                                     &state->exclude_policy,
                                                     true);
        case BX_TAR_CREATE_DIRECTIVE_WILDCARDS_OFF:
            return bx_tar_match_policy_set_wildcards(&state->member_policy,
                                                     &state->exclude_policy,
                                                     false);
        case BX_TAR_CREATE_DIRECTIVE_WILDCARDS_MATCH_SLASH_ON:
            return bx_tar_match_policy_set_wildcards_match_slash(&state->member_policy,
                                                                 &state->exclude_policy,
                                                                 true);
        case BX_TAR_CREATE_DIRECTIVE_WILDCARDS_MATCH_SLASH_OFF:
            return bx_tar_match_policy_set_wildcards_match_slash(&state->member_policy,
                                                                 &state->exclude_policy,
                                                                 false);
        case BX_TAR_CREATE_DIRECTIVE_EXCLUDE_CACHES:
            state->exclude_caches = true;
            return true;
        case BX_TAR_CREATE_DIRECTIVE_EXCLUDE_CACHES_ALL:
            state->exclude_caches_all = true;
            return true;
        case BX_TAR_CREATE_DIRECTIVE_EXCLUDE_CACHES_UNDER:
            state->exclude_caches_under = true;
            return true;
        case BX_TAR_CREATE_DIRECTIVE_EXCLUDE_IGNORE:
            return bx_archive_name_list_append(&state->exclude_ignore_files, directive->text);
        case BX_TAR_CREATE_DIRECTIVE_EXCLUDE_IGNORE_RECURSIVE:
            return bx_archive_name_list_append(&state->exclude_ignore_recursive_files, directive->text);
        case BX_TAR_CREATE_DIRECTIVE_EXCLUDE_TAG:
            return bx_archive_name_list_append(&state->exclude_tag_files, directive->text);
        case BX_TAR_CREATE_DIRECTIVE_EXCLUDE_TAG_ALL:
            return bx_archive_name_list_append(&state->exclude_tag_all_files, directive->text);
        case BX_TAR_CREATE_DIRECTIVE_EXCLUDE_TAG_UNDER:
            return bx_archive_name_list_append(&state->exclude_tag_under_files, directive->text);
        case BX_TAR_CREATE_DIRECTIVE_EXCLUDE_VCS:
            state->exclude_vcs = true;
            return true;
        case BX_TAR_CREATE_DIRECTIVE_EXCLUDE_VCS_IGNORES:
            state->exclude_vcs_ignores = true;
            return true;
    }

    return true;
}

bool bx_tar_create_visit_fs_entries(const struct bx_tar_create_options* create_options,
                                    bool sort_children,
                                    bx_archive_fs_visit_fn visit_fn,
                                    void* visit_user_data,
                                    bool* had_create_errors,
                                    struct bx_diag_ctx* diag) {
    struct bx_tar_files_from_state state = {
        .cwd = NULL,
        .separator = '\n',
        .verbatim = false,
        .unquote = true,
        .recurse = true,
        .exclude_patterns = {0},
        .exclude_ignore_files = {0},
        .exclude_ignore_recursive_files = {0},
        .exclude_tag_files = {0},
        .exclude_tag_all_files = {0},
        .exclude_tag_under_files = {0},
    };
    struct bx_tar_create_collect_ctx ctx = {
        .visit_fn = visit_fn,
        .visit_user_data = visit_user_data,
        .sort_children = sort_children,
        .had_create_errors = false,
        .diag = diag,
    };
    size_t i;
    bool ok = true;

    bx_tar_match_policy_init_member_default(&state.member_policy);
    bx_tar_match_policy_init_exclude_default(&state.exclude_policy);
    for (i = 0u; i < create_options->directives.len; i++) {
        if (!bx_tar_create_apply_directive(&ctx, &state, &create_options->directives.items[i])) {
            ok = false;
            goto out;
        }
    }

out:
    if (had_create_errors != NULL) {
        *had_create_errors = ctx.had_create_errors;
    }
    bx_tar_match_pattern_list_free(&state.exclude_patterns);
    bx_archive_name_list_free(&state.exclude_ignore_files);
    bx_archive_name_list_free(&state.exclude_ignore_recursive_files);
    bx_archive_name_list_free(&state.exclude_tag_files);
    bx_archive_name_list_free(&state.exclude_tag_all_files);
    bx_archive_name_list_free(&state.exclude_tag_under_files);
    free(state.cwd);
    return ok;
}

bool bx_tar_create_collect_fs_entries(struct bx_archive_fs_list* list,
                                      const struct bx_tar_create_options* create_options,
                                      bool sort_children,
                                      bool* had_create_errors,
                                      struct bx_diag_ctx* diag) {
    return bx_tar_create_visit_fs_entries(create_options,
                                          sort_children,
                                          bx_tar_create_collect_entry,
                                          list,
                                          had_create_errors,
                                          diag);
}

static bool bx_tar_create_path_seen(const struct bx_archive_name_list* seen_paths, const char* path) {
    size_t i;

    for (i = 0u; i < seen_paths->len; i++) {
        if (strcmp(seen_paths->items[i], path) == 0) {
            return true;
        }
    }
    return false;
}

static void bx_tar_create_report_remove_failure(const struct bx_diag_ctx* diag,
                                                const char* path,
                                                bool is_directory) {
    fprintf(stderr,
            "%s: %s: Cannot %s: %s\n",
            diag->progname,
            path,
            is_directory ? "rmdir" : "unlink",
            strerror(errno));
}

bool bx_tar_create_remove_archived_sources(const struct bx_archive_fs_list* list,
                                           const struct bx_diag_ctx* diag) {
    struct bx_archive_name_list seen_paths = {0};
    bool ok = true;
    size_t i = list->len;

    while (i > 0u) {
        const struct bx_archive_fs_entry* entry = &list->entries[--i];
        bool is_directory = S_ISDIR(entry->st.st_mode);
        int rc;

        if (bx_tar_create_path_seen(&seen_paths, entry->source_path)) {
            continue;
        }
        bx_archive_name_list_append(&seen_paths, entry->source_path);

        rc = is_directory ? rmdir(entry->source_path) : unlink(entry->source_path);
        if (rc != 0) {
            bx_tar_create_report_remove_failure(diag, entry->source_path, is_directory);
            ok = false;
        }
    }

    bx_archive_name_list_free(&seen_paths);
    return ok;
}
