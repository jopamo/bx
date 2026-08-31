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

static struct ash_source_location ash_lexer_location(const struct ash_lexer* lexer) {
    return (struct ash_source_location){
        .source = lexer->source_name,
        .line = lexer->line,
        .column = lexer->column,
        .offset = lexer->offset,
    };
}

void ash_lexer_init(
    struct ash_lexer* lexer,
    const char* source_name,
    const char* input,
    size_t length
) {
    *lexer = (struct ash_lexer){
        .source_name = (source_name != NULL) ? source_name : "<input>",
        .input = input,
        .length = length,
        .line = 1u,
        .column = 1u,
    };
}

void ash_token_destroy(struct ash_token* token) {
    if (token == NULL) {
        return;
    }
    ash_word_destroy(&token->word);
    free(token->io_number);
    *token = (struct ash_token){0};
}

static bool ash_lexer_at_end(const struct ash_lexer* lexer) {
    return lexer->offset == lexer->length;
}

static char ash_lexer_peek(const struct ash_lexer* lexer, size_t distance) {
    if (distance > lexer->length - lexer->offset) {
        return '\0';
    }
    size_t position = lexer->offset + distance;
    return (position < lexer->length) ? lexer->input[position] : '\0';
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
    size_t length = strlen(text);
    return length <= lexer->length - lexer->offset &&
        memcmp(lexer->input + lexer->offset, text, length) == 0;
}

static void ash_lexer_advance_count(struct ash_lexer* lexer, size_t count) {
    for (size_t i = 0u; i < count; i++) {
        (void)ash_lexer_advance(lexer);
    }
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
    return ch == ' ' || ch == '\t' || ch == '\r';
}

static const struct ash_operator* ash_lexer_operator(const struct ash_lexer* lexer) {
    static const struct ash_operator operators[] = {
        {";;&", 3u, ASH_TOKEN_DSEMI_AND},
        {"<<-", 3u, ASH_TOKEN_DLESS_DASH},
        {"<<<", 3u, ASH_TOKEN_TLESS},
        {"&&", 2u, ASH_TOKEN_AND_IF},
        {"||", 2u, ASH_TOKEN_OR_IF},
        {";;", 2u, ASH_TOKEN_DSEMI},
        {";&", 2u, ASH_TOKEN_SEMI_AND},
        {"|&", 2u, ASH_TOKEN_PIPE_AND},
        {"<<", 2u, ASH_TOKEN_DLESS},
        {">>", 2u, ASH_TOKEN_DGREAT},
        {"<&", 2u, ASH_TOKEN_LESS_AND},
        {">&", 2u, ASH_TOKEN_GREAT_AND},
        {"<>", 2u, ASH_TOKEN_LESS_GREAT},
        {">|", 2u, ASH_TOKEN_CLOBBER},
        {"|", 1u, ASH_TOKEN_PIPE},
        {"&", 1u, ASH_TOKEN_AMP},
        {";", 1u, ASH_TOKEN_SEMI},
        {"(", 1u, ASH_TOKEN_LPAREN},
        {")", 1u, ASH_TOKEN_RPAREN},
        {"<", 1u, ASH_TOKEN_LESS},
        {">", 1u, ASH_TOKEN_GREAT},
    };

    for (size_t i = 0u; i < sizeof(operators) / sizeof(operators[0]); i++) {
        const struct ash_operator* candidate = &operators[i];
        if (candidate->length <= lexer->length - lexer->offset &&
            memcmp(lexer->input + lexer->offset, candidate->text, candidate->length) == 0) {
            return candidate;
        }
    }
    return NULL;
}

bool ash_token_is_redirection(enum ash_token_kind kind) {
    return kind >= ASH_TOKEN_LESS && kind <= ASH_TOKEN_CLOBBER;
}

static int ash_word_append_span(
    struct ash_word* word,
    enum ash_word_part_kind kind,
    enum ash_quote_kind quote,
    struct ash_source_location location,
    const char* text,
    size_t length
) {
    if (ash_word_append(word, kind, quote, location, text, length) != 0) {
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

static enum ash_lexer_result ash_lexer_scan_parameter(
    struct ash_lexer* lexer,
    struct ash_word* word,
    enum ash_quote_kind quote
) {
    struct ash_source_location location = ash_lexer_location(lexer);
    size_t start = lexer->offset;
    ash_lexer_advance_count(lexer, 2u);
    size_t depth = 1u;
    enum ash_quote_kind nested_quote = ASH_QUOTE_NONE;

    while (!ash_lexer_at_end(lexer)) {
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
            ash_lexer_advance_count(lexer, 2u);
            continue;
        }
        (void)ash_lexer_advance(lexer);
        if (ch == '}') {
            depth--;
            if (depth == 0u) {
                size_t length = lexer->offset - start;
                if (ash_word_append_span(
                        word,
                        ASH_WORD_PARAMETER,
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
    struct ash_source_location location = ash_lexer_location(lexer);
    size_t start = lexer->offset;
    ash_lexer_advance_count(lexer, arithmetic ? 3u : 2u);
    size_t depth = 1u;
    enum ash_quote_kind nested_quote = ASH_QUOTE_NONE;

    while (!ash_lexer_at_end(lexer)) {
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
        if (arithmetic) {
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

        size_t length = lexer->offset - start;
        if (ash_word_append_span(
                word,
                arithmetic ? ASH_WORD_ARITHMETIC : ASH_WORD_COMMAND_SUBSTITUTION,
                quote,
                location,
                lexer->input + start,
                length
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
    struct ash_source_location location = ash_lexer_location(lexer);
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

    struct ash_source_location location = ash_lexer_location(lexer);
    size_t start = lexer->offset;
    (void)ash_lexer_advance(lexer);
    char ch = ash_lexer_peek(lexer, 0u);
    if (ash_is_name_start((unsigned char)ch)) {
        do {
            (void)ash_lexer_advance(lexer);
            ch = ash_lexer_peek(lexer, 0u);
        } while (ash_is_name_char((unsigned char)ch));
    }
    else if ((ch >= '0' && ch <= '9') ||
             (ch != '\0' && strchr("@*#?-$!", ch) != NULL)) {
        (void)ash_lexer_advance(lexer);
    }
    else {
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

    size_t length = lexer->offset - start;
    if (ash_word_append_span(
            word,
            ASH_WORD_PARAMETER,
            quote,
            location,
            lexer->input + start,
            length
        ) != 0) {
        return ash_lexer_fail(lexer, ASH_LEXER_ERROR, location, "out of memory");
    }
    return ASH_LEXER_TOKEN;
}

static enum ash_lexer_result ash_lexer_scan_single_quote(
    struct ash_lexer* lexer,
    struct ash_word* word
) {
    struct ash_source_location location = ash_lexer_location(lexer);
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
    struct ash_source_location location = ash_lexer_location(lexer);
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

static enum ash_lexer_result ash_lexer_scan_double_quote(
    struct ash_lexer* lexer,
    struct ash_word* word
) {
    struct ash_source_location location = ash_lexer_location(lexer);
    (void)ash_lexer_advance(lexer);
    bool produced_part = false;

    while (!ash_lexer_at_end(lexer)) {
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
            struct ash_source_location escaped_location = ash_lexer_location(lexer);
            char next = ash_lexer_peek(lexer, 1u);
            if (next == '\n') {
                ash_lexer_advance_count(lexer, 2u);
                continue;
            }
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

        struct ash_source_location text_location = ash_lexer_location(lexer);
        (void)ash_lexer_advance(lexer);
        if (ash_word_append_span(
                word,
                ASH_WORD_TEXT,
                ASH_QUOTE_DOUBLE,
                text_location,
                &ch,
                1u
            ) != 0) {
            return ash_lexer_fail(lexer, ASH_LEXER_ERROR, text_location, "out of memory");
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

static bool ash_lexer_word_boundary(const struct ash_lexer* lexer) {
    char ch = ash_lexer_peek(lexer, 0u);
    return ch == '\0' || ch == '\n' || ash_is_blank(ch) ||
        ash_lexer_operator(lexer) != NULL;
}

static enum ash_lexer_result ash_lexer_scan_word(
    struct ash_lexer* lexer,
    struct ash_token* token
) {
    struct ash_source_location location = ash_lexer_location(lexer);
    token->kind = ASH_TOKEN_WORD;
    token->location = location;
    ash_word_init(&token->word, location);

    while (!ash_lexer_word_boundary(lexer)) {
        char ch = ash_lexer_peek(lexer, 0u);
        enum ash_lexer_result result = ASH_LEXER_TOKEN;
        if (ch == '\\') {
            struct ash_source_location escaped_location = ash_lexer_location(lexer);
            (void)ash_lexer_advance(lexer);
            if (ash_lexer_at_end(lexer)) {
                return ash_lexer_fail(
                    lexer,
                    ASH_LEXER_INCOMPLETE,
                    escaped_location,
                    "trailing backslash"
                );
            }
            char escaped = ash_lexer_advance(lexer);
            if (escaped == '\n') {
                continue;
            }
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
            struct ash_source_location text_location = ash_lexer_location(lexer);
            (void)ash_lexer_advance(lexer);
            if (ash_word_append_span(
                    &token->word,
                    ASH_WORD_TEXT,
                    ASH_QUOTE_NONE,
                    text_location,
                    &ch,
                    1u
                ) != 0) {
                result = ash_lexer_fail(
                    lexer,
                    ASH_LEXER_ERROR,
                    text_location,
                    "out of memory"
                );
            }
        }

        if (result != ASH_LEXER_TOKEN) {
            ash_token_destroy(token);
            return result;
        }
    }

    return ASH_LEXER_TOKEN;
}

static enum ash_lexer_result ash_lexer_scan_io_number(
    struct ash_lexer* lexer,
    struct ash_token* token
) {
    size_t start = lexer->offset;
    struct ash_source_location location = ash_lexer_location(lexer);
    while (ash_lexer_peek(lexer, 0u) >= '0' && ash_lexer_peek(lexer, 0u) <= '9') {
        (void)ash_lexer_advance(lexer);
    }
    size_t length = lexer->offset - start;

    char* number = malloc(length + 1u);
    if (number == NULL) {
        return ash_lexer_fail(lexer, ASH_LEXER_ERROR, location, "out of memory");
    }
    memcpy(number, lexer->input + start, length);
    number[length] = '\0';

    token->kind = ASH_TOKEN_IO_NUMBER;
    token->location = location;
    token->io_number = number;
    return ASH_LEXER_TOKEN;
}

enum ash_lexer_result ash_lexer_next(struct ash_lexer* lexer, struct ash_token* token) {
    *token = (struct ash_token){0};
    lexer->error = NULL;

    while (true) {
        if (ash_lexer_peek(lexer, 0u) == '\\' &&
            ash_lexer_peek(lexer, 1u) == '\n') {
            ash_lexer_advance_count(lexer, 2u);
            continue;
        }
        while (ash_is_blank(ash_lexer_peek(lexer, 0u))) {
            (void)ash_lexer_advance(lexer);
        }
        if (ash_lexer_peek(lexer, 0u) == '#') {
            while (!ash_lexer_at_end(lexer) && ash_lexer_peek(lexer, 0u) != '\n') {
                (void)ash_lexer_advance(lexer);
            }
            continue;
        }
        break;
    }

    token->location = ash_lexer_location(lexer);
    if (ash_lexer_at_end(lexer)) {
        token->kind = ASH_TOKEN_EOF;
        return ASH_LEXER_END;
    }
    if (ash_lexer_peek(lexer, 0u) == '\n') {
        token->kind = ASH_TOKEN_NEWLINE;
        (void)ash_lexer_advance(lexer);
        return ASH_LEXER_TOKEN;
    }

    size_t digit_length = 0u;
    while (ash_lexer_peek(lexer, digit_length) >= '0' &&
           ash_lexer_peek(lexer, digit_length) <= '9') {
        digit_length++;
    }
    char after_digits = ash_lexer_peek(lexer, digit_length);
    if (digit_length != 0u && (after_digits == '<' || after_digits == '>')) {
        return ash_lexer_scan_io_number(lexer, token);
    }

    const struct ash_operator* operator = ash_lexer_operator(lexer);
    if (operator != NULL) {
        token->kind = operator->kind;
        ash_lexer_advance_count(lexer, operator->length);
        return ASH_LEXER_TOKEN;
    }

    return ash_lexer_scan_word(lexer, token);
}

const char* ash_token_kind_name(enum ash_token_kind kind) {
    static const char* const names[] = {
        [ASH_TOKEN_EOF] = "end of input",
        [ASH_TOKEN_WORD] = "word",
        [ASH_TOKEN_IO_NUMBER] = "IO number",
        [ASH_TOKEN_NEWLINE] = "newline",
        [ASH_TOKEN_AND_IF] = "&&",
        [ASH_TOKEN_OR_IF] = "||",
        [ASH_TOKEN_DSEMI] = ";;",
        [ASH_TOKEN_SEMI_AND] = ";&",
        [ASH_TOKEN_DSEMI_AND] = ";;&",
        [ASH_TOKEN_PIPE] = "|",
        [ASH_TOKEN_PIPE_AND] = "|&",
        [ASH_TOKEN_AMP] = "&",
        [ASH_TOKEN_SEMI] = ";",
        [ASH_TOKEN_LPAREN] = "(",
        [ASH_TOKEN_RPAREN] = ")",
        [ASH_TOKEN_LESS] = "<",
        [ASH_TOKEN_GREAT] = ">",
        [ASH_TOKEN_DLESS] = "<<",
        [ASH_TOKEN_DLESS_DASH] = "<<-",
        [ASH_TOKEN_TLESS] = "<<<",
        [ASH_TOKEN_DGREAT] = ">>",
        [ASH_TOKEN_LESS_AND] = "<&",
        [ASH_TOKEN_GREAT_AND] = ">&",
        [ASH_TOKEN_LESS_GREAT] = "<>",
        [ASH_TOKEN_CLOBBER] = ">|",
    };
    if ((size_t)kind >= sizeof(names) / sizeof(names[0]) || names[kind] == NULL) {
        return "unknown token";
    }
    return names[kind];
}
