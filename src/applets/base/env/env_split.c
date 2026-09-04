#include "applets/base/env/env_split.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lib/output_quote.h"
#include "lib/text_buffer.h"

struct bx_env_split_words {
    char **items;
    int count;
    int capacity;
};

static bool bx_env_split_push_word(
    struct bx_env_split_words *words,
    struct bx_text_buffer *word,
    struct bx_diag_ctx *diag) {
    if (words->count == INT_MAX) {
        errno = EOVERFLOW;
        bx_diag(diag, "memory exhausted");
        return false;
    }
    if (words->count == words->capacity) {
        int capacity = words->capacity == 0 ? 8 : words->capacity * 2;
        if (capacity <= words->capacity ||
            (size_t)capacity > SIZE_MAX / sizeof(*words->items)) {
            errno = EOVERFLOW;
            bx_diag(diag, "memory exhausted");
            return false;
        }
        char **items = realloc(
            words->items, (size_t)capacity * sizeof(*items));
        if (items == NULL) {
            bx_diag(diag, "memory exhausted");
            return false;
        }
        words->items = items;
        words->capacity = capacity;
    }

    char *item = bx_text_buffer_take(word);
    if (item == NULL) {
        bx_diag(diag, "memory exhausted");
        return false;
    }
    words->items[words->count++] = item;
    return true;
}

static void bx_env_split_words_destroy(struct bx_env_split_words *words) {
    for (int index = 0; index < words->count; index++)
        free(words->items[index]);
    free(words->items);
    *words = (struct bx_env_split_words){0};
}

static bool bx_env_split_append(
    struct bx_text_buffer *word,
    char byte,
    struct bx_diag_ctx *diag) {
    if (bx_text_buffer_append_char(word, byte))
        return true;
    bx_diag(diag, "memory exhausted");
    return false;
}

static bool bx_env_split_start_word(bool *word_started) {
    *word_started = true;
    return true;
}

static bool bx_env_split_finish_word(
    struct bx_env_split_words *words,
    struct bx_text_buffer *word,
    bool *word_started,
    struct bx_diag_ctx *diag) {
    if (!*word_started)
        return true;
    if (!bx_env_split_push_word(words, word, diag))
        return false;
    *word_started = false;
    return true;
}

static bool bx_env_split_var_end(
    const char *input,
    size_t dollar,
    size_t *end_out) {
    size_t index = dollar + 1u;
    if (input[index] != '{')
        return false;
    index++;
    unsigned char first = (unsigned char)input[index];
    bool first_is_alpha =
        (first >= 'A' && first <= 'Z') ||
        (first >= 'a' && first <= 'z');
    if (!(first_is_alpha || first == '_'))
        return false;
    index++;
    while (((input[index] >= 'A' && input[index] <= 'Z') ||
            (input[index] >= 'a' && input[index] <= 'z') ||
            (input[index] >= '0' && input[index] <= '9')) ||
           input[index] == '_') {
        index++;
    }
    if (input[index] != '}')
        return false;
    *end_out = index;
    return true;
}

static char *bx_env_split_quote(const char *text) {
    return bx_output_quote_dup(
        text, BX_OUTPUT_QUOTE_SHELL_ESCAPE_ALWAYS);
}

static void bx_env_split_debug(
    const char *input,
    const struct bx_env_split_words *words) {
    if (words->count == 0)
        return;
    char *quoted = bx_env_split_quote(input);
    fprintf(stderr, "split -S:  %s\n", quoted);
    free(quoted);

    quoted = bx_env_split_quote(words->items[0]);
    fprintf(stderr, " into:    %s\n", quoted);
    free(quoted);
    for (int index = 1; index < words->count; index++) {
        quoted = bx_env_split_quote(words->items[index]);
        fprintf(stderr, "     &    %s\n", quoted);
        free(quoted);
    }
}

bool bx_env_split_parse(
    const char *input,
    int original_argc,
    char **original_argv,
    int original_optind,
    bool debug,
    struct bx_diag_ctx *diag,
    struct bx_env_split_result *result) {
    *result = (struct bx_env_split_result){0};
    struct bx_env_split_words words = {0};
    struct bx_text_buffer word;
    bx_text_buffer_init(&word);
    bool single_quoted = false;
    bool double_quoted = false;
    bool word_started = false;
    bool separator = true;
    size_t index = 0;

    while (input[index] != '\0') {
        unsigned char byte = (unsigned char)input[index];
        if (byte == '\'' && !double_quoted) {
            single_quoted = !single_quoted;
            bx_env_split_start_word(&word_started);
            separator = false;
            index++;
            continue;
        }
        if (byte == '"' && !single_quoted) {
            double_quoted = !double_quoted;
            bx_env_split_start_word(&word_started);
            separator = false;
            index++;
            continue;
        }
        if (strchr(" \t\n\v\f\r", byte) != NULL &&
            !single_quoted && !double_quoted) {
            if (!bx_env_split_finish_word(
                    &words, &word, &word_started, diag)) {
                goto fail;
            }
            separator = true;
            index++;
            continue;
        }
        if (byte == '#' && separator && !single_quoted && !double_quoted)
            break;

        if (byte == '\\' &&
            !(single_quoted &&
              input[index + 1u] != '\\' &&
              input[index + 1u] != '\'')) {
            unsigned char escaped = (unsigned char)input[++index];
            if (escaped == '\0') {
                bx_diag(diag, "invalid backslash at end of string in -S");
                goto fail;
            }
            if (escaped == '_' && !double_quoted) {
                if (!bx_env_split_finish_word(
                        &words, &word, &word_started, diag)) {
                    goto fail;
                }
                separator = true;
                index++;
                continue;
            }
            if (escaped == '_' && double_quoted)
                escaped = ' ';
            else if (escaped == 'c') {
                if (double_quoted) {
                    bx_diag(
                        diag,
                        "'\\c' must not appear in double-quoted -S string");
                    goto fail;
                }
                break;
            } else if (escaped == 'f')
                escaped = '\f';
            else if (escaped == 'n')
                escaped = '\n';
            else if (escaped == 'r')
                escaped = '\r';
            else if (escaped == 't')
                escaped = '\t';
            else if (escaped == 'v')
                escaped = '\v';
            else if (strchr("\"#$'\\", escaped) == NULL) {
                bx_diag(diag, "invalid sequence '\\%c' in -S", escaped);
                goto fail;
            }

            bx_env_split_start_word(&word_started);
            separator = false;
            if (!bx_env_split_append(&word, (char)escaped, diag))
                goto fail;
            index++;
            continue;
        }

        if (byte == '$' && !single_quoted) {
            size_t end = 0;
            if (!bx_env_split_var_end(input, index, &end)) {
                bx_diag(
                    diag,
                    "only ${VARNAME} expansion is supported, error at: %s",
                    input + index);
                goto fail;
            }
            size_t name_length = end - index - 2u;
            char *name = malloc(name_length + 1u);
            if (name == NULL) {
                bx_diag(diag, "memory exhausted");
                goto fail;
            }
            memcpy(name, input + index + 2u, name_length);
            name[name_length] = '\0';
            const char *value = getenv(name);
            if (value != NULL) {
                if (debug) {
                    char *quoted = bx_env_split_quote(value);
                    fprintf(
                        stderr, "expanding ${%s} into %s\n", name, quoted);
                    free(quoted);
                }
                bx_env_split_start_word(&word_started);
                separator = false;
                if (!bx_text_buffer_append_text(&word, value)) {
                    free(name);
                    bx_diag(diag, "memory exhausted");
                    goto fail;
                }
            } else if (debug) {
                fprintf(
                    stderr, "replacing ${%s} with null string\n", name);
            }
            free(name);
            index = end + 1u;
            continue;
        }

        bx_env_split_start_word(&word_started);
        separator = false;
        if (!bx_env_split_append(&word, (char)byte, diag))
            goto fail;
        index++;
    }

    if (single_quoted || double_quoted) {
        bx_diag(diag, "no terminating quote in -S string");
        goto fail;
    }
    if (!bx_env_split_finish_word(
            &words, &word, &word_started, diag)) {
        goto fail;
    }
    bx_text_buffer_destroy(&word);

    if (debug)
        bx_env_split_debug(input, &words);

    int trailing_count = original_argc - original_optind;
    if (trailing_count < 0 ||
        words.count > INT_MAX - trailing_count - 1) {
        bx_diag(diag, "memory exhausted");
        goto fail_words;
    }
    int argc = 1 + words.count + trailing_count;
    char **argv = calloc((size_t)argc + 1u, sizeof(*argv));
    if (argv == NULL) {
        bx_diag(diag, "memory exhausted");
        goto fail_words;
    }
    argv[0] = original_argv[0];
    for (int word_index = 0; word_index < words.count; word_index++)
        argv[word_index + 1] = words.items[word_index];
    for (int tail = 0; tail < trailing_count; tail++)
        argv[1 + words.count + tail] = original_argv[original_optind + tail];

    free(words.items);
    result->argv = argv;
    result->argc = argc;
    result->owned_word_count = words.count;
    return true;

fail:
    bx_text_buffer_destroy(&word);
fail_words:
    bx_env_split_words_destroy(&words);
    return false;
}

void bx_env_split_result_destroy(struct bx_env_split_result *result) {
    if (result == NULL)
        return;
    for (int index = 0; index < result->owned_word_count; index++)
        free(result->argv[index + 1]);
    free(result->argv);
    *result = (struct bx_env_split_result){0};
}
