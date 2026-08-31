#ifndef BX_APPLETS_SHELL_ASH_INPUT_H
#define BX_APPLETS_SHELL_ASH_INPUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>

struct ash_shell;

enum ash_input_kind {
    ASH_INPUT_STRING = 0,
    ASH_INPUT_FILE,
};

struct ash_input_source {
    enum ash_input_kind kind;
    char* name;
    size_t line;
    struct ash_input_source* previous;

    union {
        struct {
            char* text;
            size_t length;
            size_t offset;
        } string;
        struct {
            FILE* stream;
            bool close_on_pop;
        } file;
    } source;
};

bool ash_input_push_string(struct ash_shell* shell, const char* name, const char* text);
bool ash_input_push_file(
    struct ash_shell* shell,
    const char* name,
    FILE* stream,
    bool close_on_pop
);
void ash_input_pop(struct ash_shell* shell);
void ash_input_release_all(struct ash_shell* shell);

ssize_t ash_input_read_line(struct ash_shell* shell, char** line, size_t* capacity);
const char* ash_input_source_name(const struct ash_shell* shell);
size_t ash_input_source_line(const struct ash_shell* shell);
bool ash_input_source_is_terminal(const struct ash_shell* shell);

#endif /* BX_APPLETS_SHELL_ASH_INPUT_H */
