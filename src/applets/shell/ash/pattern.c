#include <errno.h>
#include <fnmatch.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "applets/shell/ash/pattern.h"

struct ash_pattern_compiler {
    const char* input;
    size_t length;
    size_t position;
    struct ash_pattern_options options;
    uint32_t required_features;
};

static int ash_pattern_grow(
    void** items,
    size_t* capacity,
    size_t needed,
    size_t item_size
) {
    if (*capacity >= needed) {
        return 0;
    }
    size_t grown = *capacity == 0u ? 4u : *capacity;
    while (grown < needed) {
        if (grown > SIZE_MAX / 2u) {
            grown = needed;
            break;
        }
        grown *= 2u;
    }
    if (item_size != 0u && grown > SIZE_MAX / item_size) {
        errno = ENOMEM;
        return -1;
    }
    void* replacement = realloc(*items, grown * item_size);
    if (replacement == NULL) {
        return -1;
    }
    *items = replacement;
    *capacity = grown;
    return 0;
}

static void ash_pattern_sequence_destroy(
    struct ash_pattern_sequence* sequence
);

static void ash_pattern_expression_destroy(
    struct ash_pattern_expression* expression
) {
    if (expression == NULL) {
        return;
    }
    for (size_t i = 0u; i < expression->count; i++) {
        ash_pattern_sequence_destroy(&expression->alternatives[i]);
    }
    free(expression->alternatives);
    free(expression);
}

static void ash_pattern_term_destroy(struct ash_pattern_term* term) {
    if (term->kind == ASH_PATTERN_LITERAL ||
        term->kind == ASH_PATTERN_BRACKET) {
        free(term->value.span.text);
    }
    else if (term->kind == ASH_PATTERN_EXTGLOB_TERM) {
        ash_pattern_expression_destroy(term->value.extglob.expression);
    }
    *term = (struct ash_pattern_term){0};
}

static void ash_pattern_sequence_destroy(
    struct ash_pattern_sequence* sequence
) {
    for (size_t i = 0u; i < sequence->count; i++) {
        ash_pattern_term_destroy(&sequence->terms[i]);
    }
    free(sequence->terms);
    *sequence = (struct ash_pattern_sequence){0};
}

void ash_pattern_destroy(struct ash_pattern* pattern) {
    if (pattern == NULL) {
        return;
    }
    ash_pattern_sequence_destroy(&pattern->root);
    free(pattern->source);
    *pattern = (struct ash_pattern){0};
}

bool ash_pattern_options_valid(const struct ash_pattern_options* options) {
    if (options == NULL ||
        options->purpose < ASH_PATTERN_CASE ||
        options->purpose > ASH_PATTERN_COMPLETION ||
        options->domain < ASH_PATTERN_STRING ||
        options->domain > ASH_PATTERN_PATHNAME ||
        (options->flags & ~ASH_PATTERN_FLAG_ALL) != 0u) {
        return false;
    }
    bool pathname_purpose =
        options->purpose == ASH_PATTERN_PATHNAME_EXPANSION ||
        options->purpose == ASH_PATTERN_GLOBIGNORE;
    bool string_purpose =
        options->purpose == ASH_PATTERN_CASE ||
        options->purpose == ASH_PATTERN_PARAMETER_REMOVE ||
        options->purpose == ASH_PATTERN_PARAMETER_SUBSTITUTE ||
        options->purpose == ASH_PATTERN_CONDITIONAL;
    if ((pathname_purpose && options->domain != ASH_PATTERN_PATHNAME) ||
        (string_purpose && options->domain != ASH_PATTERN_STRING) ||
        ((options->flags &
            (ASH_PATTERN_GLOBSTAR | ASH_PATTERN_MATCH_DOTFILES)) != 0u &&
         options->domain != ASH_PATTERN_PATHNAME)) {
        return false;
    }
    return true;
}

static enum ash_pattern_compile_result ash_pattern_compile_failure(void) {
    if (errno == ELOOP) {
        return ASH_PATTERN_COMPILE_LIMIT;
    }
    return errno == ENOMEM ?
        ASH_PATTERN_COMPILE_NO_MEMORY :
        ASH_PATTERN_COMPILE_INVALID;
}

static int ash_pattern_sequence_take_term(
    struct ash_pattern_sequence* sequence,
    struct ash_pattern_term* term
) {
    if (ash_pattern_grow(
            (void**)&sequence->terms,
            &sequence->capacity,
            sequence->count + 1u,
            sizeof(*sequence->terms)
        ) != 0) {
        return -1;
    }
    sequence->terms[sequence->count++] = *term;
    *term = (struct ash_pattern_term){0};
    return 0;
}

static int ash_pattern_append_literal(
    struct ash_pattern_sequence* sequence,
    size_t source_offset,
    const char* text,
    size_t length
) {
    struct ash_pattern_term* literal = NULL;
    if (sequence->count != 0u &&
        sequence->terms[sequence->count - 1u].kind ==
            ASH_PATTERN_LITERAL) {
        literal = &sequence->terms[sequence->count - 1u];
    }
    if (literal == NULL) {
        struct ash_pattern_term term = {
            .kind = ASH_PATTERN_LITERAL,
            .source_offset = source_offset,
        };
        if (ash_pattern_sequence_take_term(sequence, &term) != 0) {
            return -1;
        }
        literal = &sequence->terms[sequence->count - 1u];
    }

    if (length > SIZE_MAX - literal->value.span.length - 1u) {
        errno = ENOMEM;
        return -1;
    }
    size_t new_length = literal->value.span.length + length;
    char* replacement = realloc(
        literal->value.span.text,
        new_length + 1u
    );
    if (replacement == NULL) {
        return -1;
    }
    if (length != 0u) {
        memcpy(
            replacement + literal->value.span.length,
            text,
            length
        );
    }
    replacement[new_length] = '\0';
    literal->value.span.text = replacement;
    literal->value.span.length = new_length;
    return 0;
}

static int ash_pattern_copy_span(
    struct ash_pattern_span* span,
    const char* text,
    size_t length
) {
    if (length == SIZE_MAX) {
        errno = ENOMEM;
        return -1;
    }
    span->text = malloc(length + 1u);
    if (span->text == NULL) {
        return -1;
    }
    if (length != 0u) {
        memcpy(span->text, text, length);
    }
    span->text[length] = '\0';
    span->length = length;
    return 0;
}

static bool ash_pattern_extglob_operator(
    char character,
    enum ash_extglob_operator* operator
) {
    switch (character) {
        case '?':
            *operator = ASH_EXTGLOB_ZERO_OR_ONE;
            return true;
        case '*':
            *operator = ASH_EXTGLOB_ZERO_OR_MORE;
            return true;
        case '+':
            *operator = ASH_EXTGLOB_ONE_OR_MORE;
            return true;
        case '@':
            *operator = ASH_EXTGLOB_ONE_OF;
            return true;
        case '!':
            *operator = ASH_EXTGLOB_NONE_OF;
            return true;
        default:
            return false;
    }
}

static int ash_pattern_compile_sequence(
    struct ash_pattern_compiler* compiler,
    struct ash_pattern_sequence* sequence,
    bool extglob_alternative,
    size_t nesting
);

static int ash_pattern_expression_take_alternative(
    struct ash_pattern_expression* expression,
    struct ash_pattern_sequence* alternative
) {
    if (ash_pattern_grow(
            (void**)&expression->alternatives,
            &expression->capacity,
            expression->count + 1u,
            sizeof(*expression->alternatives)
        ) != 0) {
        return -1;
    }
    expression->alternatives[expression->count++] = *alternative;
    *alternative = (struct ash_pattern_sequence){0};
    return 0;
}

static int ash_pattern_compile_extglob(
    struct ash_pattern_compiler* compiler,
    struct ash_pattern_sequence* sequence,
    enum ash_extglob_operator operator,
    size_t nesting
) {
    if (nesting >= ASH_PATTERN_MAX_NESTING) {
        errno = ELOOP;
        return -1;
    }
    size_t source_offset = compiler->position;
    compiler->position += 2u;
    struct ash_pattern_expression* expression = calloc(
        1u,
        sizeof(*expression)
    );
    if (expression == NULL) {
        return -1;
    }

    while (true) {
        struct ash_pattern_sequence alternative = {0};
        if (ash_pattern_compile_sequence(
                compiler,
                &alternative,
                true,
                nesting + 1u
            ) != 0 ||
            ash_pattern_expression_take_alternative(
                expression,
                &alternative
            ) != 0) {
            ash_pattern_sequence_destroy(&alternative);
            ash_pattern_expression_destroy(expression);
            return -1;
        }
        if (compiler->position >= compiler->length) {
            ash_pattern_expression_destroy(expression);
            errno = EINVAL;
            return -1;
        }
        char delimiter = compiler->input[compiler->position++];
        if (delimiter == ')') {
            break;
        }
    }

    struct ash_pattern_term term = {
        .kind = ASH_PATTERN_EXTGLOB_TERM,
        .source_offset = source_offset,
        .value.extglob = {
            .operator = operator,
            .expression = expression,
        },
    };
    if (ash_pattern_sequence_take_term(sequence, &term) != 0) {
        ash_pattern_term_destroy(&term);
        return -1;
    }
    compiler->required_features |= ASH_PATTERN_REQUIRES_EXTGLOB;
    return 0;
}

static size_t ash_pattern_bracket_end(
    const struct ash_pattern_compiler* compiler
) {
    size_t position = compiler->position + 1u;
    if (position < compiler->length &&
        (compiler->input[position] == '!' ||
         compiler->input[position] == '^')) {
        position++;
    }
    if (position < compiler->length &&
        compiler->input[position] == ']') {
        position++;
    }
    for (; position < compiler->length; position++) {
        if (compiler->input[position] == '\\' &&
            position + 1u < compiler->length) {
            position++;
            continue;
        }
        if (compiler->input[position] == ']') {
            return position + 1u;
        }
    }
    return compiler->position;
}

static int ash_pattern_compile_sequence(
    struct ash_pattern_compiler* compiler,
    struct ash_pattern_sequence* sequence,
    bool extglob_alternative,
    size_t nesting
) {
    while (compiler->position < compiler->length) {
        char character = compiler->input[compiler->position];
        if (extglob_alternative &&
            (character == '|' || character == ')')) {
            return 0;
        }

        enum ash_extglob_operator extglob_operator;
        if ((compiler->options.flags & ASH_PATTERN_EXTGLOB) != 0u &&
            compiler->position + 1u < compiler->length &&
            compiler->input[compiler->position + 1u] == '(' &&
            ash_pattern_extglob_operator(
                character,
                &extglob_operator
            )) {
            if (ash_pattern_compile_extglob(
                    compiler,
                    sequence,
                    extglob_operator,
                    nesting
                ) != 0) {
                return -1;
            }
            continue;
        }

        size_t source_offset = compiler->position++;
        if (character == '\\') {
            if (compiler->position < compiler->length) {
                character = compiler->input[compiler->position++];
            }
            if (ash_pattern_append_literal(
                    sequence,
                    source_offset,
                    &character,
                    1u
                ) != 0) {
                return -1;
            }
            continue;
        }
        if (character == '*') {
            size_t star_count = 1u;
            while (compiler->position < compiler->length &&
                   compiler->input[compiler->position] == '*') {
                compiler->position++;
                star_count++;
            }
            enum ash_pattern_term_kind kind = ASH_PATTERN_ANY_STRING;
            if (star_count >= 2u &&
                compiler->options.domain == ASH_PATTERN_PATHNAME &&
                (compiler->options.flags & ASH_PATTERN_GLOBSTAR) != 0u) {
                kind = ASH_PATTERN_GLOBSTAR_TERM;
                compiler->required_features |=
                    ASH_PATTERN_REQUIRES_GLOBSTAR;
            }
            struct ash_pattern_term term = {
                .kind = kind,
                .source_offset = source_offset,
            };
            if (ash_pattern_sequence_take_term(sequence, &term) != 0) {
                return -1;
            }
            continue;
        }
        if (character == '?') {
            struct ash_pattern_term term = {
                .kind = ASH_PATTERN_ANY_CHARACTER,
                .source_offset = source_offset,
            };
            if (ash_pattern_sequence_take_term(sequence, &term) != 0) {
                return -1;
            }
            continue;
        }
        if (character == '[') {
            compiler->position--;
            size_t end = ash_pattern_bracket_end(compiler);
            if (end != compiler->position) {
                struct ash_pattern_term term = {
                    .kind = ASH_PATTERN_BRACKET,
                    .source_offset = source_offset,
                };
                if (ash_pattern_copy_span(
                        &term.value.span,
                        compiler->input + compiler->position,
                        end - compiler->position
                    ) != 0 ||
                    ash_pattern_sequence_take_term(
                        sequence,
                        &term
                    ) != 0) {
                    ash_pattern_term_destroy(&term);
                    return -1;
                }
                compiler->position = end;
                if ((compiler->options.flags &
                    ASH_PATTERN_ASCII_RANGES) != 0u) {
                    compiler->required_features |=
                        ASH_PATTERN_REQUIRES_ASCII_RANGES;
                }
                continue;
            }
            compiler->position++;
        }
        if (character == '/' &&
            compiler->options.domain == ASH_PATTERN_PATHNAME) {
            struct ash_pattern_term term = {
                .kind = ASH_PATTERN_PATH_SEPARATOR,
                .source_offset = source_offset,
            };
            if (ash_pattern_sequence_take_term(sequence, &term) != 0) {
                return -1;
            }
            continue;
        }
        if (ash_pattern_append_literal(
                sequence,
                source_offset,
                &character,
                1u
            ) != 0) {
            return -1;
        }
    }
    return 0;
}

enum ash_pattern_compile_result ash_pattern_compile(
    const char* expanded_pattern,
    size_t length,
    const struct ash_pattern_options* options,
    struct ash_pattern* pattern
) {
    if (pattern == NULL || !ash_pattern_options_valid(options) ||
        (expanded_pattern == NULL && length != 0u) ||
        length == SIZE_MAX ||
        (expanded_pattern != NULL &&
         memchr(expanded_pattern, '\0', length) != NULL)) {
        return length == SIZE_MAX ?
            ASH_PATTERN_COMPILE_NO_MEMORY :
            ASH_PATTERN_COMPILE_INVALID;
    }

    struct ash_pattern candidate = {
        .options = *options,
    };
    struct ash_pattern_compiler compiler = {
        .input = expanded_pattern != NULL ? expanded_pattern : "",
        .length = length,
        .options = *options,
    };
    errno = 0;
    if (ash_pattern_compile_sequence(
            &compiler,
            &candidate.root,
            false,
            0u
        ) != 0 ||
        compiler.position != compiler.length) {
        ash_pattern_destroy(&candidate);
        return ash_pattern_compile_failure();
    }
    candidate.source = malloc(length + 1u);
    if (candidate.source == NULL) {
        ash_pattern_destroy(&candidate);
        return ASH_PATTERN_COMPILE_NO_MEMORY;
    }
    if (length != 0u) {
        memcpy(candidate.source, expanded_pattern, length);
    }
    candidate.source[length] = '\0';
    candidate.source_length = length;
    candidate.required_features = compiler.required_features;
    *pattern = candidate;
    return ASH_PATTERN_COMPILE_OK;
}

enum ash_pattern_match_result ash_pattern_match(
    const struct ash_pattern* pattern,
    const char* value
) {
    if (pattern == NULL || pattern->source == NULL || value == NULL ||
        !ash_pattern_options_valid(&pattern->options)) {
        return ASH_PATTERN_MATCH_ERROR;
    }
    if ((pattern->required_features &
        (ASH_PATTERN_REQUIRES_EXTGLOB |
         ASH_PATTERN_REQUIRES_GLOBSTAR |
         ASH_PATTERN_REQUIRES_ASCII_RANGES)) != 0u) {
        return ASH_PATTERN_MATCH_UNSUPPORTED;
    }

    int flags = 0;
    if (pattern->options.domain == ASH_PATTERN_PATHNAME) {
        flags |= FNM_PATHNAME;
        if ((pattern->options.flags &
            ASH_PATTERN_MATCH_DOTFILES) == 0u) {
            flags |= FNM_PERIOD;
        }
    }
#ifdef FNM_CASEFOLD
    if ((pattern->options.flags &
        ASH_PATTERN_CASE_INSENSITIVE) != 0u) {
        flags |= FNM_CASEFOLD;
    }
#else
    if ((pattern->options.flags &
        ASH_PATTERN_CASE_INSENSITIVE) != 0u) {
        return ASH_PATTERN_MATCH_UNSUPPORTED;
    }
#endif

    int result = fnmatch(pattern->source, value, flags);
    if (result == 0) {
        return ASH_PATTERN_MATCH;
    }
    if (result == FNM_NOMATCH) {
        return ASH_PATTERN_NO_MATCH;
    }
    return ASH_PATTERN_MATCH_ERROR;
}
