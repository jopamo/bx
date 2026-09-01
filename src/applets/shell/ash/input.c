#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "applets/shell/ash/input.h"
#include "applets/shell/ash/shell_context.h"

struct ash_source_name {
    char* value;
    struct ash_source_name* next;
    bool published;
};

static char* ash_input_duplicate(const char* text) {
    size_t length = strlen(text);
    if (length == SIZE_MAX) {
        errno = ENOMEM;
        return NULL;
    }

    char* copy = malloc(length + 1u);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, text, length + 1u);
    return copy;
}

static struct ash_input_source* ash_input_create(
    struct ash_shell* shell,
    enum ash_input_kind kind,
    const char* name
) {
    const char* effective_name = (name != NULL) ? name : "<input>";
    struct ash_input_source* input = calloc(1u, sizeof(*input));
    if (input == NULL) {
        return NULL;
    }

    struct ash_source_name* identity = shell->source_names;
    while (identity != NULL &&
           strcmp(identity->value, effective_name) != 0) {
        identity = identity->next;
    }
    if (identity == NULL) {
        identity = calloc(1u, sizeof(*identity));
        if (identity == NULL) {
            free(input);
            return NULL;
        }
        identity->value = ash_input_duplicate(effective_name);
        if (identity->value == NULL) {
            free(identity);
            free(input);
            return NULL;
        }
    }

    input->kind = kind;
    input->name = identity->value;
    input->identity = identity;
    return input;
}

static void ash_input_discard_candidate(struct ash_input_source* input) {
    if (input == NULL) {
        return;
    }
    if (!input->identity->published) {
        free(input->identity->value);
        free(input->identity);
    }
    free(input);
}

static void ash_input_publish(
    struct ash_shell* shell,
    struct ash_input_source* input
) {
    if (!input->identity->published) {
        input->identity->next = shell->source_names;
        input->identity->published = true;
        shell->source_names = input->identity;
    }
    input->previous = shell->input_stack;
    shell->input_stack = input;
}

bool ash_input_push_string(struct ash_shell* shell, const char* name, const char* text) {
    if (shell == NULL || text == NULL) {
        errno = EINVAL;
        return false;
    }

    struct ash_input_source* input = ash_input_create(
        shell,
        ASH_INPUT_STRING,
        name
    );
    if (input == NULL) {
        return false;
    }

    input->source.string.text = ash_input_duplicate(text);
    if (input->source.string.text == NULL) {
        ash_input_discard_candidate(input);
        return false;
    }
    input->source.string.length = strlen(text);
    ash_input_publish(shell, input);
    return true;
}

bool ash_input_push_file(
    struct ash_shell* shell,
    const char* name,
    FILE* stream,
    enum ash_input_stream_ownership ownership
) {
    if (shell == NULL || stream == NULL ||
        (ownership != ASH_INPUT_BORROW_STREAM &&
            ownership != ASH_INPUT_TAKE_STREAM)) {
        errno = EINVAL;
        return false;
    }

    struct ash_input_source* input = ash_input_create(
        shell,
        ASH_INPUT_FILE,
        name
    );
    if (input == NULL) {
        return false;
    }

    input->source.file.stream = stream;
    input->source.file.ownership = ownership;
    ash_input_publish(shell, input);
    return true;
}

void ash_input_pop(struct ash_shell* shell) {
    if (shell == NULL || shell->input_stack == NULL) {
        return;
    }

    struct ash_input_source* input = shell->input_stack;
    shell->input_stack = input->previous;
    if (input->kind == ASH_INPUT_STRING) {
        free(input->source.string.text);
    }
    else if (input->source.file.ownership == ASH_INPUT_TAKE_STREAM) {
        fclose(input->source.file.stream);
    }
    free(input);
}

void ash_input_release_all(struct ash_shell* shell) {
    while (shell != NULL && shell->input_stack != NULL) {
        ash_input_pop(shell);
    }
}

void ash_input_source_names_destroy(struct ash_shell* shell) {
    while (shell != NULL && shell->source_names != NULL) {
        struct ash_source_name* identity = shell->source_names;
        shell->source_names = identity->next;
        free(identity->value);
        free(identity);
    }
}

static bool ash_input_reserve(char** line, size_t* capacity, size_t needed) {
    if (*capacity >= needed) {
        return true;
    }

    size_t grown = (*capacity == 0u) ? 128u : *capacity;
    while (grown < needed) {
        if (grown > SIZE_MAX / 2u) {
            grown = needed;
            break;
        }
        grown *= 2u;
    }

    char* replacement = realloc(*line, grown);
    if (replacement == NULL) {
        return false;
    }
    *line = replacement;
    *capacity = grown;
    return true;
}

static ssize_t ash_input_read_string(
    struct ash_input_source* input,
    char** line,
    size_t* capacity
) {
    size_t offset = input->source.string.offset;
    size_t length = input->source.string.length;
    if (offset == length) {
        return -1;
    }

    const char* text = input->source.string.text;
    size_t end = offset;
    while (end < length && text[end] != '\n') {
        end++;
    }
    if (end < length) {
        end++;
    }

    size_t line_length = end - offset;
    if (line_length > (size_t)PTRDIFF_MAX || line_length == SIZE_MAX ||
        !ash_input_reserve(line, capacity, line_length + 1u)) {
        errno = ENOMEM;
        return -1;
    }

    memcpy(*line, text + offset, line_length);
    (*line)[line_length] = '\0';
    input->source.string.offset = end;
    return (ssize_t)line_length;
}

ssize_t ash_input_read_line(struct ash_shell* shell, char** line, size_t* capacity) {
    if (shell == NULL || shell->input_stack == NULL || line == NULL || capacity == NULL) {
        errno = EINVAL;
        return -1;
    }

    struct ash_input_source* input = shell->input_stack;
    ssize_t result;
    if (input->kind == ASH_INPUT_STRING) {
        result = ash_input_read_string(input, line, capacity);
    }
    else {
        result = getline(line, capacity, input->source.file.stream);
    }

    if (result >= 0) {
        input->line++;
    }
    return result;
}

const char* ash_input_source_name(const struct ash_shell* shell) {
    if (shell == NULL || shell->input_stack == NULL) {
        return NULL;
    }
    return shell->input_stack->name;
}

size_t ash_input_source_line(const struct ash_shell* shell) {
    if (shell == NULL || shell->input_stack == NULL) {
        return 0u;
    }
    return shell->input_stack->line;
}

bool ash_input_source_is_terminal(const struct ash_shell* shell) {
    if (shell == NULL || shell->input_stack == NULL ||
        shell->input_stack->kind != ASH_INPUT_FILE) {
        return false;
    }

    int fd = fileno(shell->input_stack->source.file.stream);
    return fd >= 0 && isatty(fd);
}
