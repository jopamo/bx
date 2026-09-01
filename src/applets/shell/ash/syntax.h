#ifndef BX_APPLETS_SHELL_ASH_SYNTAX_H
#define BX_APPLETS_SHELL_ASH_SYNTAX_H

#include <stddef.h>

struct ash_source_location {
    /* Borrowed context-lifetime source identity; never freed by a location. */
    const char* source;
    size_t line;
    size_t column;
    size_t offset;
};

enum ash_quote_kind {
    ASH_QUOTE_NONE = 0,
    ASH_QUOTE_BACKSLASH,
    ASH_QUOTE_SINGLE,
    ASH_QUOTE_DOUBLE,
    ASH_QUOTE_DOLLAR_SINGLE,
};

enum ash_word_part_kind {
    ASH_WORD_TEXT = 0,
    ASH_WORD_PARAMETER,
    ASH_WORD_COMMAND_SUBSTITUTION,
    ASH_WORD_ARITHMETIC,
    ASH_WORD_BACKQUOTE,
    /*
     * The AST layer attaches a parsed command and direction to this
     * placeholder; syntax retains its exact source span.
     */
    ASH_WORD_PROCESS_SUBSTITUTION,
};

struct ash_word_part {
    enum ash_word_part_kind kind;
    enum ash_quote_kind quote;
    struct ash_source_location location;
    /* Owned, NUL-terminated storage; length excludes the terminator. */
    char* text;
    size_t length;
    size_t capacity;
};

struct ash_word {
    /* Owns the part array and every part's text. */
    struct ash_word_part* parts;
    size_t count;
    size_t capacity;
    struct ash_source_location location;
};

void ash_word_init(struct ash_word* word, struct ash_source_location location);
void ash_word_destroy(struct ash_word* word);
/* Deep clone: destination owns an independent word on success. */
int ash_word_clone(struct ash_word* destination, const struct ash_word* source);
bool ash_word_part_is_quoted(const struct ash_word_part* part);
bool ash_word_part_is_expansion(const struct ash_word_part* part);
int ash_word_append(
    struct ash_word* word,
    enum ash_word_part_kind kind,
    enum ash_quote_kind quote,
    struct ash_source_location location,
    const char* text,
    size_t length
);

#endif /* BX_APPLETS_SHELL_ASH_SYNTAX_H */
