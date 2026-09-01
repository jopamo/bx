#ifndef BX_APPLETS_SHELL_ASH_COMMAND_H
#define BX_APPLETS_SHELL_ASH_COMMAND_H

#include <stddef.h>

enum ash_redir_kind {
    ASH_REDIR_IN = 0,
    ASH_REDIR_OUT,
    ASH_REDIR_CLOBBER,
    ASH_REDIR_APPEND,
    ASH_REDIR_READWRITE,
    ASH_REDIR_DUP,
};

struct ash_redir {
    int fd;
    enum ash_redir_kind kind;
    /* Owned, NUL-terminated target. */
    char* target;
};

struct ash_command {
    /* Owns every string and redirection target in these backing arrays. */
    char** words;
    size_t word_count;
    size_t word_cap;
    char** assignments;
    size_t assignment_count;
    size_t assignment_cap;
    struct ash_redir* redirs;
    size_t redir_count;
    size_t redir_cap;
};

#endif /* BX_APPLETS_SHELL_ASH_COMMAND_H */
