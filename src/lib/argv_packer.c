#define _GNU_SOURCE
#include <limits.h>
#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "argv_packer.h"

extern char **environ;

static bool bx_argv_add_size(size_t *total, size_t value) {
    if (value > SIZE_MAX - *total)
        return false;
    *total += value;
    return true;
}

static bool bx_argv_string_bytes(const char *arg, size_t *bytes) {
    size_t len = strlen(arg);
    if (len == SIZE_MAX)
        return false;
    *bytes = len + 1u;
    return true;
}

static bool bx_argv_pointer_bytes_checked(int argc, size_t *bytes) {
    if (argc < 0)
        return false;
    if ((size_t)argc > SIZE_MAX / sizeof(char *))
        return false;
    *bytes = (size_t)argc * sizeof(char *);
    return true;
}

static size_t bx_argv_pointer_bytes(int argc) {
    size_t bytes = 0;
    if (!bx_argv_pointer_bytes_checked(argc, &bytes))
        return (size_t)-1;
    return bytes;
}

static bool bx_argv_count_one(int *argc) {
    if (*argc == INT_MAX)
        return false;
    (*argc)++;
    return true;
}

size_t bx_argv_environment_bytes(void) {
    size_t env_bytes = 0;
    if (!environ)
        return 0;

    for (char **ep = environ; *ep; ep++) {
        size_t arg_bytes = 0;
        if (!bx_argv_string_bytes(*ep, &arg_bytes) ||
            !bx_argv_add_size(&env_bytes, arg_bytes))
            return (size_t)-1;
    }
    return env_bytes;
}

size_t bx_argv_bytes(char **argv) {
    size_t total = 0;
    if (!argv)
        return 0;

    int argc = 0;
    for (int i = 0; argv[i]; i++) {
        size_t arg_bytes = 0;
        if (!bx_argv_string_bytes(argv[i], &arg_bytes) ||
            !bx_argv_add_size(&total, arg_bytes) ||
            !bx_argv_count_one(&argc))
            return (size_t)-1;
    }
    size_t pointer_bytes = bx_argv_pointer_bytes(argc);
    if (pointer_bytes == (size_t)-1 ||
        !bx_argv_add_size(&total, pointer_bytes))
        return (size_t)-1;
    return total;
}

size_t bx_argv_bytes_with_items(const char *const *base_argv, int base_argc,
                                char **items, int start, int count) {
    size_t total = 0;
    int argc = 0;
    if (base_argc < 0 || start < 0 || count < 0)
        return (size_t)-1;
    for (int i = 0; i < base_argc; i++) {
        size_t arg_bytes = 0;
        if (!bx_argv_string_bytes(base_argv[i], &arg_bytes) ||
            !bx_argv_add_size(&total, arg_bytes) ||
            !bx_argv_count_one(&argc))
            return (size_t)-1;
    }
    for (int i = 0; i < count; i++) {
        if (start > INT_MAX - i)
            return (size_t)-1;
        size_t arg_bytes = 0;
        if (!bx_argv_string_bytes(items[start + i], &arg_bytes) ||
            !bx_argv_add_size(&total, arg_bytes) ||
            !bx_argv_count_one(&argc))
            return (size_t)-1;
    }
    size_t pointer_bytes = bx_argv_pointer_bytes(argc);
    if (pointer_bytes == (size_t)-1 ||
        !bx_argv_add_size(&total, pointer_bytes))
        return (size_t)-1;
    return total;
}

static bool bx_argv_push_owned(char ***argvp, int *argc, int *cap, char *arg) {
    if (*argc < 0 || *cap < 0 || *argc == INT_MAX) {
        free(arg);
        return false;
    }
    if (*argc + 1 >= *cap) {
        if (*cap > INT_MAX / 2) {
            free(arg);
            return false;
        }
        int new_cap = *cap == 0 ? 8 : *cap * 2;
        if ((size_t)new_cap > SIZE_MAX / sizeof(char *)) {
            free(arg);
            return false;
        }
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

static bool bx_argv_word_push_char(char **word, size_t *len, size_t *cap, char ch) {
    if (*len == SIZE_MAX)
        return false;
    if (*len + 1u >= *cap) {
        size_t new_cap = *cap == 0 ? 32u : *cap * 2u;
        if (new_cap <= *cap)
            return false;
        char *tmp = realloc(*word, new_cap);
        if (!tmp)
            return false;
        *word = tmp;
        *cap = new_cap;
    }
    (*word)[(*len)++] = ch;
    (*word)[*len] = '\0';
    return true;
}

int bx_argv_parse_command(const char *command, char ***argv_out) {
    char **argv = NULL;
    int argc = 0;
    int argv_cap = 0;
    const unsigned char *cursor = (const unsigned char *)command;

    if (!command || !argv_out)
        return -1;
    *argv_out = NULL;

    while (*cursor != '\0') {
        char *word = NULL;
        size_t word_len = 0;
        size_t word_cap = 0;
        unsigned char quote = '\0';
        bool started = false;

        while (isspace(*cursor))
            cursor++;
        if (*cursor == '\0')
            break;

        while (*cursor != '\0') {
            unsigned char ch = *cursor;

            if (quote == '\0' && isspace(ch))
                break;
            if (ch == '\'' && quote != '"') {
                quote = quote == '\'' ? '\0' : '\'';
                started = true;
                cursor++;
                continue;
            }
            if (ch == '"' && quote != '\'') {
                quote = quote == '"' ? '\0' : '"';
                started = true;
                cursor++;
                continue;
            }
            if (ch == '\\' && quote != '\'') {
                cursor++;
                if (*cursor == '\0')
                    goto invalid;
                ch = *cursor;
            }
            if (!bx_argv_word_push_char(&word, &word_len, &word_cap, (char)ch))
                goto invalid;
            started = true;
            cursor++;
        }

        if (quote != '\0' || !started)
            goto invalid;
        if (!word) {
            word = strdup("");
            if (!word)
                goto invalid;
        }
        if (!bx_argv_push_owned(&argv, &argc, &argv_cap, word))
            goto invalid;
        while (isspace(*cursor))
            cursor++;
        continue;

invalid:
        free(word);
        bx_argv_free(argv);
        return -1;
    }

    if (!argv || argc == 0) {
        bx_argv_free(argv);
        return -1;
    }
    *argv_out = argv;
    return 0;
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
    if (base_argc < 0 || start < 0 || count < 0)
        return NULL;

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
    if (base_argc < 0 || start < 0 || count < 0)
        return (size_t)-1;

    for (int i = 0; i < base_argc; i++) {
        size_t marker_count = marker_count_fn ? marker_count_fn(base_argv[i], user) : 0;
        if (marker_count == 0) {
            size_t arg_bytes = 0;
            if (expand_bytes_fn)
                arg_bytes = expand_bytes_fn(base_argv[i], "", user);
            else if (!bx_argv_string_bytes(base_argv[i], &arg_bytes))
                return (size_t)-1;
            if (arg_bytes == (size_t)-1)
                return (size_t)-1;
            if (!bx_argv_add_size(&total, arg_bytes) ||
                !bx_argv_count_one(&argc))
                return (size_t)-1;
            continue;
        }

        saw = 1;
        int item_total = batch_mode ? count : (count > 0 ? 1 : 0);
        for (int j = 0; j < item_total; j++) {
            if (start > INT_MAX - j)
                return (size_t)-1;
            if (!expand_bytes_fn)
                return (size_t)-1;
            size_t arg_bytes = expand_bytes_fn(base_argv[i], items[start + j], user);
            if (arg_bytes == (size_t)-1)
                return (size_t)-1;
            if (!bx_argv_add_size(&total, arg_bytes) ||
                !bx_argv_count_one(&argc))
                return (size_t)-1;
        }
    }

    if (!saw) {
        int item_total = batch_mode ? count : (count > 0 ? 1 : 0);
        for (int j = 0; j < item_total; j++) {
            if (start > INT_MAX - j)
                return (size_t)-1;
            size_t arg_bytes = 0;
            if (!bx_argv_string_bytes(items[start + j], &arg_bytes) ||
                !bx_argv_add_size(&total, arg_bytes) ||
                !bx_argv_count_one(&argc))
                return (size_t)-1;
        }
    }

    size_t pointer_bytes = bx_argv_pointer_bytes(argc);
    if (pointer_bytes == (size_t)-1 ||
        !bx_argv_add_size(&total, pointer_bytes))
        return (size_t)-1;
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
    if (!bytes_fn || item_count < 0 || start < 0 || start > item_count)
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
    if (base_argc < 0 || item_count < 0 || start < 0 || start > item_count)
        return -1;
    int capped_args = max_args > 0 ? max_args : (item_count - start);
    int capped_lines = max_lines > 0 ? max_lines : (item_count - start);
    int take = 0;
    int used_lines = 0;
    int last_group = -1;
    struct bx_argv_select_batch_ctx ctx = {
        .base_argv = base_argv,
        .base_argc = base_argc,
        .items = items,
    };

    while (start + take < item_count && take < capped_args) {
        int group = line_groups ? line_groups[start + take] : (start + take);
        bool new_group = (take == 0 || group != last_group);
        if (new_group && used_lines >= capped_lines)
            break;

        size_t bytes = bx_argv_select_batch_ctx_bytes(&ctx, start, take + 1);
        if (bytes == (size_t)-1)
            return -1;
        if (char_limit > 0 && bytes > char_limit)
            break;

        take++;
        if (new_group) {
            used_lines++;
            last_group = group;
        }
    }

    return take > 0 ? take : -1;
}
