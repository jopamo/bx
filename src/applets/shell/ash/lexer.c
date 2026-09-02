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

bool ash_lexer_options_valid(
    const struct ash_lexer_options* options
) {
    return options != NULL &&
        (options->flags & ~(uint32_t)ASH_LEXER_FLAG_ALL) == 0u;
}

void ash_lexer_init_at_with_options(
    struct ash_lexer* lexer,
    struct ash_source_location origin,
    const char* input,
    size_t length,
    const struct ash_lexer_options* options
) {
    assert(lexer != NULL);
    assert(ash_source_location_valid(&origin));
    assert(input != NULL);
    assert(origin.offset <= SIZE_MAX - length);
    assert(ash_lexer_options_valid(options));
    *lexer = (struct ash_lexer){
        .source_name = origin.source,
        .source_identity = origin.identity,
        .input = input,
        .length = length,
        .source_offset = origin.offset,
        .line = origin.line,
        .column = origin.column,
        .options = *options,
    };
}

void ash_lexer_init_at(
    struct ash_lexer* lexer,
    struct ash_source_location origin,
    const char* input,
    size_t length
) {
    ash_lexer_init_at_with_options(
        lexer,
        origin,
        input,
        length,
        &(const struct ash_lexer_options){0}
    );
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

static void ash_lexer_skip_comment(struct ash_lexer* lexer) {
    assert(ash_lexer_peek(lexer, 0u) == '#');
    lexer->discarded_comment = true;
    while (!ash_lexer_at_end(lexer) &&
           ash_lexer_peek(lexer, 0u) != '\n') {
        (void)ash_lexer_advance(lexer);
    }
    lexer->ended_in_comment = ash_lexer_at_end(lexer);
}

/*
 * Legacy backquotes close on an unescaped backquote even when that byte is in
 * text the nested command will later discard as a comment. Stop there rather
 * than skipping to newline; all other comment bytes are lexically inert.
 */
static void ash_lexer_skip_backquote_comment(
    struct ash_lexer* lexer
) {
    assert(ash_lexer_peek(lexer, 0u) == '#');
    while (!ash_lexer_at_end(lexer)) {
        char ch = ash_lexer_peek(lexer, 0u);
        if (ch == '\n' || ch == '`') {
            return;
        }
        if (ch == '\\' &&
            ash_lexer_peek(lexer, 1u) != '\0' &&
            ash_lexer_peek(lexer, 1u) != '\n') {
            ash_lexer_advance_count(lexer, 2u);
        }
        else {
            (void)ash_lexer_advance(lexer);
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

bool ash_lexer_ended_in_comment(const struct ash_lexer* lexer) {
    return lexer != NULL && lexer->ended_in_comment;
}

bool ash_lexer_discarded_comment(const struct ash_lexer* lexer) {
    return lexer != NULL && lexer->discarded_comment;
}

bool ash_lexer_discard_comment_tail(struct ash_lexer* lexer) {
    if (lexer == NULL) {
        return false;
    }
    while (!ash_lexer_at_end(lexer) &&
           ash_lexer_peek(lexer, 0u) != '\n') {
        (void)ash_lexer_advance(lexer);
    }
    return ash_lexer_at_end(lexer);
}

void ash_lexer_discard_remaining(struct ash_lexer* lexer) {
    if (lexer == NULL) {
        return;
    }
    while (!ash_lexer_at_end(lexer)) {
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

static bool ash_operator_kind_can_extend(enum ash_token_kind kind) {
    const struct ash_operator* token_operator = NULL;
    for (size_t i = 0u;
         i < sizeof(ash_operators) / sizeof(ash_operators[0]);
         i++) {
        if (ash_operators[i].kind == kind) {
            token_operator = &ash_operators[i];
            break;
        }
    }
    if (token_operator == NULL) {
        return false;
    }
    for (size_t i = 0u;
         i < sizeof(ash_operators) / sizeof(ash_operators[0]);
         i++) {
        const struct ash_operator* candidate = &ash_operators[i];
        if (candidate->length > token_operator->length &&
            memcmp(
                candidate->text,
                token_operator->text,
                token_operator->length
            ) == 0) {
            return true;
        }
    }
    return false;
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

enum ash_matched_frame_kind {
    ASH_MATCH_PARAMETER = 0,
    ASH_MATCH_COMMAND,
    ASH_MATCH_ARITHMETIC,
    ASH_MATCH_PROCESS,
    ASH_MATCH_PAREN,
    ASH_MATCH_BACKQUOTE,
    ASH_MATCH_SINGLE_QUOTE,
    ASH_MATCH_DOUBLE_QUOTE,
    ASH_MATCH_ANSI_C_QUOTE,
    ASH_MATCH_LOCALE_QUOTE,
    ASH_MATCH_COUNT,
};

#define ASH_MATCH_INLINE_FRAMES 8u
struct ash_matched_frame {
    enum ash_matched_frame_kind kind;
    bool comments_enabled;
    bool comment_eligible;
    enum ash_quote_kind backquote_quote;
};

struct ash_matched_stack {
    struct ash_matched_frame* frames;
    size_t count;
    size_t capacity;
    struct ash_matched_frame inline_frames[ASH_MATCH_INLINE_FRAMES];
};
#undef ASH_MATCH_INLINE_FRAMES

static void ash_matched_stack_init(struct ash_matched_stack* stack) {
    *stack = (struct ash_matched_stack){
        .frames = stack->inline_frames,
        .capacity = sizeof(stack->inline_frames) /
            sizeof(stack->inline_frames[0]),
    };
}

static void ash_matched_stack_destroy(struct ash_matched_stack* stack) {
    if (stack->frames != stack->inline_frames) {
        free(stack->frames);
    }
    *stack = (struct ash_matched_stack){0};
}

static int ash_matched_stack_push(
    struct ash_matched_stack* stack,
    enum ash_matched_frame_kind kind
) {
    assert(kind >= ASH_MATCH_PARAMETER && kind < ASH_MATCH_COUNT);
    bool comments_enabled =
        kind == ASH_MATCH_COMMAND ||
        kind == ASH_MATCH_PROCESS ||
        kind == ASH_MATCH_BACKQUOTE;
    if (stack->count != 0u) {
        struct ash_matched_frame* parent =
            &stack->frames[stack->count - 1u];
        if (kind == ASH_MATCH_PAREN) {
            comments_enabled = parent->comments_enabled;
            parent->comment_eligible = parent->comments_enabled;
        }
        else if (parent->comments_enabled) {
            parent->comment_eligible = false;
        }
    }
    if (stack->count == stack->capacity) {
        if (stack->capacity > SIZE_MAX / 2u ||
            stack->capacity * 2u >
                SIZE_MAX / sizeof(*stack->frames)) {
            errno = ENOMEM;
            return -1;
        }
        size_t capacity = stack->capacity * 2u;
        struct ash_matched_frame* replacement;
        if (stack->frames == stack->inline_frames) {
            replacement = malloc(capacity * sizeof(*replacement));
            if (replacement != NULL) {
                memcpy(
                    replacement,
                    stack->inline_frames,
                    stack->count * sizeof(*replacement)
                );
            }
        }
        else {
            replacement = realloc(
                stack->frames,
                capacity * sizeof(*replacement)
            );
        }
        if (replacement == NULL) {
            errno = ENOMEM;
            return -1;
        }
        stack->frames = replacement;
        stack->capacity = capacity;
    }
    stack->frames[stack->count++] = (struct ash_matched_frame){
        .kind = kind,
        .comments_enabled = comments_enabled,
        .comment_eligible = comments_enabled,
    };
    return 0;
}

static size_t ash_matched_opener_length(
    enum ash_matched_frame_kind frame
) {
    switch (frame) {
        case ASH_MATCH_PARAMETER:
        case ASH_MATCH_COMMAND:
        case ASH_MATCH_PROCESS:
        case ASH_MATCH_ANSI_C_QUOTE:
        case ASH_MATCH_LOCALE_QUOTE:
            return 2u;
        case ASH_MATCH_ARITHMETIC:
            return 3u;
        case ASH_MATCH_PAREN:
        case ASH_MATCH_BACKQUOTE:
        case ASH_MATCH_SINGLE_QUOTE:
        case ASH_MATCH_DOUBLE_QUOTE:
            return 1u;
        case ASH_MATCH_COUNT:
            break;
    }
    return 0u;
}

static int ash_lexer_push_matched_frame(
    struct ash_lexer* lexer,
    struct ash_matched_stack* stack,
    enum ash_matched_frame_kind frame
) {
    if (ash_matched_stack_push(stack, frame) != 0) {
        return -1;
    }
    for (size_t i = 0u; i < ash_matched_opener_length(frame); i++) {
        (void)ash_lexer_advance_logical(lexer);
    }
    return 0;
}

static bool ash_lexer_dollar_frame(
    const struct ash_lexer* lexer,
    bool quote_forms,
    enum ash_matched_frame_kind* frame
) {
    if (ash_lexer_starts_with(lexer, "$((")) {
        *frame = ASH_MATCH_ARITHMETIC;
        return true;
    }
    if (ash_lexer_starts_with(lexer, "${")) {
        *frame = ASH_MATCH_PARAMETER;
        return true;
    }
    if (ash_lexer_starts_with(lexer, "$(")) {
        *frame = ASH_MATCH_COMMAND;
        return true;
    }
    if (quote_forms && ash_lexer_starts_with(lexer, "$'")) {
        *frame = ASH_MATCH_ANSI_C_QUOTE;
        return true;
    }
    if (quote_forms && ash_lexer_starts_with(lexer, "$\"")) {
        *frame = ASH_MATCH_LOCALE_QUOTE;
        return true;
    }
    return false;
}

static bool ash_lexer_process_substitution_at(
    const struct ash_lexer* lexer,
    size_t position
) {
    char direction = ash_lexer_peek_logical_at(lexer, &position);
    if (direction != '<' && direction != '>') {
        return false;
    }
    position++;
    return ash_lexer_peek_logical_at(lexer, &position) == '(';
}

static bool ash_lexer_starts_process_substitution(
    const struct ash_lexer* lexer
) {
    return ash_lexer_process_substitution_at(lexer, lexer->offset);
}

static bool ash_lexer_starts_extglob(
    const struct ash_lexer* lexer
) {
    if ((lexer->options.flags & ASH_LEXER_EXTGLOB) == 0u) {
        return false;
    }
    char operator = ash_lexer_peek_logical(lexer, 0u);
    return operator != '\0' &&
        strchr("?*+@!", operator) != NULL &&
        ash_lexer_peek_logical(lexer, 1u) == '(';
}

static const char* ash_matched_error(
    enum ash_matched_frame_kind root
) {
    switch (root) {
        case ASH_MATCH_PARAMETER:
            return "unterminated parameter expansion";
        case ASH_MATCH_COMMAND:
            return "unterminated command substitution";
        case ASH_MATCH_ARITHMETIC:
            return "unterminated arithmetic expansion";
        case ASH_MATCH_PROCESS:
            return "unterminated process substitution";
        case ASH_MATCH_BACKQUOTE:
            return "unterminated backquote substitution";
        case ASH_MATCH_PAREN:
        case ASH_MATCH_SINGLE_QUOTE:
        case ASH_MATCH_DOUBLE_QUOTE:
        case ASH_MATCH_ANSI_C_QUOTE:
        case ASH_MATCH_LOCALE_QUOTE:
        case ASH_MATCH_COUNT:
            break;
    }
    return "unterminated shell construct";
}

static int ash_word_append_matched_span(
    struct ash_word* word,
    enum ash_quote_kind quote,
    enum ash_matched_frame_kind root,
    struct ash_source_location location,
    const struct ash_lexer* lexer,
    size_t start,
    size_t body_start,
    size_t body_end
) {
    if (root == ASH_MATCH_PARAMETER) {
        return ash_word_append_parameter_span(
            word,
            quote,
            location,
            lexer->input + start,
            lexer->offset - start
        );
    }
    if (root == ASH_MATCH_BACKQUOTE) {
        return ash_word_append_span(
            word,
            ASH_WORD_BACKQUOTE,
            quote,
            location,
            lexer->input + start,
            lexer->offset - start
        );
    }

    enum ash_word_part_kind kind;
    const char* prefix;
    const char* suffix;
    switch (root) {
        case ASH_MATCH_COMMAND:
            kind = ASH_WORD_COMMAND_SUBSTITUTION;
            prefix = "$(";
            suffix = ")";
            break;
        case ASH_MATCH_ARITHMETIC:
            kind = ASH_WORD_ARITHMETIC;
            prefix = "$((";
            suffix = "))";
            break;
        case ASH_MATCH_PROCESS:
            kind = ASH_WORD_PROCESS_SUBSTITUTION;
            prefix = lexer->input[start] == '<' ? "<(" : ">(";
            suffix = ")";
            break;
        case ASH_MATCH_PARAMETER:
        case ASH_MATCH_PAREN:
        case ASH_MATCH_BACKQUOTE:
        case ASH_MATCH_SINGLE_QUOTE:
        case ASH_MATCH_DOUBLE_QUOTE:
        case ASH_MATCH_ANSI_C_QUOTE:
        case ASH_MATCH_LOCALE_QUOTE:
        case ASH_MATCH_COUNT:
            errno = EINVAL;
            return -1;
    }

    return ash_word_append_span(
            word,
            kind,
            quote,
            location,
            prefix,
            strlen(prefix)
        ) != 0 ||
        ash_word_extend_span(
            word,
            kind,
            quote,
            lexer->input + body_start,
            body_end - body_start
        ) != 0 ||
        ash_word_extend_span(
            word,
            kind,
            quote,
            suffix,
            strlen(suffix)
        ) != 0 ? -1 : 0;
}

static bool ash_matched_comment_separator(char ch) {
    return ash_is_blank(ch) || ch == '\n' ||
        strchr("&|;<>", ch) != NULL;
}

/*
 * Matched shell constructs share one iterative scanner. The explicit frame
 * stack makes nesting depth input-owned rather than C-stack-owned, while the
 * inline frame storage keeps the ordinary non-nested path allocation-free.
 */
static enum ash_lexer_result ash_lexer_scan_matched(
    struct ash_lexer* lexer,
    struct ash_word* word,
    enum ash_quote_kind quote,
    enum ash_matched_frame_kind root
) {
    assert(
        root == ASH_MATCH_PARAMETER ||
        root == ASH_MATCH_COMMAND ||
        root == ASH_MATCH_ARITHMETIC ||
        root == ASH_MATCH_PROCESS ||
        root == ASH_MATCH_BACKQUOTE
    );
    struct ash_source_location location =
        ash_lexer_current_location(lexer);
    size_t start = lexer->offset;
    struct ash_matched_stack stack;
    ash_matched_stack_init(&stack);
    if (ash_lexer_push_matched_frame(lexer, &stack, root) != 0) {
        ash_matched_stack_destroy(&stack);
        return ash_lexer_fail(
            lexer,
            ASH_LEXER_ERROR,
            location,
            "out of memory"
        );
    }
    size_t body_start = lexer->offset;
    size_t body_end = body_start;

    while (stack.count != 0u) {
        struct ash_matched_frame* active =
            &stack.frames[stack.count - 1u];
        enum ash_matched_frame_kind frame = active->kind;
        if (ash_lexer_at_end(lexer)) {
            break;
        }

        char ch = ash_lexer_peek(lexer, 0u);
        if (frame == ASH_MATCH_SINGLE_QUOTE) {
            (void)ash_lexer_advance(lexer);
            if (ch == '\'') {
                stack.count--;
            }
            continue;
        }
        if (frame == ASH_MATCH_ANSI_C_QUOTE) {
            if (ch == '\\' && ash_lexer_peek(lexer, 1u) != '\0') {
                ash_lexer_advance_count(lexer, 2u);
            }
            else {
                (void)ash_lexer_advance(lexer);
                if (ch == '\'') {
                    stack.count--;
                }
            }
            continue;
        }
        if (frame == ASH_MATCH_BACKQUOTE) {
            if (ash_is_line_continuation_at(
                    lexer->input,
                    lexer->length,
                    lexer->offset
                )) {
                ash_lexer_advance_count(lexer, 2u);
                continue;
            }
            if (ch == '\\' && ash_lexer_peek(lexer, 1u) != '\0') {
                ash_lexer_advance_count(lexer, 2u);
                active->comment_eligible = false;
                continue;
            }
            if (ch == '`') {
                if (stack.count == 1u) {
                    body_end = lexer->offset;
                }
                (void)ash_lexer_advance(lexer);
                stack.count--;
                continue;
            }

            if (active->backquote_quote == ASH_QUOTE_SINGLE) {
                (void)ash_lexer_advance(lexer);
                if (ch == '\'') {
                    active->backquote_quote = ASH_QUOTE_NONE;
                }
                continue;
            }
            if (active->backquote_quote == ASH_QUOTE_DOUBLE) {
                if (ch == '"') {
                    active->backquote_quote = ASH_QUOTE_NONE;
                    (void)ash_lexer_advance(lexer);
                    continue;
                }
                enum ash_matched_frame_kind nested;
                if (ch == '$' &&
                    ash_lexer_dollar_frame(lexer, false, &nested)) {
                    if (ash_lexer_push_matched_frame(
                            lexer,
                            &stack,
                            nested
                        ) != 0) {
                        goto out_of_memory;
                    }
                    continue;
                }
                (void)ash_lexer_advance(lexer);
                continue;
            }
            if (ch == '#' && active->comment_eligible) {
                ash_lexer_skip_backquote_comment(lexer);
                continue;
            }
            if (ch == '\'') {
                active->backquote_quote = ASH_QUOTE_SINGLE;
                active->comment_eligible = false;
                (void)ash_lexer_advance(lexer);
                continue;
            }
            if (ch == '"') {
                active->backquote_quote = ASH_QUOTE_DOUBLE;
                active->comment_eligible = false;
                (void)ash_lexer_advance(lexer);
                continue;
            }

            enum ash_matched_frame_kind nested;
            if (ch == '$' &&
                ash_lexer_dollar_frame(lexer, false, &nested)) {
                if (ash_lexer_push_matched_frame(
                        lexer,
                        &stack,
                        nested
                    ) != 0) {
                    goto out_of_memory;
                }
                continue;
            }
            if (ash_lexer_starts_process_substitution(lexer)) {
                if (ash_lexer_push_matched_frame(
                        lexer,
                        &stack,
                        ASH_MATCH_PROCESS
                    ) != 0) {
                    goto out_of_memory;
                }
                continue;
            }
            active->comment_eligible =
                ash_matched_comment_separator(ch);
            (void)ash_lexer_advance(lexer);
            continue;
        }

        ash_lexer_skip_line_continuations(lexer);
        if (ash_lexer_at_end(lexer)) {
            break;
        }
        ch = ash_lexer_peek(lexer, 0u);
        if (frame == ASH_MATCH_DOUBLE_QUOTE ||
            frame == ASH_MATCH_LOCALE_QUOTE) {
            if (ch == '\\') {
                char next = ash_lexer_peek(lexer, 1u);
                if (next != '\0' &&
                    strchr("$`\"\\", next) != NULL) {
                    ash_lexer_advance_count(lexer, 2u);
                }
                else {
                    (void)ash_lexer_advance(lexer);
                }
                continue;
            }
            if (ch == '"') {
                (void)ash_lexer_advance(lexer);
                stack.count--;
                continue;
            }

            enum ash_matched_frame_kind nested;
            if (ch == '$' &&
                ash_lexer_dollar_frame(lexer, false, &nested)) {
                if (ash_lexer_push_matched_frame(
                        lexer,
                        &stack,
                        nested
                    ) != 0) {
                    goto out_of_memory;
                }
                continue;
            }
            if (ch == '`') {
                if (ash_lexer_push_matched_frame(
                        lexer,
                        &stack,
                        ASH_MATCH_BACKQUOTE
                    ) != 0) {
                    goto out_of_memory;
                }
                continue;
            }
            (void)ash_lexer_advance(lexer);
            continue;
        }

        if (ch == '\\') {
            (void)ash_lexer_advance(lexer);
            if (!ash_lexer_at_end(lexer)) {
                (void)ash_lexer_advance(lexer);
            }
            if (active->comments_enabled) {
                active->comment_eligible = false;
            }
            continue;
        }
        if (ch == '#' &&
            active->comments_enabled &&
            active->comment_eligible) {
            ash_lexer_skip_comment(lexer);
            continue;
        }

        enum ash_matched_frame_kind nested;
        if (ch == '$' &&
            ash_lexer_dollar_frame(lexer, true, &nested)) {
            if (ash_lexer_push_matched_frame(
                    lexer,
                    &stack,
                    nested
                ) != 0) {
                goto out_of_memory;
            }
            continue;
        }
        if (ash_lexer_starts_process_substitution(lexer)) {
            if (ash_lexer_push_matched_frame(
                    lexer,
                    &stack,
                    ASH_MATCH_PROCESS
                ) != 0) {
                goto out_of_memory;
            }
            continue;
        }
        if (ch == '\'') {
            if (ash_lexer_push_matched_frame(
                    lexer,
                    &stack,
                    ASH_MATCH_SINGLE_QUOTE
                ) != 0) {
                goto out_of_memory;
            }
            continue;
        }
        if (ch == '"') {
            if (ash_lexer_push_matched_frame(
                    lexer,
                    &stack,
                    ASH_MATCH_DOUBLE_QUOTE
                ) != 0) {
                goto out_of_memory;
            }
            continue;
        }
        if (ch == '`') {
            if (ash_lexer_push_matched_frame(
                    lexer,
                    &stack,
                    ASH_MATCH_BACKQUOTE
                ) != 0) {
                goto out_of_memory;
            }
            continue;
        }

        bool root_closing = stack.count == 1u;
        if (frame == ASH_MATCH_PARAMETER && ch == '}') {
            if (root_closing) {
                body_end = lexer->offset;
            }
            (void)ash_lexer_advance(lexer);
            stack.count--;
            continue;
        }
        if (frame == ASH_MATCH_ARITHMETIC &&
            ash_lexer_starts_with(lexer, "))")) {
            if (root_closing) {
                body_end = lexer->offset;
            }
            (void)ash_lexer_advance_logical(lexer);
            (void)ash_lexer_advance_logical(lexer);
            stack.count--;
            continue;
        }
        if ((frame == ASH_MATCH_COMMAND ||
             frame == ASH_MATCH_PROCESS ||
             frame == ASH_MATCH_PAREN) &&
            ch == ')') {
            if (root_closing) {
                body_end = lexer->offset;
            }
            (void)ash_lexer_advance(lexer);
            stack.count--;
            continue;
        }
        if ((frame == ASH_MATCH_COMMAND ||
             frame == ASH_MATCH_ARITHMETIC ||
             frame == ASH_MATCH_PROCESS ||
             frame == ASH_MATCH_PAREN) &&
            ch == '(') {
            if (ash_lexer_push_matched_frame(
                    lexer,
                    &stack,
                    ASH_MATCH_PAREN
                ) != 0) {
                goto out_of_memory;
            }
            continue;
        }
        if (active->comments_enabled) {
            active->comment_eligible =
                ash_matched_comment_separator(ch);
        }
        (void)ash_lexer_advance(lexer);
    }

    if (stack.count != 0u) {
        ash_matched_stack_destroy(&stack);
        return ash_lexer_fail(
            lexer,
            ASH_LEXER_INCOMPLETE,
            location,
            ash_matched_error(root)
        );
    }
    ash_matched_stack_destroy(&stack);
    if (ash_word_append_matched_span(
            word,
            quote,
            root,
            location,
            lexer,
            start,
            body_start,
            body_end
        ) != 0) {
        return ash_lexer_fail(
            lexer,
            ASH_LEXER_ERROR,
            location,
            "out of memory"
        );
    }
    return ASH_LEXER_TOKEN;

out_of_memory:
    ash_matched_stack_destroy(&stack);
    return ash_lexer_fail(
        lexer,
        ASH_LEXER_ERROR,
        location,
        "out of memory"
    );
}

static enum ash_lexer_result ash_lexer_scan_dollar(
    struct ash_lexer* lexer,
    struct ash_word* word,
    enum ash_quote_kind quote
) {
    if (ash_lexer_starts_with(lexer, "${")) {
        return ash_lexer_scan_matched(
            lexer,
            word,
            quote,
            ASH_MATCH_PARAMETER
        );
    }
    if (ash_lexer_starts_with(lexer, "$((")) {
        return ash_lexer_scan_matched(
            lexer,
            word,
            quote,
            ASH_MATCH_ARITHMETIC
        );
    }
    if (ash_lexer_starts_with(lexer, "$(")) {
        return ash_lexer_scan_matched(
            lexer,
            word,
            quote,
            ASH_MATCH_COMMAND
        );
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
    (void)ash_lexer_advance_logical(lexer);
    (void)ash_lexer_advance_logical(lexer);
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
    if (ch == '\0' || ash_is_blank(ch)) {
        return true;
    }
    return !ash_lexer_starts_process_substitution(lexer) &&
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
        return ash_lexer_starts_extglob(lexer) ||
            ash_lexer_starts_process_substitution(lexer) ||
            ash_lexer_word_boundary(lexer) ||
            ch == '\\' || ch == '\'' || ch == '"' ||
            ch == '$' || ch == '`';
    }

    assert(quote == ASH_QUOTE_DOUBLE || quote == ASH_QUOTE_LOCALE);
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
    struct ash_word* word,
    enum ash_quote_kind quote
) {
    assert(quote == ASH_QUOTE_DOUBLE || quote == ASH_QUOTE_LOCALE);
    struct ash_source_location location =
        ash_lexer_current_location(lexer);
    if (quote == ASH_QUOTE_LOCALE) {
        (void)ash_lexer_advance_logical(lexer);
    }
    (void)ash_lexer_advance_logical(lexer);
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
                    quote,
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
                quote
            );
            if (result != ASH_LEXER_TOKEN) {
                return result;
            }
            produced_part = true;
            continue;
        }
        if (ch == '`') {
            enum ash_lexer_result result = ash_lexer_scan_matched(
                lexer,
                word,
                quote,
                ASH_MATCH_BACKQUOTE
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
                quote
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

static enum ash_lexer_result ash_lexer_scan_backslash(
    struct ash_lexer* lexer,
    struct ash_word* word
) {
    struct ash_source_location location =
        ash_lexer_current_location(lexer);
    (void)ash_lexer_advance(lexer);
    if (ash_lexer_at_end(lexer)) {
        const char backslash = '\\';
        return ash_word_append_span(
            word,
            ASH_WORD_TEXT,
            ASH_QUOTE_NONE,
            location,
            &backslash,
            1u
        ) == 0 ? ASH_LEXER_TOKEN :
            ash_lexer_fail(
                lexer,
                ASH_LEXER_ERROR,
                location,
                "out of memory"
            );
    }

    char escaped = ash_lexer_advance(lexer);
    return ash_word_append_span(
        word,
        ASH_WORD_TEXT,
        ASH_QUOTE_BACKSLASH,
        location,
        &escaped,
        1u
    ) == 0 ? ASH_LEXER_TOKEN :
        ash_lexer_fail(
            lexer,
            ASH_LEXER_ERROR,
            location,
            "out of memory"
        );
}

/*
 * Extglob parentheses suppress ordinary shell token boundaries, but the
 * contents remain part of the surrounding structured word. Scan the balanced
 * region iteratively so nested groups do not consume the C stack, while the
 * existing quote/substitution scanners continue to preserve semantic parts.
 */
static enum ash_lexer_result ash_lexer_scan_extglob(
    struct ash_lexer* lexer,
    struct ash_word* word
) {
    assert(ash_lexer_starts_extglob(lexer));
    struct ash_source_location opening =
        ash_lexer_current_location(lexer);
    char prefix[] = {
        ash_lexer_advance_logical(lexer),
        ash_lexer_advance_logical(lexer),
    };
    if (ash_word_append_span(
            word,
            ASH_WORD_TEXT,
            ASH_QUOTE_NONE,
            opening,
            prefix,
            sizeof(prefix)
        ) != 0) {
        return ash_lexer_fail(
            lexer,
            ASH_LEXER_ERROR,
            opening,
            "out of memory"
        );
    }
    size_t depth = 1u;

    while (true) {
        ash_lexer_skip_line_continuations(lexer);
        if (ash_lexer_at_end(lexer)) {
            return ash_lexer_fail(
                lexer,
                ASH_LEXER_INCOMPLETE,
                opening,
                "unterminated extended glob"
            );
        }

        char ch = ash_lexer_peek(lexer, 0u);
        enum ash_lexer_result result = ASH_LEXER_TOKEN;
        if (ch == '\\') {
            result = ash_lexer_scan_backslash(lexer, word);
        }
        else if (ch == '\'') {
            result = ash_lexer_scan_single_quote(lexer, word);
        }
        else if (ch == '"') {
            result = ash_lexer_scan_double_quote(
                lexer,
                word,
                ASH_QUOTE_DOUBLE
            );
        }
        else if (ch == '$' && ash_lexer_starts_with(lexer, "$'")) {
            result = ash_lexer_scan_dollar_single_quote(lexer, word);
        }
        else if (ch == '$' && ash_lexer_starts_with(lexer, "$\"")) {
            result = ash_lexer_scan_double_quote(
                lexer,
                word,
                ASH_QUOTE_LOCALE
            );
        }
        else if (ch == '$') {
            result = ash_lexer_scan_dollar(
                lexer,
                word,
                ASH_QUOTE_NONE
            );
        }
        else if (ch == '`') {
            result = ash_lexer_scan_matched(
                lexer,
                word,
                ASH_QUOTE_NONE,
                ASH_MATCH_BACKQUOTE
            );
        }
        else if (ash_lexer_starts_process_substitution(lexer)) {
            result = ash_lexer_scan_matched(
                lexer,
                word,
                ASH_QUOTE_NONE,
                ASH_MATCH_PROCESS
            );
        }
        else {
            struct ash_source_location location =
                ash_lexer_current_location(lexer);
            size_t start = lexer->offset;
            bool complete = false;
            while (!ash_lexer_at_end(lexer) &&
                   !ash_is_line_continuation_at(
                       lexer->input,
                       lexer->length,
                       lexer->offset
                   )) {
                ch = ash_lexer_peek(lexer, 0u);
                if (ch == '\\' || ch == '\'' || ch == '"' ||
                    ch == '$' || ch == '`' ||
                    ash_lexer_starts_process_substitution(lexer)) {
                    break;
                }
                if (ch == '(') {
                    depth++;
                }
                else if (ch == ')') {
                    assert(depth != 0u);
                    depth--;
                }
                (void)ash_lexer_advance(lexer);
                if (depth == 0u) {
                    complete = true;
                    break;
                }
            }
            if (ash_word_append_span(
                    word,
                    ASH_WORD_TEXT,
                    ASH_QUOTE_NONE,
                    location,
                    lexer->input + start,
                    lexer->offset - start
                ) != 0) {
                result = ash_lexer_fail(
                    lexer,
                    ASH_LEXER_ERROR,
                    location,
                    "out of memory"
                );
            }
            else if (complete) {
                return ASH_LEXER_TOKEN;
            }
        }
        if (result != ASH_LEXER_TOKEN) {
            return result;
        }
    }
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
            result = ash_lexer_scan_backslash(lexer, &token->word);
        }
        else if (ch == '\'') {
            result = ash_lexer_scan_single_quote(lexer, &token->word);
        }
        else if (ch == '"' ) {
            result = ash_lexer_scan_double_quote(
                lexer,
                &token->word,
                ASH_QUOTE_DOUBLE
            );
        }
        else if (ch == '$' && ash_lexer_starts_with(lexer, "$'")) {
            result = ash_lexer_scan_dollar_single_quote(lexer, &token->word);
        }
        else if (ch == '$' && ash_lexer_starts_with(lexer, "$\"")) {
            result = ash_lexer_scan_double_quote(
                lexer,
                &token->word,
                ASH_QUOTE_LOCALE
            );
        }
        else if (ch == '$') {
            result = ash_lexer_scan_dollar(lexer, &token->word, ASH_QUOTE_NONE);
        }
        else if (ch == '`') {
            result = ash_lexer_scan_matched(
                lexer,
                &token->word,
                ASH_QUOTE_NONE,
                ASH_MATCH_BACKQUOTE
            );
        }
        else if (ash_lexer_starts_process_substitution(lexer)) {
            result = ash_lexer_scan_matched(
                lexer,
                &token->word,
                ASH_QUOTE_NONE,
                ASH_MATCH_PROCESS
            );
        }
        else if (ash_lexer_starts_extglob(lexer)) {
            result = ash_lexer_scan_extglob(
                lexer,
                &token->word
            );
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
            if (ash_lexer_process_substitution_at(
                    lexer,
                    position
                )) {
                *match = (struct ash_io_redirect_match){0};
                return false;
            }
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
    if (ash_lexer_process_substitution_at(lexer, position)) {
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
    lexer->discarded_comment = false;
    lexer->ended_in_comment = false;

    while (true) {
        ash_lexer_skip_line_continuations(lexer);
        while (ash_is_blank(ash_lexer_peek_logical(lexer, 0u))) {
            ash_lexer_skip_line_continuations(lexer);
            (void)ash_lexer_advance(lexer);
        }
        ash_lexer_skip_line_continuations(lexer);
        if (ash_lexer_peek(lexer, 0u) == '#') {
            ash_lexer_skip_comment(lexer);
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

    const struct ash_operator* operator =
        ash_lexer_starts_process_substitution(lexer) ?
            NULL :
            ash_lexer_operator(lexer);
    if (operator != NULL) {
        token->kind = operator->kind;
        for (size_t i = 0u; i < operator->length; i++) {
            (void)ash_lexer_advance_logical(lexer);
        }
        return ASH_LEXER_TOKEN;
    }

    return ash_lexer_scan_word(lexer, token);
}

enum ash_lexer_fragment_result ash_lexer_classify_fragment_with_options(
    const char* input,
    size_t length,
    const struct ash_lexer_options* options
) {
    if (input == NULL || !ash_lexer_options_valid(options)) {
        errno = EINVAL;
        return ASH_LEXER_FRAGMENT_ERROR;
    }
    struct ash_lexer lexer;
    ash_lexer_init_at_with_options(
        &lexer,
        (struct ash_source_location){
            .source = "<fragment>",
            .line = 1u,
            .column = 1u,
        },
        input,
        length,
        options
    );
    bool terminal_operator_can_extend = false;
    while (true) {
        struct ash_token token;
        enum ash_lexer_result result = ash_lexer_next(
            &lexer,
            &token
        );
        if (result == ASH_LEXER_TOKEN) {
            terminal_operator_can_extend =
                lexer.offset == lexer.length &&
                ash_operator_kind_can_extend(token.kind);
        }
        ash_token_destroy(&token);
        if (result == ASH_LEXER_ERROR) {
            errno = ENOMEM;
            return ASH_LEXER_FRAGMENT_ERROR;
        }
        if (result == ASH_LEXER_INCOMPLETE ||
            (result == ASH_LEXER_END &&
             (ash_lexer_ended_in_comment(&lexer) ||
              ash_lexer_ended_with_line_continuation(&lexer) ||
              terminal_operator_can_extend ||
              (length != 0u && input[length - 1u] == '\\')))) {
            return ASH_LEXER_FRAGMENT_NEEDS_TAIL;
        }
        if (result == ASH_LEXER_END) {
            return ASH_LEXER_FRAGMENT_SELF_CONTAINED;
        }
    }
}

enum ash_lexer_fragment_result ash_lexer_classify_fragment(
    const char* input,
    size_t length
) {
    return ash_lexer_classify_fragment_with_options(
        input,
        length,
        &(const struct ash_lexer_options){0}
    );
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
