#define _GNU_SOURCE
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "argv_packer.h"

extern char **environ;

static size_t bx_argv_pointer_bytes(int argc) {
    if (argc < 0)
        return 0;
    return (size_t)(argc + 1) * sizeof(char *);
}

size_t bx_argv_environment_bytes(void) {
    size_t env_bytes = 0;
    if (!environ)
        return 0;

    for (char **ep = environ; *ep; ep++)
        env_bytes += strlen(*ep) + 1;
    return env_bytes;
}

size_t bx_argv_bytes(char **argv) {
    size_t total = 0;
    if (!argv)
        return 0;

    int argc = 0;
    for (int i = 0; argv[i]; i++) {
        total += strlen(argv[i]) + 1;
        argc++;
    }
    total += bx_argv_pointer_bytes(argc);
    return total;
}

size_t bx_argv_bytes_with_items(const char *const *base_argv, int base_argc,
                                char **items, int start, int count) {
    size_t total = 0;
    for (int i = 0; i < base_argc; i++)
        total += strlen(base_argv[i]) + 1;
    for (int i = 0; i < count; i++)
        total += strlen(items[start + i]) + 1;
    total += bx_argv_pointer_bytes(base_argc + count);
    return total;
}

static bool bx_argv_push_owned(char ***argvp, int *argc, int *cap, char *arg) {
    if (*argc + 1 >= *cap) {
        int new_cap = *cap == 0 ? 8 : *cap * 2;
        char **tmp = realloc(*argvp, (size_t)new_cap * sizeof(*tmp));
        if (!tmp) {
            free(arg);
            return false;
        }
        *argvp = tmp;
        *cap = new_cap;
    }

    (*argvp)[(*argc)++] = arg;
    (*argvp)[*argc] = NULL;
    return true;
}

void bx_argv_free(char **argv) {
    if (!argv)
        return;
    for (int i = 0; argv[i]; i++)
        free(argv[i]);
    free(argv);
}

char **bx_argv_build_with_item_expansion(const char *const *base_argv, int base_argc,
                                         char **items, int start, int count,
                                         int batch_mode,
                                         bx_argv_marker_count_fn marker_count_fn,
                                         bx_argv_expand_arg_fn expand_arg_fn,
                                         int *saw_marker,
                                         void *user) {
    char **argv = NULL;
    int argc = 0;
    int cap = 0;
    int saw = 0;

    for (int i = 0; i < base_argc; i++) {
        size_t marker_count = marker_count_fn ? marker_count_fn(base_argv[i], user) : 0;
        if (marker_count == 0) {
            if (!bx_argv_push_owned(&argv, &argc, &cap, strdup(base_argv[i])))
                goto fail;
            continue;
        }

        saw = 1;
        int item_total = batch_mode ? count : (count > 0 ? 1 : 0);
        for (int j = 0; j < item_total; j++) {
            if (!expand_arg_fn)
                goto fail;
            char *expanded = expand_arg_fn(base_argv[i], items[start + j], user);
            if (!expanded || !bx_argv_push_owned(&argv, &argc, &cap, expanded))
                goto fail;
        }
    }

    if (!saw) {
        int item_total = batch_mode ? count : (count > 0 ? 1 : 0);
        for (int j = 0; j < item_total; j++) {
            if (!bx_argv_push_owned(&argv, &argc, &cap, strdup(items[start + j])))
                goto fail;
        }
    }

    if (!argv) {
        argv = calloc(1, sizeof(*argv));
        if (!argv)
            return NULL;
    }

    if (saw_marker)
        *saw_marker = saw;
    return argv;

fail:
    bx_argv_free(argv);
    return NULL;
}

size_t bx_argv_bytes_with_item_expansion(const char *const *base_argv, int base_argc,
                                         char **items, int start, int count,
                                         int batch_mode,
                                         bx_argv_marker_count_fn marker_count_fn,
                                         bx_argv_expand_bytes_fn expand_bytes_fn,
                                         int *saw_marker,
                                         void *user) {
    size_t total = 0;
    int argc = 0;
    int saw = 0;

    for (int i = 0; i < base_argc; i++) {
        size_t marker_count = marker_count_fn ? marker_count_fn(base_argv[i], user) : 0;
        if (marker_count == 0) {
            size_t arg_bytes = expand_bytes_fn
                                   ? expand_bytes_fn(base_argv[i], "", user)
                                   : strlen(base_argv[i]) + 1;
            if (arg_bytes == (size_t)-1)
                return (size_t)-1;
            total += arg_bytes;
            argc++;
            continue;
        }

        saw = 1;
        int item_total = batch_mode ? count : (count > 0 ? 1 : 0);
        for (int j = 0; j < item_total; j++) {
            if (!expand_bytes_fn)
                return (size_t)-1;
            size_t arg_bytes = expand_bytes_fn(base_argv[i], items[start + j], user);
            if (arg_bytes == (size_t)-1)
                return (size_t)-1;
            total += arg_bytes;
            argc++;
        }
    }

    if (!saw) {
        int item_total = batch_mode ? count : (count > 0 ? 1 : 0);
        for (int j = 0; j < item_total; j++) {
            total += strlen(items[start + j]) + 1;
            argc++;
        }
    }

    total += bx_argv_pointer_bytes(argc);
    if (saw_marker)
        *saw_marker = saw;
    return total;
}

size_t bx_argv_effective_char_limit(int max_chars) {
    long arg_max = sysconf(_SC_ARG_MAX);
    size_t sys_limit = 0;
    if (arg_max > 0) {
        size_t env_bytes = bx_argv_environment_bytes();
        if ((size_t)arg_max > env_bytes)
            sys_limit = (size_t)arg_max - env_bytes;
    }

    if (max_chars > 0 && sys_limit > 0)
        return (size_t)max_chars < sys_limit ? (size_t)max_chars : sys_limit;
    if (max_chars > 0)
        return (size_t)max_chars;
    return sys_limit;
}

int bx_argv_select_batch_count_by_bytes(int item_count, int start,
                                        int max_args, int max_lines,
                                        size_t char_limit,
                                        bx_argv_batch_bytes_fn bytes_fn,
                                        void *user) {
    if (!bytes_fn || start < 0 || start > item_count)
        return -1;

    int capped_args = max_args > 0 ? max_args : (item_count - start);
    int capped_lines = max_lines > 0 ? max_lines : (item_count - start);
    int limit = capped_args < capped_lines ? capped_args : capped_lines;
    int take = 0;

    while (start + take < item_count && take < limit) {
        size_t bytes = bytes_fn(user, start, take + 1);
        if (bytes == (size_t)-1)
            return -1;
        if (char_limit > 0 && bytes > char_limit)
            break;
        take++;
    }

    return take > 0 ? take : -1;
}

struct bx_argv_select_batch_ctx {
    const char *const *base_argv;
    int base_argc;
    char **items;
};

static size_t bx_argv_select_batch_ctx_bytes(void *user, int start, int count) {
    struct bx_argv_select_batch_ctx *ctx = user;
    return bx_argv_bytes_with_items(ctx->base_argv, ctx->base_argc,
                                    ctx->items, start, count);
}

int bx_argv_select_batch_count(const char *const *base_argv, int base_argc,
                               char **items, const int *line_groups,
                               int item_count, int start,
                               int max_args, int max_lines,
                               size_t char_limit) {
    int capped_args = max_args > 0 ? max_args : (item_count - start);
    int take = 0;
    int used_lines = 0;
    int last_group = -1;

    while (start + take < item_count && take < capped_args) {
        int group = line_groups ? line_groups[start + take] : (start + take);
        if (take == 0 || group != last_group) {
            int capped_lines = max_lines > 0 ? max_lines : (item_count - start);
            if (used_lines >= capped_lines)
                break;
            used_lines++;
            last_group = group;
        }
        struct bx_argv_select_batch_ctx ctx = {
            .base_argv = base_argv,
            .base_argc = base_argc,
            .items = items,
        };
        int next_take = bx_argv_select_batch_count_by_bytes(
            item_count, start, take + 1, used_lines, char_limit,
            bx_argv_select_batch_ctx_bytes, &ctx);
        if (next_take < 0 || next_take == take)
            break;
        take = next_take;
    }

    return take > 0 ? take : -1;
}
