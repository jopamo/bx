#ifndef BX_APPLETS_SHELL_ASH_COMMAND_H
#define BX_APPLETS_SHELL_ASH_COMMAND_H

#include <stdbool.h>
#include <stddef.h>

#include "applets/shell/ash/syntax.h"

enum ash_redir_kind {
    ASH_REDIR_IN = 0,
    ASH_REDIR_OUT,
    ASH_REDIR_CLOBBER,
    ASH_REDIR_APPEND,
    ASH_REDIR_READWRITE,
    ASH_REDIR_DUP,
    ASH_REDIR_HERE_DOCUMENT,
};

struct ash_redir {
    int fd;
    enum ash_redir_kind kind;
    /* Owned target, or exact here-document body bytes with a trailing NUL. */
    char* target;
    size_t target_length;
    bool expand_here_document;
    bool strip_here_document_tabs;
    /* Borrowed from the AST, which remains alive through descriptor setup. */
    struct ash_source_location body_location;
    /* Deferred operands borrow syntax from the same live AST. */
    const struct ash_word* target_word;
};

struct ash_command {
    /* Owns word strings, redirection targets and backing arrays. */
    char** words;
    size_t word_count;
    size_t word_cap;
    /* Assignment syntax borrows the AST until command execution finishes. */
    const struct ash_word** assignments;
    size_t assignment_count;
    size_t assignment_cap;
    struct ash_redir* redirs;
    size_t redir_count;
    size_t redir_cap;
    /* Substitution status carried into the next expansion phase. */
    int substitution_status;
};

#endif /* BX_APPLETS_SHELL_ASH_COMMAND_H */
