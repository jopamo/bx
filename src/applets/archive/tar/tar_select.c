#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "applets/archive/tar/tar_files_from.h"
#include "applets/archive/tar/tar_patterns.h"
#include "applets/archive/tar/tar_select.h"
#include "bx/libbx.h"
#include "lib/path_ops.h"

struct bx_tar_select_state {
    char* extract_dir;
    unsigned char separator;
    bool verbatim;
    bool unquote;
    bool recurse;
    struct bx_tar_match_policy member_policy;
    struct bx_tar_match_policy exclude_policy;
    bool had_errors;
    struct bx_tar_select_plan* plan;
    struct bx_diag_ctx* diag;
};

static bool bx_tar_select_plan_append_member(struct bx_tar_select_plan* plan,
                                             const char* name,
                                             const char* extract_dir,
                                             const struct bx_tar_match_policy* policy,
                                             bool recurse) {
    struct bx_tar_select_member* slot;

    if (plan->len == plan->cap) {
        size_t next_cap = plan->cap ? plan->cap * 2u : 8u;
        plan->members = xrealloc(plan->members, next_cap * sizeof(*plan->members));
        plan->cap = next_cap;
    }

    slot = &plan->members[plan->len++];
    slot->name = xstrdup(name);
    slot->extract_dir = extract_dir ? xstrdup(extract_dir) : NULL;
    slot->policy = *policy;
    slot->recurse = recurse;
    return true;
}

static char* bx_tar_select_resolve_extract_dir(const char* base,
                                               const char* path) {
    if (path[0] == '/' || base == NULL) {
        return xstrdup(path);
    }
    return bx_path_join(base, path);
}

static bool bx_tar_select_set_extract_dir(struct bx_tar_select_state* state,
                                          const char* path) {
    char* next = bx_tar_select_resolve_extract_dir(state->extract_dir, path);
    free(state->extract_dir);
    state->extract_dir = next;
    return true;
}

static bool bx_tar_select_append_name_list(struct bx_tar_match_pattern_list* dest,
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

static bool bx_tar_select_add_exclude_from_path(struct bx_tar_select_state* state,
                                                const char* path) {
    struct bx_archive_name_list loaded = {0};
    bool ok = bx_archive_name_list_read_path(path, '\n', &loaded, state->diag);

    if (!ok) {
        bx_archive_name_list_free(&loaded);
        return false;
    }
    ok = bx_tar_select_append_name_list(&state->plan->exclude_patterns,
                                        &loaded,
                                        &state->exclude_policy);
    bx_archive_name_list_free(&loaded);
    return ok;
}

static bool bx_tar_select_note_option_error(struct bx_tar_select_state* state,
                                            const char* list_path,
                                            size_t record_no,
                                            const char* message) {
    bx_tar_files_from_report_option_error(state->diag, list_path, record_no, message);
    state->had_errors = true;
    return true;
}

static bool bx_tar_select_process_files_from(struct bx_tar_select_state* state,
                                             const char* list_path,
                                             const char* display_path);

static bool bx_tar_select_process_option_record(struct bx_tar_select_state* state,
                                                const char* record,
                                                const char* list_path,
                                                size_t record_no) {
    const char* arg = NULL;
    char* decoded = NULL;

    if (strcmp(record, "--null") == 0) {
        state->separator = '\0';
        state->verbatim = true;
        return true;
    }
    if (strcmp(record, "--no-null") == 0) {
        state->separator = '\n';
        state->verbatim = false;
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

    if (strncmp(record, "--exclude=", strlen("--exclude=")) == 0) {
        arg = record + strlen("--exclude=");
    }
    else if (strncmp(record, "--exclude", strlen("--exclude")) == 0) {
        const char* rest = record + strlen("--exclude");
        if (*rest == '\0' || (*rest != ' ' && *rest != '\t')) {
            return bx_tar_select_note_option_error(state, list_path, record_no, "unrecognized option");
        }
        arg = bx_tar_files_from_skip_inline_space(rest);
    }

    if (arg != NULL) {
        bool ok;

        decoded = bx_tar_files_from_decode_text(state->verbatim, state->unquote, arg);
        ok = bx_tar_match_pattern_list_append(&state->plan->exclude_patterns,
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
        if (*rest == '\0' || (*rest != ' ' && *rest != '\t')) {
            return bx_tar_select_note_option_error(state, list_path, record_no, "unrecognized option");
        }
        arg = bx_tar_files_from_skip_inline_space(rest);
    }
    else if (record[0] == '-' && record[1] == 'X') {
        arg = record + 2u;
        if (*arg == ' ' || *arg == '\t') {
            arg = bx_tar_files_from_skip_inline_space(arg);
        }
        if (*arg == '\0') {
            return bx_tar_select_note_option_error(state, list_path, record_no, "unrecognized option");
        }
    }

    if (arg != NULL) {
        bool ok;

        decoded = bx_tar_files_from_decode_text(state->verbatim, state->unquote, arg);
        ok = bx_tar_select_add_exclude_from_path(state, decoded);
        free(decoded);
        return ok;
    }

    arg = NULL;
    if (strncmp(record, "--directory=", strlen("--directory=")) == 0) {
        arg = record + strlen("--directory=");
    }
    else if (strncmp(record, "--directory", strlen("--directory")) == 0) {
        const char* rest = record + strlen("--directory");
        if (*rest == '\0' || (*rest != ' ' && *rest != '\t')) {
            return bx_tar_select_note_option_error(state, list_path, record_no, "unrecognized option");
        }
        arg = bx_tar_files_from_skip_inline_space(rest);
    }
    else if (record[0] == '-' && record[1] == 'C') {
        arg = record + 2u;
        if (*arg == ' ' || *arg == '\t') {
            arg = bx_tar_files_from_skip_inline_space(arg);
        }
        if (*arg == '\0') {
            return bx_tar_select_note_option_error(state, list_path, record_no, "unrecognized option");
        }
    }

    if (arg != NULL) {
        decoded = bx_tar_files_from_decode_text(state->verbatim, state->unquote, arg);
        bx_tar_select_set_extract_dir(state, decoded);
        free(decoded);
        return true;
    }

    arg = NULL;
    if (strncmp(record, "--files-from=", strlen("--files-from=")) == 0) {
        arg = record + strlen("--files-from=");
    }
    else if (strncmp(record, "--files-from", strlen("--files-from")) == 0) {
        const char* rest = record + strlen("--files-from");
        if (*rest == '\0' || (*rest != ' ' && *rest != '\t')) {
            return bx_tar_select_note_option_error(state, list_path, record_no, "unrecognized option");
        }
        arg = bx_tar_files_from_skip_inline_space(rest);
    }
    else if (record[0] == '-' && record[1] == 'T') {
        arg = record + 2u;
        if (*arg == ' ' || *arg == '\t') {
            arg = bx_tar_files_from_skip_inline_space(arg);
        }
        if (*arg == '\0') {
            return bx_tar_select_note_option_error(state, list_path, record_no, "unrecognized option");
        }
    }

    if (arg != NULL) {
        bool ok;

        decoded = bx_tar_files_from_decode_text(state->verbatim, state->unquote, arg);
        ok = bx_tar_select_process_files_from(state, decoded, decoded);
        free(decoded);
        return ok;
    }

    return bx_tar_select_note_option_error(state, list_path, record_no, "unrecognized option");
}

static bool bx_tar_select_add_member_record(struct bx_tar_select_state* state,
                                            const char* record) {
    char* decoded = bx_tar_files_from_decode_text(state->verbatim, state->unquote, record);
    bool ok = bx_tar_select_plan_append_member(state->plan,
                                               decoded,
                                               state->extract_dir,
                                               &state->member_policy,
                                               state->recurse);
    free(decoded);
    return ok;
}

static bool bx_tar_select_process_record(struct bx_tar_select_state* state,
                                         const char* record,
                                         const char* list_path,
                                         size_t record_no) {
    if (record[0] == '\0') {
        return true;
    }
    if (!state->verbatim && record[0] == '-') {
        return bx_tar_select_process_option_record(state, record, list_path, record_no);
    }
    return bx_tar_select_add_member_record(state, record);
}

static bool bx_tar_select_process_files_from(struct bx_tar_select_state* state,
                                             const char* list_path,
                                             const char* display_path) {
    struct bx_archive_buffer input = {0};
    size_t pos = 0u;
    size_t record_no = 0u;

    if (!bx_tar_files_from_read_buffer(list_path, &input, state->diag)) {
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

        ok = bx_tar_select_process_record(state, record, display_path, record_no);
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

void bx_tar_select_plan_cleanup(struct bx_tar_select_plan* plan) {
    size_t i;

    for (i = 0u; i < plan->len; i++) {
        free(plan->members[i].name);
        free(plan->members[i].extract_dir);
    }
    free(plan->members);
    plan->members = NULL;
    plan->len = 0u;
    plan->cap = 0u;
    free(plan->default_extract_dir);
    plan->default_extract_dir = NULL;
    bx_tar_match_pattern_list_free(&plan->exclude_patterns);
}

bool bx_tar_select_plan_build(struct bx_tar_select_plan* plan,
                              const struct bx_tar_create_options* directives,
                              bool* had_errors,
                              struct bx_diag_ctx* diag) {
    struct bx_tar_select_state state = {
        .extract_dir = NULL,
        .separator = '\n',
        .verbatim = false,
        .unquote = true,
        .recurse = true,
        .member_policy = {0},
        .exclude_policy = {0},
        .had_errors = false,
        .plan = plan,
        .diag = diag,
    };
    size_t i;

    memset(plan, 0, sizeof(*plan));
    bx_tar_match_policy_init_member_default(&state.member_policy);
    bx_tar_match_policy_init_exclude_default(&state.exclude_policy);
    for (i = 0u; i < directives->directives.len; i++) {
        const struct bx_tar_create_directive* directive = &directives->directives.items[i];

        switch (directive->kind) {
            case BX_TAR_CREATE_DIRECTIVE_CHDIR:
                if (!bx_tar_select_set_extract_dir(&state, directive->text)) {
                    goto fail;
                }
                break;
            case BX_TAR_CREATE_DIRECTIVE_ADD_PATH:
                if (!bx_tar_select_plan_append_member(plan,
                                                      directive->text,
                                                      state.extract_dir,
                                                      &state.member_policy,
                                                      state.recurse)) {
                    goto fail;
                }
                break;
            case BX_TAR_CREATE_DIRECTIVE_EXCLUDE_PATTERN:
                if (!bx_tar_match_pattern_list_append(&plan->exclude_patterns,
                                                      directive->text,
                                                      &state.exclude_policy)) {
                    goto fail;
                }
                break;
            case BX_TAR_CREATE_DIRECTIVE_EXCLUDE_FROM:
                if (!bx_tar_select_add_exclude_from_path(&state, directive->text)) {
                    goto fail;
                }
                break;
            case BX_TAR_CREATE_DIRECTIVE_RECURSE_ON:
                state.recurse = true;
                break;
            case BX_TAR_CREATE_DIRECTIVE_RECURSE_OFF:
                state.recurse = false;
                break;
            case BX_TAR_CREATE_DIRECTIVE_FILES_FROM:
                if (!bx_tar_select_process_files_from(&state, directive->text, directive->text)) {
                    goto fail;
                }
                break;
            case BX_TAR_CREATE_DIRECTIVE_FILES_FROM_NULL_ON:
                state.separator = '\0';
                state.verbatim = true;
                break;
            case BX_TAR_CREATE_DIRECTIVE_FILES_FROM_NULL_OFF:
                state.separator = '\n';
                state.verbatim = false;
                break;
            case BX_TAR_CREATE_DIRECTIVE_FILES_FROM_VERBATIM_ON:
                state.verbatim = true;
                break;
            case BX_TAR_CREATE_DIRECTIVE_FILES_FROM_VERBATIM_OFF:
                state.verbatim = false;
                break;
            case BX_TAR_CREATE_DIRECTIVE_FILES_FROM_UNQUOTE_ON:
                state.unquote = true;
                break;
            case BX_TAR_CREATE_DIRECTIVE_FILES_FROM_UNQUOTE_OFF:
                state.unquote = false;
                break;
            case BX_TAR_CREATE_DIRECTIVE_ANCHORED_ON:
                bx_tar_match_policy_set_anchored(&state.member_policy, &state.exclude_policy, true);
                break;
            case BX_TAR_CREATE_DIRECTIVE_ANCHORED_OFF:
                bx_tar_match_policy_set_anchored(&state.member_policy, &state.exclude_policy, false);
                break;
            case BX_TAR_CREATE_DIRECTIVE_IGNORE_CASE_ON:
                bx_tar_match_policy_set_ignore_case(&state.member_policy, &state.exclude_policy, true);
                break;
            case BX_TAR_CREATE_DIRECTIVE_IGNORE_CASE_OFF:
                bx_tar_match_policy_set_ignore_case(&state.member_policy, &state.exclude_policy, false);
                break;
            case BX_TAR_CREATE_DIRECTIVE_WILDCARDS_ON:
                bx_tar_match_policy_set_wildcards(&state.member_policy, &state.exclude_policy, true);
                break;
            case BX_TAR_CREATE_DIRECTIVE_WILDCARDS_OFF:
                bx_tar_match_policy_set_wildcards(&state.member_policy, &state.exclude_policy, false);
                break;
            case BX_TAR_CREATE_DIRECTIVE_WILDCARDS_MATCH_SLASH_ON:
                bx_tar_match_policy_set_wildcards_match_slash(&state.member_policy,
                                                              &state.exclude_policy,
                                                              true);
                break;
            case BX_TAR_CREATE_DIRECTIVE_WILDCARDS_MATCH_SLASH_OFF:
                bx_tar_match_policy_set_wildcards_match_slash(&state.member_policy,
                                                              &state.exclude_policy,
                                                              false);
                break;
            case BX_TAR_CREATE_DIRECTIVE_EXCLUDE_CACHES:
            case BX_TAR_CREATE_DIRECTIVE_EXCLUDE_CACHES_ALL:
            case BX_TAR_CREATE_DIRECTIVE_EXCLUDE_CACHES_UNDER:
            case BX_TAR_CREATE_DIRECTIVE_EXCLUDE_IGNORE:
            case BX_TAR_CREATE_DIRECTIVE_EXCLUDE_IGNORE_RECURSIVE:
            case BX_TAR_CREATE_DIRECTIVE_EXCLUDE_TAG:
            case BX_TAR_CREATE_DIRECTIVE_EXCLUDE_TAG_ALL:
            case BX_TAR_CREATE_DIRECTIVE_EXCLUDE_TAG_UNDER:
            case BX_TAR_CREATE_DIRECTIVE_EXCLUDE_VCS:
            case BX_TAR_CREATE_DIRECTIVE_EXCLUDE_VCS_IGNORES:
                break;
        }
    }

    plan->default_extract_dir = state.extract_dir ? xstrdup(state.extract_dir) : NULL;
    if (had_errors != NULL) {
        *had_errors = state.had_errors;
    }
    free(state.extract_dir);
    return true;

fail:
    free(state.extract_dir);
    bx_tar_select_plan_cleanup(plan);
    return false;
}

bool bx_tar_select_member_matches_name(const struct bx_tar_select_member* member,
                                       const char* name) {
    return bx_tar_match_member_name(member->name, &member->policy, member->recurse, name);
}

bool bx_tar_select_plan_match(const struct bx_tar_select_plan* plan,
                              const char* name,
                              bool default_select_all,
                              bool* matched_members,
                              const char** extract_dir_out) {
    size_t i;
    const char* extract_dir = plan->default_extract_dir;
    bool selected = default_select_all;

    for (i = 0u; i < plan->len; i++) {
        if (bx_tar_select_member_matches_name(&plan->members[i], name)) {
            if (matched_members != NULL) {
                matched_members[i] = true;
            }
            if (!selected) {
                selected = true;
                extract_dir = plan->members[i].extract_dir;
            }
        }
    }

    if (!selected || bx_tar_path_excluded(&plan->exclude_patterns, name)) {
        if (extract_dir_out != NULL) {
            *extract_dir_out = NULL;
        }
        return false;
    }

    if (extract_dir_out != NULL) {
        *extract_dir_out = extract_dir;
    }
    return true;
}

bool bx_tar_select_plan_report_unmatched(const struct bx_tar_select_plan* plan,
                                         const bool* matched_members,
                                         const struct bx_diag_ctx* diag) {
    bool had_errors = false;
    size_t i;

    if (matched_members == NULL) {
        return false;
    }

    for (i = 0u; i < plan->len; i++) {
        if (matched_members[i]) {
            continue;
        }
        fprintf(stderr, "%s: %s: Not found in archive\n", diag->progname, plan->members[i].name);
        had_errors = true;
    }

    return had_errors;
}
