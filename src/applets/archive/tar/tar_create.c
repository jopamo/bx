#include <errno.h>
#include <fnmatch.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "applets/archive/tar/tar_create.h"
#include "bx/libbx.h"
#include "lib/path_ops.h"

static void bx_tar_name_source_list_free(struct bx_tar_name_source_list* list) {
    size_t i;

    for (i = 0u; i < list->len; i++) {
        free(list->items[i].path);
        free(list->items[i].cwd);
    }
    free(list->items);
    list->items = NULL;
    list->len = 0u;
    list->cap = 0u;
}

static void bx_tar_files_from_source_list_free(struct bx_tar_files_from_source_list* list) {
    size_t i;

    for (i = 0u; i < list->len; i++) {
        free(list->items[i].path);
        free(list->items[i].cwd);
    }
    free(list->items);
    list->items = NULL;
    list->len = 0u;
    list->cap = 0u;
}

void bx_tar_create_options_cleanup(struct bx_tar_create_options* options) {
    bx_archive_name_list_free(&options->exclude_patterns);
    bx_tar_name_source_list_free(&options->exclude_from_sources);
    bx_tar_files_from_source_list_free(&options->files_from_sources);
}

static bool bx_tar_name_source_list_append(struct bx_tar_name_source_list* list,
                                           const char* path,
                                           const char* cwd) {
    struct bx_tar_name_source* slot;

    if (list->len == list->cap) {
        size_t next_cap = list->cap ? list->cap * 2u : 8u;
        list->items = xrealloc(list->items, next_cap * sizeof(*list->items));
        list->cap = next_cap;
    }

    slot = &list->items[list->len++];
    slot->path = xstrdup(path);
    slot->cwd = cwd ? xstrdup(cwd) : NULL;
    return true;
}

static bool bx_tar_files_from_source_list_append(struct bx_tar_files_from_source_list* list,
                                                 const char* path,
                                                 const char* cwd,
                                                 unsigned char separator) {
    struct bx_tar_files_from_source* slot;

    if (list->len == list->cap) {
        size_t next_cap = list->cap ? list->cap * 2u : 8u;
        list->items = xrealloc(list->items, next_cap * sizeof(*list->items));
        list->cap = next_cap;
    }

    slot = &list->items[list->len++];
    slot->path = xstrdup(path);
    slot->cwd = cwd ? xstrdup(cwd) : NULL;
    slot->separator = separator;
    return true;
}

bool bx_tar_create_options_add_exclude_pattern(struct bx_tar_create_options* options,
                                               const char* pattern) {
    return bx_archive_name_list_append(&options->exclude_patterns, pattern);
}

bool bx_tar_create_options_add_exclude_from(struct bx_tar_create_options* options,
                                            const char* path,
                                            const char* cwd) {
    return bx_tar_name_source_list_append(&options->exclude_from_sources, path, cwd);
}

bool bx_tar_create_options_add_files_from(struct bx_tar_create_options* options,
                                          const char* path,
                                          const char* cwd) {
    return bx_tar_files_from_source_list_append(
        &options->files_from_sources,
        path,
        cwd,
        options->files_from_separator
    );
}

static char* bx_tar_create_resolve_input_path(const char* cwd, const char* path) {
    if (path[0] == '/' || cwd == NULL) {
        return xstrdup(path);
    }
    return bx_path_join(cwd, path);
}

static char* bx_tar_create_resolve_list_path(const char* cwd, const char* path) {
    if (strcmp(path, "-") == 0) {
        return xstrdup(path);
    }
    return bx_tar_create_resolve_input_path(cwd, path);
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

struct bx_tar_create_input {
    char* name;
    char* cwd;
};

struct bx_tar_create_input_list {
    struct bx_tar_create_input* items;
    size_t len;
    size_t cap;
};

static void bx_tar_create_input_list_free(struct bx_tar_create_input_list* list) {
    size_t i;

    for (i = 0u; i < list->len; i++) {
        free(list->items[i].name);
        free(list->items[i].cwd);
    }
    free(list->items);
    list->items = NULL;
    list->len = 0u;
    list->cap = 0u;
}

static bool bx_tar_create_input_list_append(struct bx_tar_create_input_list* list,
                                            const char* name,
                                            const char* cwd) {
    struct bx_tar_create_input* slot;

    if (list->len == list->cap) {
        size_t next_cap = list->cap ? list->cap * 2u : 16u;
        list->items = xrealloc(list->items, next_cap * sizeof(*list->items));
        list->cap = next_cap;
    }

    slot = &list->items[list->len++];
    slot->name = xstrdup(name);
    slot->cwd = cwd ? xstrdup(cwd) : NULL;
    return true;
}

static bool bx_tar_create_load_names_from_sources(struct bx_tar_create_input_list* inputs,
                                                  const struct bx_tar_files_from_source_list* sources,
                                                  struct bx_diag_ctx* diag) {
    size_t i;

    for (i = 0u; i < sources->len; i++) {
        struct bx_archive_name_list loaded = {0};
        char* resolved = bx_tar_create_resolve_list_path(sources->items[i].cwd, sources->items[i].path);
        bool ok = bx_archive_name_list_read_path(
            resolved,
            sources->items[i].separator,
            &loaded,
            diag
        );
        free(resolved);
        if (!ok) {
            bx_archive_name_list_free(&loaded);
            return false;
        }
        for (size_t j = 0u; j < loaded.len; j++) {
            if (!bx_tar_create_input_list_append(inputs, loaded.items[j], sources->items[i].cwd)) {
                bx_archive_name_list_free(&loaded);
                return false;
            }
        }
        bx_archive_name_list_free(&loaded);
    }

    return true;
}

static bool bx_tar_create_load_exclude_patterns(struct bx_archive_name_list* patterns,
                                                const struct bx_tar_create_options* create_options,
                                                struct bx_diag_ctx* diag) {
    size_t i;

    if (!bx_tar_create_append_name_list(patterns, &create_options->exclude_patterns)) {
        return false;
    }

    for (i = 0u; i < create_options->exclude_from_sources.len; i++) {
        struct bx_archive_name_list loaded = {0};
        char* resolved = bx_tar_create_resolve_list_path(
            create_options->exclude_from_sources.items[i].cwd,
            create_options->exclude_from_sources.items[i].path
        );
        bool ok = bx_archive_name_list_read_path(resolved, '\n', &loaded, diag);
        free(resolved);
        if (!ok) {
            bx_archive_name_list_free(&loaded);
            return false;
        }
        if (!bx_tar_create_append_name_list(patterns, &loaded)) {
            bx_archive_name_list_free(&loaded);
            return false;
        }
        bx_archive_name_list_free(&loaded);
    }

    return true;
}

static bool bx_tar_create_match_exclude_pattern(const char* pattern, const char* archive_path) {
    int flags = FNM_PATHNAME;

    if (strchr(pattern, '/') == NULL) {
        return fnmatch(pattern, bx_path_basename_ptr(archive_path), 0) == 0;
    }

    if (fnmatch(pattern, archive_path, flags) == 0) {
        return true;
    }

    for (const char* cursor = archive_path; *cursor != '\0'; cursor++) {
        if (*cursor == '/' && fnmatch(pattern, cursor + 1, flags) == 0) {
            return true;
        }
    }

    return false;
}

static bool bx_tar_create_path_excluded(const struct bx_archive_name_list* patterns,
                                        const char* archive_path) {
    size_t i;

    for (i = 0u; i < patterns->len; i++) {
        if (bx_tar_create_match_exclude_pattern(patterns->items[i], archive_path)) {
            return true;
        }
    }
    return false;
}

struct bx_tar_create_filter_state {
    const struct bx_archive_name_list* exclude_patterns;
    const struct bx_diag_ctx* diag;
    bool had_create_errors;
};

static bool bx_tar_create_include_path(const char* source_path,
                                       const char* archive_path,
                                       const struct stat* st,
                                       void* user_data) {
    const struct bx_tar_create_filter_state* state = user_data;

    (void)source_path;
    (void)st;
    return !bx_tar_create_path_excluded(state->exclude_patterns, archive_path);
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
    state->had_create_errors = true;
    return BX_ARCHIVE_FS_ERROR_SKIP;
}

bool bx_tar_create_collect_fs_entries(struct bx_archive_fs_list* list,
                                      const struct bx_tar_create_options* create_options,
                                      const char* create_cwd,
                                      int argc,
                                      char** argv,
                                      int operand_index,
                                      bool sort_children,
                                      bool* had_create_errors,
                                      struct bx_diag_ctx* diag) {
    struct bx_tar_create_input_list inputs = {0};
    struct bx_archive_name_list exclude_patterns = {0};
    struct bx_tar_create_filter_state filter_state = {
        .exclude_patterns = NULL,
        .diag = diag,
        .had_create_errors = false,
    };
    int i;

    if (!bx_tar_create_load_names_from_sources(&inputs, &create_options->files_from_sources, diag)) {
        bx_tar_create_input_list_free(&inputs);
        return false;
    }

    for (i = operand_index; i < argc; i++) {
        if (!bx_tar_create_input_list_append(&inputs, argv[i], create_cwd)) {
            bx_tar_create_input_list_free(&inputs);
            return false;
        }
    }

    if (!bx_tar_create_load_exclude_patterns(&exclude_patterns, create_options, diag)) {
        bx_tar_create_input_list_free(&inputs);
        bx_archive_name_list_free(&exclude_patterns);
        return false;
    }

    filter_state.exclude_patterns = &exclude_patterns;
    for (i = 0; (size_t)i < inputs.len; i++) {
        char* source_path = bx_tar_create_resolve_input_path(inputs.items[i].cwd, inputs.items[i].name);
        bool ok = bx_archive_fs_add_path_filtered(
            list,
            source_path,
            inputs.items[i].name,
            create_options->recurse,
            sort_children,
            bx_tar_create_include_path,
            &filter_state,
            bx_tar_create_handle_fs_error,
            &filter_state,
            diag
        );

        free(source_path);
        if (!ok) {
            bx_tar_create_input_list_free(&inputs);
            bx_archive_name_list_free(&exclude_patterns);
            return false;
        }
    }

    bx_tar_create_input_list_free(&inputs);
    bx_archive_name_list_free(&exclude_patterns);
    if (had_create_errors != NULL) {
        *had_create_errors = filter_state.had_create_errors;
    }
    return true;
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
