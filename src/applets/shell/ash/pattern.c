#include <errno.h>
#include <fnmatch.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#include "applets/shell/ash/character.h"
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

static const char* ash_pattern_notation_end(const char* text, const char* end) {
    for (const char* cursor = text + 2u; cursor < end - 1u; cursor++) {
        if (*cursor == '\\') {
            cursor++;
        }
        else if (cursor[0] == text[1] && cursor[1] == ']') {
            return cursor + 2u;
        }
    }
    return NULL;
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
        if (compiler->input[position] == '[' && position + 1u < compiler->length && strchr(".=:", compiler->input[position + 1u]) != NULL) {
            const char* end = ash_pattern_notation_end(compiler->input + position, compiler->input + compiler->length);
            if (end != NULL) {
                position = (size_t)(end - compiler->input) - 1u;
                continue;
            }
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

static size_t ash_pattern_character(const char* text, wint_t* character) {
    if (MB_CUR_MAX == 1u) {
        *character = (unsigned char)*text;
        return *text != '\0' ? 1u : 0u;
    }
    mbstate_t state = {0};
    return ash_character_decode(text, strnlen(text, MB_CUR_MAX), &state, character);
}

static bool ash_pattern_literal(const struct ash_pattern_span* span, const char** value, bool fold) {
    const char* literal = span->text;
    const char* cursor = *value;
    if (!fold) {
        if (strncmp(literal, cursor, span->length) != 0) {
            return false;
        }
        if (span->length != 0u && (unsigned char)literal[span->length - 1u] >= 0x80u && MB_CUR_MAX > 1u && cursor[span->length] != '\0') {
            /* UTF-8 resynchronizes within one maximum-width character. */
            size_t consumed = span->length > MB_CUR_MAX ? span->length - MB_CUR_MAX : 0u;
            while (consumed < span->length) {
                wint_t character;
                consumed += ash_pattern_character(cursor + consumed, &character);
            }
            if (consumed != span->length) {
                return false;
            }
        }
        *value += span->length;
        return true;
    }
    while (*literal != '\0') {
        if (*cursor == '\0') {
            return false;
        }
        wint_t left, right;
        size_t left_width = ash_pattern_character(literal, &left);
        size_t right_width = ash_pattern_character(cursor, &right);
        if (left == WEOF || right == WEOF) {
            if (left != right || *literal != *cursor) {
                return false;
            }
        }
        else if (towlower(left) != towlower(right)) {
            return false;
        }
        literal += left_width;
        cursor += right_width;
    }
    *value = cursor;
    return true;
}

static bool ash_pattern_wildcard_allowed(const struct ash_pattern* pattern, const char* start, const char* value) {
    if (pattern->options.domain != ASH_PATTERN_PATHNAME) {
        return true;
    }
    return *value != '/' && !(*value == '.' && (value == start || value[-1] == '/') && (pattern->options.flags & ASH_PATTERN_MATCH_DOTFILES) == 0u);
}

static bool ash_pattern_valid_span(const char* text, size_t length) {
    for (size_t offset = 0u; offset < length;) {
        if ((unsigned char)text[offset] < 0x80u) {
            offset++;
            continue;
        }
        wint_t character;
        size_t width = ash_pattern_character(text + offset, &character);
        if (character == WEOF || width > length - offset) {
            return false;
        }
        offset += width;
    }
    return true;
}

struct ash_bracket_atom {
    const char* end;
    const char* literal;
    bool valid;
    wint_t character;
    size_t literal_length;
    char notation;
};

static struct ash_bracket_atom ash_pattern_bracket_atom(const char* text, const char* end) {
    if (*text == '\\' && text + 1u < end) {
        text++;
    }
    else if (*text == '[' && text + 1u < end && strchr(".=:", text[1]) != NULL) {
        const char* close = ash_pattern_notation_end(text, end);
        if (close != NULL) {
            struct ash_bracket_atom atom = {
                .end = close,
                .valid = ash_pattern_valid_span(text, (size_t)(close - text)),
                .character = WEOF,
                .notation = text[1],
            };
            if (atom.valid && text[1] != ':' && text + 4u < close) {
                const char* literal = text + 2u;
                if (*literal == '\\' && literal + 1u < close - 2u) {
                    literal++;
                }
                wint_t character;
                size_t width = ash_pattern_character(literal, &character);
                if (character != WEOF && width == (size_t)(close - 2u - literal)) {
                    atom.literal = literal;
                    atom.character = character;
                    atom.literal_length = width;
                }
            }
            return atom;
        }
    }
    wint_t character;
    size_t width = ash_pattern_character(text, &character);
    return (struct ash_bracket_atom){text + width, text, character != WEOF, character, width, '\0'};
}

static bool ash_pattern_range_literal(const struct ash_bracket_atom* atom) {
    return atom->valid && atom->literal != NULL && atom->notation != '=' && (atom->character >= 0x80u || strchr("[]!^-\\", (int)atom->character) == NULL);
}

static enum ash_pattern_match_result ash_pattern_classify(const char* bracket, const char* single, int flags) {
    errno = 0;
    int result = fnmatch(bracket, single, flags);
    if (result == 0 || result == FNM_NOMATCH) {
        return result == 0 ? ASH_PATTERN_MATCH : ASH_PATTERN_NO_MATCH;
    }
    if (errno == 0) {
        errno = EINVAL;
    }
    return ASH_PATTERN_MATCH_ERROR;
}

static enum ash_pattern_match_result ash_pattern_collation_range(const struct ash_bracket_atom* left, const struct ash_bracket_atom* right, const char* single, wint_t character, bool fold) {
    char first[MB_LEN_MAX + 1u];
    char last[MB_LEN_MAX + 1u];
    memcpy(first, left->literal, left->literal_length);
    first[left->literal_length] = '\0';
    memcpy(last, right->literal, right->literal_length);
    last[right->literal_length] = '\0';
    char folded[MB_LEN_MAX + 1u];
    const char* candidate = single;
    /* Some libc declarations mark strcoll pure despite its errno contract. */
    int (*volatile compare)(const char*, const char*) = strcoll;
    for (unsigned int attempt = 0u; attempt < (fold ? 2u : 1u); attempt++) {
        if (attempt != 0u) {
            wint_t alternate = towupper(character);
            if (alternate == character) {
                alternate = towlower(character);
            }
            if (alternate == character) {
                break;
            }
            mbstate_t state = {0};
            size_t length = wcrtomb(folded, (wchar_t)alternate, &state);
            if (length == (size_t)-1) {
                return ASH_PATTERN_MATCH_ERROR;
            }
            folded[length] = '\0';
            candidate = folded;
        }
        errno = 0;
        int lower = compare(first, candidate);
        if (errno != 0) {
            return ASH_PATTERN_MATCH_ERROR;
        }
        int upper = compare(candidate, last);
        if (errno != 0) {
            return ASH_PATTERN_MATCH_ERROR;
        }
        if (lower <= 0 && upper <= 0) {
            return ASH_PATTERN_MATCH;
        }
    }
    return ASH_PATTERN_NO_MATCH;
}

static enum ash_pattern_match_result ash_pattern_bracket(const struct ash_pattern_span* span, const char* single, wint_t character, bool fold, int flags) {
    if ((MB_CUR_MAX == 1u || (character != WEOF && ash_pattern_valid_span(span->text, span->length))) && strchr(span->text, '\\') == NULL && strstr(span->text, "[.") == NULL &&
        strstr(span->text, "[=") == NULL) {
        return ash_pattern_classify(span->text, single, flags);
    }

    const char* cursor = span->text + 1u;
    const char* end = span->text + span->length - 1u;
    bool negate = *cursor == '!' || *cursor == '^';
    cursor += negate ? 1u : 0u;
    char* member = NULL;
    bool matched = false;
    while (cursor < end) {
        const char* start = cursor;
        struct ash_bracket_atom left = ash_pattern_bracket_atom(cursor, end);
        cursor = left.end;
        bool classify = left.valid && (left.literal == NULL || left.notation == '=');
        struct ash_bracket_atom right = {0};
        bool normalize_range = false;
        bool symbol_range = false;
        if (cursor + 1u < end && *cursor == '-') {
            right = ash_pattern_bracket_atom(cursor + 1u, end);
            cursor = right.end;
            classify = left.valid && right.valid;
            symbol_range = classify && left.literal != NULL && right.literal != NULL && left.notation != '=' && right.notation != '=' && (left.notation == '.' || right.notation == '.');
            normalize_range = symbol_range && ash_pattern_range_literal(&left) && ash_pattern_range_literal(&right);
            if (character == WEOF && !left.valid && !right.valid && left.literal != NULL && right.literal != NULL) {
                unsigned char byte = (unsigned char)*single;
                matched |= (unsigned char)*left.literal <= byte && byte <= (unsigned char)*right.literal;
            }
        }
        else if (left.literal != NULL) {
            if (character == WEOF) {
                matched |= !left.valid && *left.literal == *single;
            }
            else if (left.valid) {
                matched |= fold ? towlower(left.character) == towlower(character) : left.character == character;
            }
        }
        if (classify && character != WEOF) {
            if (member == NULL) {
                if (span->length > SIZE_MAX - 2u) {
                    errno = ENOMEM;
                    return ASH_PATTERN_MATCH_ERROR;
                }
                member = malloc(span->length + 2u);
                if (member == NULL) {
                    return ASH_PATTERN_MATCH_ERROR;
                }
            }
            size_t length = 1u;
            member[0] = '[';
            if (normalize_range) {
                memcpy(member + length, left.literal, left.literal_length);
                length += left.literal_length;
                member[length++] = '-';
                memcpy(member + length, right.literal, right.literal_length);
                length += right.literal_length;
            }
            else {
                if (*start == '!' || *start == '^') {
                    member[length++] = '\\';
                }
                size_t copied = (size_t)(cursor - start);
                memcpy(member + length, start, copied);
                length += copied;
            }
            member[length++] = ']';
            member[length] = '\0';
            enum ash_pattern_match_result result;
            if (symbol_range && !normalize_range) {
                result = ash_pattern_classify("[[.a.]]", "a", 0);
                if (result == ASH_PATTERN_MATCH) {
                    result = ash_pattern_classify(member, single, flags);
                }
                else if (result == ASH_PATTERN_NO_MATCH) {
                    result = ash_pattern_collation_range(&left, &right, single, character, fold);
                }
            }
            else {
                result = ash_pattern_classify(member, single, flags);
            }
            if (result == ASH_PATTERN_MATCH_ERROR) {
                int error = errno;
                free(member);
                errno = error;
                return ASH_PATTERN_MATCH_ERROR;
            }
            matched |= result == ASH_PATTERN_MATCH;
        }
    }
    free(member);
    return matched != negate ? ASH_PATTERN_MATCH : ASH_PATTERN_NO_MATCH;
}

enum ash_pattern_match_result ash_pattern_match(
    const struct ash_pattern* pattern,
    const char* value
) {
    if (pattern == NULL || pattern->source == NULL || value == NULL ||
        !ash_pattern_options_valid(&pattern->options)) {
        errno = EINVAL;
        return ASH_PATTERN_MATCH_ERROR;
    }
    if ((pattern->required_features &
        (ASH_PATTERN_REQUIRES_EXTGLOB |
         ASH_PATTERN_REQUIRES_GLOBSTAR |
         ASH_PATTERN_REQUIRES_ASCII_RANGES)) != 0u) {
        return ASH_PATTERN_MATCH_UNSUPPORTED;
    }

    bool fold = (pattern->options.flags & ASH_PATTERN_CASE_INSENSITIVE) != 0u;
    int flags = 0;
#ifdef FNM_CASEFOLD
    if (fold) {
        flags |= FNM_CASEFOLD;
    }
#else
    if ((pattern->options.flags &
        ASH_PATTERN_CASE_INSENSITIVE) != 0u) {
        return ASH_PATTERN_MATCH_UNSUPPORTED;
    }
#endif

    size_t trailing = pattern->source_length;
    while (trailing != 0u && pattern->source[trailing - 1u] == '\\') {
        trailing--;
    }
    if ((pattern->source_length - trailing) % 2u != 0u) {
        return ASH_PATTERN_NO_MATCH;
    }

    const char* start = value;
    const char* retry = NULL;
    size_t after_star = 0u;
    size_t position = 0u;
    for (;;) {
        if (position == pattern->root.count) {
            if (*value == '\0') {
                return ASH_PATTERN_MATCH;
            }
        }
        else {
            const struct ash_pattern_term* term = &pattern->root.terms[position];
            bool matched = false;
            if (term->kind == ASH_PATTERN_LITERAL) {
                matched = ash_pattern_literal(&term->value.span, &value, fold);
            }
            else if (term->kind == ASH_PATTERN_PATH_SEPARATOR) {
                matched = *value == '/';
                value += matched ? 1u : 0u;
                if (matched) {
                    retry = NULL;
                }
            }
            else if (ash_pattern_wildcard_allowed(pattern, start, value) || (term->kind == ASH_PATTERN_ANY_STRING && *value == '/')) {
                if (term->kind == ASH_PATTERN_ANY_STRING) {
                    after_star = ++position;
                    retry = value;
                    continue;
                }
                if (*value != '\0') {
                    wint_t character;
                    size_t width = ash_pattern_character(value, &character);
                    if (term->kind == ASH_PATTERN_ANY_CHARACTER) {
                        matched = true;
                    }
                    else if (term->kind == ASH_PATTERN_BRACKET) {
                        char single[MB_LEN_MAX + 1u];
                        memcpy(single, value, width);
                        single[width] = '\0';
                        enum ash_pattern_match_result result = ash_pattern_bracket(&term->value.span, single, character, fold, flags);
                        if (result == ASH_PATTERN_MATCH_ERROR) {
                            return ASH_PATTERN_MATCH_ERROR;
                        }
                        matched = result == ASH_PATTERN_MATCH;
                    }
                    value += matched ? width : 0u;
                }
            }
            if (matched) {
                position++;
                continue;
            }
        }
        if (retry == NULL || *retry == '\0' || !ash_pattern_wildcard_allowed(pattern, start, retry)) {
            return ASH_PATTERN_NO_MATCH;
        }
        wint_t character;
        retry += ash_pattern_character(retry, &character);
        value = retry;
        position = after_star;
    }
}
