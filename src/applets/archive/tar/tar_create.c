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
    struct bx_archive_name_list exclude_patterns;
};

struct bx_tar_create_filter_state {
    const struct bx_archive_name_list* exclude_patterns;
    const struct bx_diag_ctx* diag;
    bool* had_create_errors;
};

struct bx_tar_create_collect_ctx {
    struct bx_archive_fs_list* list;
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

static bool bx_tar_create_append_name_list(struct bx_archive_name_list* dest,
                                           const struct bx_archive_name_list* src) {
    size_t i;

    for (i = 0u; i < src->len; i++) {
        if (!bx_archive_name_list_append(dest, src->items[i])) {
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
    ok = bx_tar_create_append_name_list(&state->exclude_patterns, &loaded);
    bx_archive_name_list_free(&loaded);
    return ok;
}

static bool bx_tar_create_include_path(const char* source_path,
                                       const char* archive_path,
                                       const struct stat* st,
                                       void* user_data) {
    const struct bx_tar_create_filter_state* state = user_data;

    (void)source_path;
    (void)st;
    return !bx_tar_path_excluded(state->exclude_patterns, archive_path);
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

static bool bx_tar_create_add_path(struct bx_tar_create_collect_ctx* ctx,
                                   const struct bx_tar_files_from_state* state,
                                   const char* name) {
    struct bx_tar_create_filter_state filter_state = {
        .exclude_patterns = &state->exclude_patterns,
        .diag = ctx->diag,
        .had_create_errors = &ctx->had_create_errors,
    };
    char* source_path = bx_tar_create_resolve_input_path(state->cwd, name);
    bool ok = bx_archive_fs_add_path_filtered(ctx->list,
                                              source_path,
                                              name,
                                              state->recurse,
                                              ctx->sort_children,
                                              bx_tar_create_include_path,
                                              &filter_state,
                                              bx_tar_create_handle_fs_error,
                                              &filter_state,
                                              ctx->diag);

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
        ok = bx_archive_name_list_append(&state->exclude_patterns, decoded);
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
            return bx_archive_name_list_append(&state->exclude_patterns, directive->text);
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
    }

    return true;
}

bool bx_tar_create_collect_fs_entries(struct bx_archive_fs_list* list,
                                      const struct bx_tar_create_options* create_options,
                                      bool sort_children,
                                      bool* had_create_errors,
                                      struct bx_diag_ctx* diag) {
    struct bx_tar_files_from_state state = {
        .cwd = NULL,
        .separator = '\n',
        .verbatim = false,
        .unquote = true,
        .recurse = true,
        .exclude_patterns = {0},
    };
    struct bx_tar_create_collect_ctx ctx = {
        .list = list,
        .sort_children = sort_children,
        .had_create_errors = false,
        .diag = diag,
    };
    size_t i;
    bool ok = true;

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
    bx_archive_name_list_free(&state.exclude_patterns);
    free(state.cwd);
    return ok;
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
