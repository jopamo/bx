#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "applets/shell/ash/lexer.h"

struct ash_operator {
    const char* text;
    size_t length;
    enum ash_token_kind kind;
};

#define ASH_OPERATOR(text, kind) {text, sizeof(text) - 1u, kind}
static const struct ash_operator ash_operators[] = {
    ASH_OPERATOR("\n", ASH_TOKEN_NEWLINE),
    ASH_OPERATOR("&&", ASH_TOKEN_AND_IF),
    ASH_OPERATOR("||", ASH_TOKEN_OR_IF),
    ASH_OPERATOR("|", ASH_TOKEN_PIPE),
    ASH_OPERATOR("|&", ASH_TOKEN_PIPE_AND),
    ASH_OPERATOR("&", ASH_TOKEN_AMP),
    ASH_OPERATOR("&>", ASH_TOKEN_AND_GREAT),
    ASH_OPERATOR("&>>", ASH_TOKEN_AND_DGREAT),
    ASH_OPERATOR(";", ASH_TOKEN_SEMI),
    ASH_OPERATOR(";;", ASH_TOKEN_DSEMI),
    ASH_OPERATOR(";&", ASH_TOKEN_SEMI_AND),
    ASH_OPERATOR(";;&", ASH_TOKEN_DSEMI_AND),
    ASH_OPERATOR("(", ASH_TOKEN_LPAREN),
    ASH_OPERATOR(")", ASH_TOKEN_RPAREN),
    ASH_OPERATOR("<", ASH_TOKEN_LESS),
    ASH_OPERATOR("<<", ASH_TOKEN_DLESS),
    ASH_OPERATOR("<<-", ASH_TOKEN_DLESS_DASH),
    ASH_OPERATOR("<<<", ASH_TOKEN_TLESS),
    ASH_OPERATOR("<&", ASH_TOKEN_LESS_AND),
    ASH_OPERATOR("<>", ASH_TOKEN_LESS_GREAT),
    ASH_OPERATOR(">", ASH_TOKEN_GREAT),
    ASH_OPERATOR(">>", ASH_TOKEN_DGREAT),
    ASH_OPERATOR(">&", ASH_TOKEN_GREAT_AND),
    ASH_OPERATOR(">|", ASH_TOKEN_CLOBBER),
};
#undef ASH_OPERATOR

struct ash_source_location ash_lexer_current_location(
    const struct ash_lexer* lexer
) {
    return (struct ash_source_location){
        .source = lexer->source_name,
        .identity = lexer->source_identity,
        .line = lexer->line,
        .column = lexer->column,
        .offset = lexer->source_offset + lexer->offset,
    };
}

void ash_lexer_init_at(
    struct ash_lexer* lexer,
    struct ash_source_location origin,
    const char* input,
    size_t length
) {
    assert(lexer != NULL);
    assert(ash_source_location_valid(&origin));
    assert(input != NULL);
    assert(origin.offset <= SIZE_MAX - length);
    *lexer = (struct ash_lexer){
        .source_name = origin.source,
        .source_identity = origin.identity,
        .input = input,
        .length = length,
        .source_offset = origin.offset,
        .line = origin.line,
        .column = origin.column,
    };
}

void ash_lexer_init(
    struct ash_lexer* lexer,
    const char* source_name,
    const char* input,
    size_t length
) {
    ash_lexer_init_at(
        lexer,
        (struct ash_source_location){
            .source = source_name != NULL ? source_name : "<input>",
            .line = 1u,
            .column = 1u,
        },
        input,
        length
    );
}

void ash_token_destroy(struct ash_token* token) {
    if (token == NULL) {
        return;
    }
    ash_word_destroy(&token->word);
    free(token->io_redirect);
    *token = (struct ash_token){0};
}

static bool ash_lexer_at_end(const struct ash_lexer* lexer) {
    return lexer->offset == lexer->length;
}

static bool ash_is_line_continuation_at(
    const char* input,
    size_t length,
    size_t position
) {
    return position < length &&
        input[position] == '\\' &&
        position + 1u < length &&
        input[position + 1u] == '\n';
}

static char ash_lexer_peek(const struct ash_lexer* lexer, size_t distance) {
    if (distance > lexer->length - lexer->offset) {
        return '\0';
    }
    size_t position = lexer->offset + distance;
    return (position < lexer->length) ? lexer->input[position] : '\0';
}

static char ash_lexer_peek_logical_at(
    const struct ash_lexer* lexer,
    size_t* position
) {
    while (*position < lexer->length &&
           ash_is_line_continuation_at(
               lexer->input,
               lexer->length,
               *position
           )) {
        *position += 2u;
    }
    return *position < lexer->length ?
        lexer->input[*position] :
        '\0';
}

/*
 * Outside quote modes that preserve backslashes, a backslash-newline pair is
 * not a shell character. Logical lookahead must therefore see through the
 * pair before token, operator, or name recognition makes a decision.
 */
static char ash_lexer_peek_logical(
    const struct ash_lexer* lexer,
    size_t distance
) {
    size_t position = lexer->offset;
    for (size_t logical_offset = 0u;
         logical_offset <= distance;
         logical_offset++) {
        char ch = ash_lexer_peek_logical_at(lexer, &position);
        if (logical_offset == distance || ch == '\0') {
            return ch;
        }
        position++;
    }
    return '\0';
}

static char ash_lexer_advance(struct ash_lexer* lexer) {
    if (ash_lexer_at_end(lexer)) {
        return '\0';
    }

    char ch = lexer->input[lexer->offset++];
    if (ch == '\n') {
        lexer->line++;
        lexer->column = 1u;
    }
    else {
        lexer->column++;
    }
    return ch;
}

static bool ash_lexer_starts_with(const struct ash_lexer* lexer, const char* text) {
    for (size_t i = 0u; text[i] != '\0'; i++) {
        if (ash_lexer_peek_logical(lexer, i) != text[i]) {
            return false;
        }
    }
    return true;
}

static void ash_lexer_advance_count(struct ash_lexer* lexer, size_t count) {
    for (size_t i = 0u; i < count; i++) {
        (void)ash_lexer_advance(lexer);
    }
}

static void ash_lexer_skip_line_continuations(struct ash_lexer* lexer) {
    while (ash_is_line_continuation_at(
               lexer->input,
               lexer->length,
               lexer->offset
           )) {
        ash_lexer_advance_count(lexer, 2u);
        if (ash_lexer_at_end(lexer)) {
            lexer->ended_with_line_continuation = true;
        }
    }
}

static char ash_lexer_advance_logical(struct ash_lexer* lexer) {
    ash_lexer_skip_line_continuations(lexer);
    return ash_lexer_advance(lexer);
}

bool ash_lexer_ended_with_line_continuation(
    const struct ash_lexer* lexer
) {
    return lexer != NULL && lexer->ended_with_line_continuation;
}

static enum ash_lexer_result ash_lexer_fail(
    struct ash_lexer* lexer,
    enum ash_lexer_result result,
    struct ash_source_location location,
    const char* message
) {
    lexer->error_location = location;
    lexer->error = message;
    return result;
}

static bool ash_is_name_start(unsigned char ch) {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_';
}

static bool ash_is_name_char(unsigned char ch) {
    return ash_is_name_start(ch) || (ch >= '0' && ch <= '9');
}

static bool ash_is_blank(char ch) {
    return ch == ' ' || ch == '\t';
}

static const struct ash_operator* ash_lexer_operator(const struct ash_lexer* lexer) {
    const struct ash_operator* longest = NULL;
    for (size_t i = 0u;
         i < sizeof(ash_operators) / sizeof(ash_operators[0]);
         i++) {
        const struct ash_operator* candidate = &ash_operators[i];
        if (longest != NULL && candidate->length <= longest->length) {
            continue;
        }
        bool matches = true;
        for (size_t j = 0u; j < candidate->length; j++) {
            if (ash_lexer_peek_logical(lexer, j) != candidate->text[j]) {
                matches = false;
                break;
            }
        }
        if (matches) {
            longest = candidate;
        }
    }
    return longest;
}

bool ash_token_is_redirection(enum ash_token_kind kind) {
    return kind >= ASH_TOKEN_LESS && kind <= ASH_TOKEN_AND_DGREAT;
}

bool ash_token_kind_valid(enum ash_token_kind kind) {
    return kind >= ASH_TOKEN_EOF && kind < ASH_TOKEN_COUNT;
}

bool ash_token_is_redirection_prefix(enum ash_token_kind kind) {
    return kind == ASH_TOKEN_IO_NUMBER ||
        kind == ASH_TOKEN_IO_VARIABLE;
}

static int ash_word_append_span(
    struct ash_word* word,
    enum ash_word_part_kind kind,
    enum ash_quote_kind quote,
    struct ash_source_location location,
    const char* text,
    size_t length
) {
    int result;
    if (kind == ASH_WORD_TEXT && word->count != 0u &&
        word->parts[word->count - 1u].kind == kind &&
        word->parts[word->count - 1u].quote == quote) {
        result = ash_word_extend_last_part(
            word,
            kind,
            quote,
            text,
            length
        );
    }
    else {
        result = ash_word_add_part(
            word,
            kind,
            quote,
            location,
            text,
            length
        );
    }
    if (result != 0) {
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

static int ash_word_extend_span(
    struct ash_word* word,
    enum ash_word_part_kind kind,
    enum ash_quote_kind quote,
    const char* text,
    size_t length
) {
    if (ash_word_extend_last_part(
            word,
            kind,
            quote,
            text,
            length
        ) != 0) {
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

static int ash_word_append_parameter_span(
    struct ash_word* word,
    enum ash_quote_kind quote,
    struct ash_source_location location,
    const char* text,
    size_t length
) {
    if (length == SIZE_MAX) {
        errno = ENOMEM;
        return -1;
    }
    if (memchr(text, '\\', length) == NULL) {
        return ash_word_append_span(
            word,
            ASH_WORD_PARAMETER,
            quote,
            location,
            text,
            length
        );
    }
    char* normalized = malloc(length + 1u);
    if (normalized == NULL) {
        errno = ENOMEM;
        return -1;
    }

    size_t output_length = 0u;
    enum ash_quote_kind nested_quote = ASH_QUOTE_NONE;
    for (size_t i = 0u; i < length;) {
        char ch = text[i];
        if (nested_quote != ASH_QUOTE_SINGLE &&
            ash_is_line_continuation_at(text, length, i)) {
            i += 2u;
            continue;
        }

        normalized[output_length++] = ch;
        i++;
        if (nested_quote == ASH_QUOTE_SINGLE) {
            if (ch == '\'') {
                nested_quote = ASH_QUOTE_NONE;
            }
            continue;
        }
        if (ch == '\\' && i < length) {
            normalized[output_length++] = text[i++];
            continue;
        }
        if (nested_quote == ASH_QUOTE_DOUBLE) {
            if (ch == '"') {
                nested_quote = ASH_QUOTE_NONE;
            }
            continue;
        }
        if (ch == '\'') {
            nested_quote = ASH_QUOTE_SINGLE;
        }
        else if (ch == '"') {
            nested_quote = ASH_QUOTE_DOUBLE;
        }
    }
    normalized[output_length] = '\0';

    int result = ash_word_append_span(
        word,
        ASH_WORD_PARAMETER,
        quote,
        location,
        normalized,
        output_length
    );
    free(normalized);
    return result;
}

static enum ash_lexer_result ash_lexer_scan_parameter(
    struct ash_lexer* lexer,
    struct ash_word* word,
    enum ash_quote_kind quote
) {
    struct ash_source_location location =
        ash_lexer_current_location(lexer);
    size_t start = lexer->offset;
    (void)ash_lexer_advance_logical(lexer);
    (void)ash_lexer_advance_logical(lexer);
    size_t depth = 1u;
    enum ash_quote_kind nested_quote = ASH_QUOTE_NONE;

    while (!ash_lexer_at_end(lexer)) {
        if (nested_quote != ASH_QUOTE_SINGLE) {
            ash_lexer_skip_line_continuations(lexer);
            if (ash_lexer_at_end(lexer)) {
                break;
            }
        }
        char ch = ash_lexer_peek(lexer, 0u);
        if (nested_quote == ASH_QUOTE_SINGLE) {
            (void)ash_lexer_advance(lexer);
            if (ch == '\'') {
                nested_quote = ASH_QUOTE_NONE;
            }
            continue;
        }
        if (nested_quote == ASH_QUOTE_DOUBLE) {
            if (ch == '\\' && ash_lexer_peek(lexer, 1u) != '\0') {
                ash_lexer_advance_count(lexer, 2u);
                continue;
            }
            (void)ash_lexer_advance(lexer);
            if (ch == '"') {
                nested_quote = ASH_QUOTE_NONE;
            }
            continue;
        }
        if (ch == '\'') {
            nested_quote = ASH_QUOTE_SINGLE;
            (void)ash_lexer_advance(lexer);
            continue;
        }
        if (ch == '"') {
            nested_quote = ASH_QUOTE_DOUBLE;
            (void)ash_lexer_advance(lexer);
            continue;
        }
        if (ch == '\\' && ash_lexer_peek(lexer, 1u) != '\0') {
            ash_lexer_advance_count(lexer, 2u);
            continue;
        }
        if (ash_lexer_starts_with(lexer, "${")) {
            depth++;
            (void)ash_lexer_advance_logical(lexer);
            (void)ash_lexer_advance_logical(lexer);
            continue;
        }
        (void)ash_lexer_advance(lexer);
        if (ch == '}') {
            depth--;
            if (depth == 0u) {
                size_t length = lexer->offset - start;
                if (ash_word_append_parameter_span(
                        word,
                        quote,
                        location,
                        lexer->input + start,
                        length
                    ) != 0) {
                    return ash_lexer_fail(lexer, ASH_LEXER_ERROR, location, "out of memory");
                }
                return ASH_LEXER_TOKEN;
            }
        }
    }

    return ash_lexer_fail(
        lexer,
        ASH_LEXER_INCOMPLETE,
        location,
        "unterminated parameter expansion"
    );
}

static enum ash_lexer_result ash_lexer_scan_parenthesized(
    struct ash_lexer* lexer,
    struct ash_word* word,
    enum ash_quote_kind quote,
    bool arithmetic
) {
    struct ash_source_location location =
        ash_lexer_current_location(lexer);
    (void)ash_lexer_advance_logical(lexer);
    (void)ash_lexer_advance_logical(lexer);
    if (arithmetic) {
        (void)ash_lexer_advance_logical(lexer);
    }
    size_t body_start = lexer->offset;
    size_t depth = 1u;
    enum ash_quote_kind nested_quote = ASH_QUOTE_NONE;

    while (!ash_lexer_at_end(lexer)) {
        if (nested_quote != ASH_QUOTE_SINGLE) {
            ash_lexer_skip_line_continuations(lexer);
            if (ash_lexer_at_end(lexer)) {
                break;
            }
        }
        char ch = ash_lexer_peek(lexer, 0u);
        if (nested_quote == ASH_QUOTE_SINGLE) {
            (void)ash_lexer_advance(lexer);
            if (ch == '\'') {
                nested_quote = ASH_QUOTE_NONE;
            }
            continue;
        }
        if (nested_quote == ASH_QUOTE_DOUBLE) {
            if (ch == '\\' && ash_lexer_peek(lexer, 1u) != '\0') {
                ash_lexer_advance_count(lexer, 2u);
                continue;
            }
            (void)ash_lexer_advance(lexer);
            if (ch == '"') {
                nested_quote = ASH_QUOTE_NONE;
            }
            continue;
        }
        if (ch == '\'') {
            nested_quote = ASH_QUOTE_SINGLE;
            (void)ash_lexer_advance(lexer);
            continue;
        }
        if (ch == '"') {
            nested_quote = ASH_QUOTE_DOUBLE;
            (void)ash_lexer_advance(lexer);
            continue;
        }
        if (ch == '\\' && ash_lexer_peek(lexer, 1u) != '\0') {
            ash_lexer_advance_count(lexer, 2u);
            continue;
        }
        if (ch == '(') {
            depth++;
            (void)ash_lexer_advance(lexer);
            continue;
        }
        if (ch != ')') {
            (void)ash_lexer_advance(lexer);
            continue;
        }

        (void)ash_lexer_advance(lexer);
        depth--;
        if (depth != 0u) {
            continue;
        }
        size_t body_end = lexer->offset - 1u;
        if (arithmetic) {
            ash_lexer_skip_line_continuations(lexer);
            if (ash_lexer_peek(lexer, 0u) != ')') {
                return ash_lexer_fail(
                    lexer,
                    ASH_LEXER_INCOMPLETE,
                    location,
                    "unterminated arithmetic expansion"
                );
            }
            (void)ash_lexer_advance(lexer);
        }

        const char* prefix = arithmetic ? "$((" : "$(";
        size_t prefix_length = arithmetic ? 3u : 2u;
        const char* suffix = arithmetic ? "))" : ")";
        size_t suffix_length = arithmetic ? 2u : 1u;
        if (ash_word_append_span(
                word,
                arithmetic ? ASH_WORD_ARITHMETIC : ASH_WORD_COMMAND_SUBSTITUTION,
                quote,
                location,
                prefix,
                prefix_length
            ) != 0 ||
            ash_word_extend_span(
                word,
                arithmetic ? ASH_WORD_ARITHMETIC : ASH_WORD_COMMAND_SUBSTITUTION,
                quote,
                lexer->input + body_start,
                body_end - body_start
            ) != 0 ||
            ash_word_extend_span(
                word,
                arithmetic ? ASH_WORD_ARITHMETIC : ASH_WORD_COMMAND_SUBSTITUTION,
                quote,
                suffix,
                suffix_length
            ) != 0) {
            return ash_lexer_fail(lexer, ASH_LEXER_ERROR, location, "out of memory");
        }
        return ASH_LEXER_TOKEN;
    }

    return ash_lexer_fail(
        lexer,
        ASH_LEXER_INCOMPLETE,
        location,
        arithmetic ? "unterminated arithmetic expansion" :
            "unterminated command substitution"
    );
}

static enum ash_lexer_result ash_lexer_scan_backquote(
    struct ash_lexer* lexer,
    struct ash_word* word,
    enum ash_quote_kind quote
) {
    struct ash_source_location location =
        ash_lexer_current_location(lexer);
    size_t start = lexer->offset;
    (void)ash_lexer_advance(lexer);
    while (!ash_lexer_at_end(lexer)) {
        char ch = ash_lexer_advance(lexer);
        if (ch == '\\' && !ash_lexer_at_end(lexer)) {
            (void)ash_lexer_advance(lexer);
            continue;
        }
        if (ch == '`') {
            size_t length = lexer->offset - start;
            if (ash_word_append_span(
                    word,
                    ASH_WORD_BACKQUOTE,
                    quote,
                    location,
                    lexer->input + start,
                    length
                ) != 0) {
                return ash_lexer_fail(lexer, ASH_LEXER_ERROR, location, "out of memory");
            }
            return ASH_LEXER_TOKEN;
        }
    }

    return ash_lexer_fail(
        lexer,
        ASH_LEXER_INCOMPLETE,
        location,
        "unterminated backquote substitution"
    );
}

static enum ash_lexer_result ash_lexer_scan_dollar(
    struct ash_lexer* lexer,
    struct ash_word* word,
    enum ash_quote_kind quote
) {
    if (ash_lexer_starts_with(lexer, "${")) {
        return ash_lexer_scan_parameter(lexer, word, quote);
    }
    if (ash_lexer_starts_with(lexer, "$((")) {
        return ash_lexer_scan_parenthesized(lexer, word, quote, true);
    }
    if (ash_lexer_starts_with(lexer, "$(")) {
        return ash_lexer_scan_parenthesized(lexer, word, quote, false);
    }

    struct ash_source_location location =
        ash_lexer_current_location(lexer);
    size_t start = lexer->offset;
    (void)ash_lexer_advance_logical(lexer);
    char ch = ash_lexer_peek_logical(lexer, 0u);
    bool named = ash_is_name_start((unsigned char)ch);
    bool special = (ch >= '0' && ch <= '9') ||
        (ch != '\0' && strchr("@*#?-$!", ch) != NULL);
    if (!named && !special) {
        return ash_word_append_span(
            word,
            ASH_WORD_TEXT,
            quote,
            location,
            "$",
            1u
        ) == 0 ? ASH_LEXER_TOKEN :
            ash_lexer_fail(lexer, ASH_LEXER_ERROR, location, "out of memory");
    }

    if (named) {
        do {
            (void)ash_lexer_advance_logical(lexer);
            ch = ash_lexer_peek_logical(lexer, 0u);
        } while (ash_is_name_char((unsigned char)ch));
    }
    else {
        (void)ash_lexer_advance_logical(lexer);
    }
    if (ash_word_append_parameter_span(
            word,
            quote,
            location,
            lexer->input + start,
            lexer->offset - start
        ) != 0) {
        return ash_lexer_fail(
            lexer,
            ASH_LEXER_ERROR,
            location,
            "out of memory"
        );
    }
    return ASH_LEXER_TOKEN;
}

static enum ash_lexer_result ash_lexer_scan_single_quote(
    struct ash_lexer* lexer,
    struct ash_word* word
) {
    struct ash_source_location location =
        ash_lexer_current_location(lexer);
    (void)ash_lexer_advance(lexer);
    size_t start = lexer->offset;
    while (!ash_lexer_at_end(lexer) && ash_lexer_peek(lexer, 0u) != '\'') {
        (void)ash_lexer_advance(lexer);
    }
    if (ash_lexer_at_end(lexer)) {
        return ash_lexer_fail(
            lexer,
            ASH_LEXER_INCOMPLETE,
            location,
            "unterminated single quote"
        );
    }

    size_t length = lexer->offset - start;
    if (ash_word_append_span(
            word,
            ASH_WORD_TEXT,
            ASH_QUOTE_SINGLE,
            location,
            lexer->input + start,
            length
        ) != 0) {
        return ash_lexer_fail(lexer, ASH_LEXER_ERROR, location, "out of memory");
    }
    (void)ash_lexer_advance(lexer);
    return ASH_LEXER_TOKEN;
}

static enum ash_lexer_result ash_lexer_scan_dollar_single_quote(
    struct ash_lexer* lexer,
    struct ash_word* word
) {
    struct ash_source_location location =
        ash_lexer_current_location(lexer);
    ash_lexer_advance_count(lexer, 2u);
    size_t start = lexer->offset;
    while (!ash_lexer_at_end(lexer)) {
        char ch = ash_lexer_peek(lexer, 0u);
        if (ch == '\'') {
            size_t length = lexer->offset - start;
            if (ash_word_append_span(
                    word,
                    ASH_WORD_TEXT,
                    ASH_QUOTE_DOLLAR_SINGLE,
                    location,
                    lexer->input + start,
                    length
                ) != 0) {
                return ash_lexer_fail(lexer, ASH_LEXER_ERROR, location, "out of memory");
            }
            (void)ash_lexer_advance(lexer);
            return ASH_LEXER_TOKEN;
        }
        if (ch == '\\' && ash_lexer_peek(lexer, 1u) != '\0') {
            ash_lexer_advance_count(lexer, 2u);
        }
        else {
            (void)ash_lexer_advance(lexer);
        }
    }

    return ash_lexer_fail(
        lexer,
        ASH_LEXER_INCOMPLETE,
        location,
        "unterminated dollar-single quote"
    );
}

static bool ash_lexer_word_boundary(const struct ash_lexer* lexer) {
    char ch = ash_lexer_peek_logical(lexer, 0u);
    return ch == '\0' || ash_is_blank(ch) ||
        ash_lexer_operator(lexer) != NULL;
}

static bool ash_lexer_text_boundary(
    const struct ash_lexer* lexer,
    enum ash_quote_kind quote
) {
    if (ash_is_line_continuation_at(
            lexer->input,
            lexer->length,
            lexer->offset
        )) {
        return true;
    }

    char ch = ash_lexer_peek(lexer, 0u);
    if (quote == ASH_QUOTE_NONE) {
        return ash_lexer_word_boundary(lexer) ||
            ch == '\\' || ch == '\'' || ch == '"' ||
            ch == '$' || ch == '`';
    }

    assert(quote == ASH_QUOTE_DOUBLE);
    if (ash_lexer_at_end(lexer) ||
        ch == '"' || ch == '$' || ch == '`') {
        return true;
    }
    char next = ash_lexer_peek(lexer, 1u);
    return ch == '\\' && next != '\0' &&
        strchr("$`\"\\", next) != NULL;
}

static enum ash_lexer_result ash_lexer_scan_text(
    struct ash_lexer* lexer,
    struct ash_word* word,
    enum ash_quote_kind quote
) {
    struct ash_source_location location =
        ash_lexer_current_location(lexer);
    bool produced = false;
    while (true) {
        size_t start = lexer->offset;
        while (!ash_lexer_text_boundary(lexer, quote)) {
            (void)ash_lexer_advance(lexer);
        }

        size_t length = lexer->offset - start;
        if (length != 0u) {
            int result = produced ?
                ash_word_extend_span(
                    word,
                    ASH_WORD_TEXT,
                    quote,
                    lexer->input + start,
                    length
                ) :
                ash_word_append_span(
                    word,
                    ASH_WORD_TEXT,
                    quote,
                    location,
                    lexer->input + start,
                    length
                );
            if (result != 0) {
                return ash_lexer_fail(
                    lexer,
                    ASH_LEXER_ERROR,
                    location,
                    "out of memory"
                );
            }
            produced = true;
        }
        if (!ash_is_line_continuation_at(
                lexer->input,
                lexer->length,
                lexer->offset
            )) {
            return ASH_LEXER_TOKEN;
        }
        ash_lexer_skip_line_continuations(lexer);
    }
}

static enum ash_lexer_result ash_lexer_scan_double_quote(
    struct ash_lexer* lexer,
    struct ash_word* word
) {
    struct ash_source_location location =
        ash_lexer_current_location(lexer);
    (void)ash_lexer_advance(lexer);
    bool produced_part = false;

    while (!ash_lexer_at_end(lexer)) {
        ash_lexer_skip_line_continuations(lexer);
        if (ash_lexer_at_end(lexer)) {
            break;
        }
        char ch = ash_lexer_peek(lexer, 0u);
        if (ch == '"') {
            if (!produced_part && ash_word_append_span(
                    word,
                    ASH_WORD_TEXT,
                    ASH_QUOTE_DOUBLE,
                    location,
                    "",
                    0u
                ) != 0) {
                return ash_lexer_fail(lexer, ASH_LEXER_ERROR, location, "out of memory");
            }
            (void)ash_lexer_advance(lexer);
            return ASH_LEXER_TOKEN;
        }
        if (ch == '\\') {
            struct ash_source_location escaped_location =
                ash_lexer_current_location(lexer);
            char next = ash_lexer_peek(lexer, 1u);
            if (next == '$' || next == '`' || next == '"' || next == '\\') {
                ash_lexer_advance_count(lexer, 2u);
                if (ash_word_append_span(
                        word,
                        ASH_WORD_TEXT,
                        ASH_QUOTE_BACKSLASH,
                        escaped_location,
                        &next,
                        1u
                    ) != 0) {
                    return ash_lexer_fail(
                        lexer,
                        ASH_LEXER_ERROR,
                        escaped_location,
                        "out of memory"
                    );
                }
                produced_part = true;
                continue;
            }
        }
        if (ch == '$') {
            enum ash_lexer_result result = ash_lexer_scan_dollar(
                lexer,
                word,
                ASH_QUOTE_DOUBLE
            );
            if (result != ASH_LEXER_TOKEN) {
                return result;
            }
            produced_part = true;
            continue;
        }
        if (ch == '`') {
            enum ash_lexer_result result = ash_lexer_scan_backquote(
                lexer,
                word,
                ASH_QUOTE_DOUBLE
            );
            if (result != ASH_LEXER_TOKEN) {
                return result;
            }
            produced_part = true;
            continue;
        }

        enum ash_lexer_result result =
            ash_lexer_scan_text(
                lexer,
                word,
                ASH_QUOTE_DOUBLE
            );
        if (result != ASH_LEXER_TOKEN) {
            return result;
        }
        produced_part = true;
    }

    return ash_lexer_fail(
        lexer,
        ASH_LEXER_INCOMPLETE,
        location,
        "unterminated double quote"
    );
}

static enum ash_lexer_result ash_lexer_scan_word(
    struct ash_lexer* lexer,
    struct ash_token* token
) {
    struct ash_source_location location =
        ash_lexer_current_location(lexer);
    token->kind = ASH_TOKEN_WORD;
    token->location = location;
    ash_word_init(&token->word, location);

    while (true) {
        ash_lexer_skip_line_continuations(lexer);
        if (ash_lexer_word_boundary(lexer)) {
            break;
        }
        char ch = ash_lexer_peek(lexer, 0u);
        enum ash_lexer_result result = ASH_LEXER_TOKEN;
        if (ch == '\\') {
            struct ash_source_location escaped_location =
                ash_lexer_current_location(lexer);
            (void)ash_lexer_advance(lexer);
            if (ash_lexer_at_end(lexer)) {
                const char backslash = '\\';
                if (ash_word_append_span(
                        &token->word,
                        ASH_WORD_TEXT,
                        ASH_QUOTE_NONE,
                        escaped_location,
                        &backslash,
                        1u
                    ) != 0) {
                    return ash_lexer_fail(
                        lexer,
                        ASH_LEXER_ERROR,
                        escaped_location,
                        "out of memory"
                    );
                }
                continue;
            }
            char escaped = ash_lexer_advance(lexer);
            if (ash_word_append_span(
                    &token->word,
                    ASH_WORD_TEXT,
                    ASH_QUOTE_BACKSLASH,
                    escaped_location,
                    &escaped,
                    1u
                ) != 0) {
                result = ash_lexer_fail(
                    lexer,
                    ASH_LEXER_ERROR,
                    escaped_location,
                    "out of memory"
                );
            }
        }
        else if (ch == '\'') {
            result = ash_lexer_scan_single_quote(lexer, &token->word);
        }
        else if (ch == '"' ) {
            result = ash_lexer_scan_double_quote(lexer, &token->word);
        }
        else if (ch == '$' && ash_lexer_peek(lexer, 1u) == '\'') {
            result = ash_lexer_scan_dollar_single_quote(lexer, &token->word);
        }
        else if (ch == '$') {
            result = ash_lexer_scan_dollar(lexer, &token->word, ASH_QUOTE_NONE);
        }
        else if (ch == '`') {
            result = ash_lexer_scan_backquote(lexer, &token->word, ASH_QUOTE_NONE);
        }
        else {
            result = ash_lexer_scan_text(
                lexer,
                &token->word,
                ASH_QUOTE_NONE
            );
        }

        if (result != ASH_LEXER_TOKEN) {
            ash_token_destroy(token);
            return result;
        }
    }

    return ASH_LEXER_TOKEN;
}

struct ash_io_redirect_match {
    enum ash_token_kind kind;
    size_t text_length;
};

static bool ash_lexer_match_io_redirect(
    const struct ash_lexer* lexer,
    struct ash_io_redirect_match* match
) {
    *match = (struct ash_io_redirect_match){0};
    size_t position = lexer->offset;
    char ch = ash_lexer_peek_logical_at(lexer, &position);
    if (ch >= '0' && ch <= '9') {
        do {
            match->text_length++;
            position++;
            ch = ash_lexer_peek_logical_at(lexer, &position);
        } while (ch >= '0' && ch <= '9');
        if (ch == '<' || ch == '>') {
            match->kind = ASH_TOKEN_IO_NUMBER;
            return true;
        }
        *match = (struct ash_io_redirect_match){0};
        return false;
    }

    if (ch != '{') {
        return false;
    }
    position++;
    ch = ash_lexer_peek_logical_at(lexer, &position);
    if (!ash_is_name_start((unsigned char)ch)) {
        return false;
    }
    do {
        match->text_length++;
        position++;
        ch = ash_lexer_peek_logical_at(lexer, &position);
    } while (ash_is_name_char((unsigned char)ch));
    if (ch != '}') {
        *match = (struct ash_io_redirect_match){0};
        return false;
    }
    position++;
    ch = ash_lexer_peek_logical_at(lexer, &position);
    if (ch != '<' && ch != '>') {
        *match = (struct ash_io_redirect_match){0};
        return false;
    }
    match->kind = ASH_TOKEN_IO_VARIABLE;
    return true;
}

static enum ash_lexer_result ash_lexer_scan_io_redirect(
    struct ash_lexer* lexer,
    struct ash_token* token,
    const struct ash_io_redirect_match* match
) {
    struct ash_source_location location =
        ash_lexer_current_location(lexer);
    char* text = malloc(match->text_length + 1u);
    if (text == NULL) {
        return ash_lexer_fail(lexer, ASH_LEXER_ERROR, location, "out of memory");
    }
    if (match->kind == ASH_TOKEN_IO_VARIABLE) {
        (void)ash_lexer_advance_logical(lexer);
    }
    for (size_t i = 0u; i < match->text_length; i++) {
        text[i] = ash_lexer_advance_logical(lexer);
    }
    if (match->kind == ASH_TOKEN_IO_VARIABLE) {
        (void)ash_lexer_advance_logical(lexer);
    }
    text[match->text_length] = '\0';

    token->kind = match->kind;
    token->location = location;
    token->io_redirect = text;
    return ASH_LEXER_TOKEN;
}

enum ash_lexer_result ash_lexer_next(struct ash_lexer* lexer, struct ash_token* token) {
    *token = (struct ash_token){0};
    lexer->error = NULL;

    while (true) {
        ash_lexer_skip_line_continuations(lexer);
        while (ash_is_blank(ash_lexer_peek_logical(lexer, 0u))) {
            ash_lexer_skip_line_continuations(lexer);
            (void)ash_lexer_advance(lexer);
        }
        ash_lexer_skip_line_continuations(lexer);
        if (ash_lexer_peek(lexer, 0u) == '#') {
            while (!ash_lexer_at_end(lexer) && ash_lexer_peek(lexer, 0u) != '\n') {
                (void)ash_lexer_advance(lexer);
            }
            continue;
        }
        break;
    }

    token->location = ash_lexer_current_location(lexer);
    if (ash_lexer_at_end(lexer)) {
        token->kind = ASH_TOKEN_EOF;
        return ASH_LEXER_END;
    }

    struct ash_io_redirect_match io_redirect;
    if (ash_lexer_match_io_redirect(lexer, &io_redirect)) {
        return ash_lexer_scan_io_redirect(
            lexer,
            token,
            &io_redirect
        );
    }

    const struct ash_operator* operator = ash_lexer_operator(lexer);
    if (operator != NULL) {
        token->kind = operator->kind;
        for (size_t i = 0u; i < operator->length; i++) {
            (void)ash_lexer_advance_logical(lexer);
        }
        return ASH_LEXER_TOKEN;
    }

    return ash_lexer_scan_word(lexer, token);
}

const char* ash_token_kind_name(enum ash_token_kind kind) {
    switch (kind) {
        case ASH_TOKEN_EOF:
            return "end of input";
        case ASH_TOKEN_WORD:
            return "word";
        case ASH_TOKEN_IO_NUMBER:
            return "IO number";
        case ASH_TOKEN_IO_VARIABLE:
            return "IO variable";
        case ASH_TOKEN_NEWLINE:
            return "newline";
        default:
            break;
    }
    for (size_t i = 0u;
         i < sizeof(ash_operators) / sizeof(ash_operators[0]);
         i++) {
        if (ash_operators[i].kind == kind) {
            return ash_operators[i].text;
        }
    }
    return "unknown token";
}
