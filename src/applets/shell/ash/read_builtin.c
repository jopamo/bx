#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "applets/shell/ash/command.h"
#include "applets/shell/ash/diagnostic.h"
#include "applets/shell/ash/field_split.h"
#include "applets/shell/ash/read_builtin.h"
#include "applets/shell/ash/traps.h"
#include "applets/shell/ash/variables.h"
#include "lib/text_buffer.h"

struct ash_read_line {
    struct bx_text_buffer text;
    struct bx_text_buffer quoted;
    const char* ifs;
    size_t ifs_length;
};

static bool ash_read_space(const struct ash_read_line* line, size_t position) {
    return position < line->text.length && !line->quoted.data[position] && ash_ifs_whitespace(line->ifs, (unsigned char)line->text.data[position]);
}

static size_t ash_read_delimiter(const struct ash_read_line* line, size_t position) {
    if (position == line->text.length || line->quoted.data[position])
        return 0u;
    size_t width = ash_ifs_delimiter_length(line->ifs, line->ifs_length, line->text.data + position, line->text.length - position);
    for (size_t i = 1u; i < width; i++) {
        if (line->quoted.data[position + i])
            return 0u;
    }
    return width;
}

/* Retain offsets: the final variable may need the original intervening delimiters. */
static bool ash_read_field(const struct ash_read_line* line, size_t* cursor, size_t* begin, size_t* end) {
    size_t position = *cursor;
    while (ash_read_space(line, position))
        position++;
    *begin = *end = position;
    if (position == line->text.length) {
        *cursor = position;
        return false;
    }
    mbstate_t state = {0};
    while (position < line->text.length && ash_read_delimiter(line, position) == 0u)
        position += ash_character_width(line->text.data + position, line->text.length - position, &state);
    *end = position;
    while (ash_read_space(line, position))
        position++;
    position += ash_read_delimiter(line, position);
    while (ash_read_space(line, position))
        position++;
    *cursor = position;
    return true;
}

static int ash_read_input(struct ash_shell* shell, struct ash_read_line* line, bool raw) {
    bool escaped = false;
    for (;;) {
        /* Do not read ahead into the next command when stdin also holds the script. */
        char byte;
        if (ash_trap_signal_wait_readable(shell, STDIN_FILENO) < 0)
            return -1;
        ssize_t count = read(STDIN_FILENO, &byte, 1u);
        if (count < 0)
            return -1;
        if (count == 0)
            return 1;
        if (byte == '\0')
            continue;
        if (!raw && !escaped && byte == '\\') {
            escaped = true;
            continue;
        }
        if (byte == '\n') {
            if (!escaped)
                return 0;
            escaped = false;
            continue;
        }
        if (!bx_text_buffer_append_char(&line->text, byte) || !bx_text_buffer_append_char(&line->quoted, escaped ? 1 : 0))
            return -1;
        escaped = false;
    }
}

int ash_read_builtin(struct ash_shell* shell, const struct ash_command* command) {
    bool raw = false;
    size_t index = 1u;
    for (; index < command->word_count; index++) {
        const char* option = command->words[index];
        if (strcmp(option, "--") == 0) {
            index++;
            break;
        }
        if (option[0] != '-' || option[1] == '\0')
            break;
        for (size_t i = 1u; option[i]; i++) {
            if (option[i] != 'r') {
                ash_diag(shell, "read: usage: read [-r] [--] [NAME...]");
                return 2;
            }
            raw = true;
        }
    }
    for (size_t i = index; i < command->word_count; i++) {
        if (!ash_is_valid_name_span(command->words[i], strlen(command->words[i]))) {
            ash_diag(shell, "read: invalid name '%s'", command->words[i]);
            return 2;
        }
    }
    const char* current_ifs = ash_var_get(shell, "IFS");
    struct ash_read_line line = {.ifs = current_ifs != NULL ? current_ifs : " \t\n"};
    size_t count = command->word_count - index;
    char** values = calloc(count != 0u ? count : 1u, sizeof(*values));
    int status = 1;
    if (values == NULL || !bx_text_buffer_append_text(&line.text, "")) {
        ash_diag_oom(shell);
        goto done;
    }
    line.ifs_length = strlen(line.ifs);
    status = ash_read_input(shell, &line, raw);
    if (status < 0) {
        if (errno != EINTR)
            ash_exec_error(shell, "read", errno);
        status = 1;
        goto done;
    }
    values[0] = line.text.data;
    /* Split before assigning: destinations may change IFS or the active locale. */
    size_t cursor = 0u;
    for (size_t i = 0u; i < count; i++) {
        size_t begin, end;
        (void)ash_read_field(&line, &cursor, &begin, &end);
        if (i + 1u == count) {
            size_t next_begin, next_end;
            if (ash_read_field(&line, &cursor, &next_begin, &next_end)) {
                end = line.text.length;
                while (end > begin && ash_read_space(&line, end - 1u))
                    end--;
            }
        }
        values[i] = line.text.data + begin;
        line.text.data[end] = '\0';
    }
    for (size_t i = 0u; i < (count != 0u ? count : 1u); i++) {
        const char* name = count != 0u ? command->words[index + i] : "REPLY";
        if (!ash_var_builtin_assignment_allowed(shell, name, strlen(name)) || !ash_var_set_caller(shell, name, values[i], false))
            status = 1;
    }
done:
    bx_text_buffer_destroy(&line.text);
    bx_text_buffer_destroy(&line.quoted);
    free(values);
    return status;
}
