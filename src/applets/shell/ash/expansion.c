#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "applets/shell/ash/expansion.h"
#include "applets/shell/ash/shell_context.h"
#include "applets/shell/ash/syntax.h"
#include "applets/shell/ash/variables.h"
#include "lib/text_buffer.h"

static bool ash_expansion_oom(const struct ash_shell* shell) {
    fprintf(stderr, "%s: out of memory\n", shell->progname);
    return false;
}

static bool ash_expansion_bad_substitution(const struct ash_shell* shell) {
    fprintf(stderr, "%s: bad substitution\n", shell->progname);
    return false;
}

static bool ash_expansion_append_span(
    const struct ash_shell* shell,
    struct bx_text_buffer* output,
    const char* text,
    size_t length
) {
    return bx_text_buffer_append_span(output, text, length) ||
        ash_expansion_oom(shell);
}

static bool ash_expansion_append_text(
    const struct ash_shell* shell,
    struct bx_text_buffer* output,
    const char* text
) {
    return bx_text_buffer_append_text(output, text) ||
        ash_expansion_oom(shell);
}

static bool ash_expansion_append_char(
    const struct ash_shell* shell,
    struct bx_text_buffer* output,
    char character
) {
    return bx_text_buffer_append_char(output, character) ||
        ash_expansion_oom(shell);
}

static const char* ash_positional(
    const struct ash_shell* shell,
    long index
) {
    if (index == 0) {
        return shell->positionals.argv0;
    }
    if (index < 0 || index > shell->positionals.count) {
        return "";
    }
    return shell->positionals.values[index - 1];
}

static const char* ash_ifs_joiner(const struct ash_shell* shell) {
    const char* ifs = ash_var_get(shell, "IFS");
    if (ifs == NULL) {
        return " ";
    }
    return ifs;
}

static bool ash_append_positionals_joined(
    struct ash_shell* shell,
    struct bx_text_buffer* output
) {
    const char* ifs = ash_ifs_joiner(shell);
    size_t separator_length = ifs[0] == '\0' ? 0u : 1u;
    for (int i = 0; i < shell->positionals.count; i++) {
        if (i != 0 &&
            !ash_expansion_append_span(
                shell,
                output,
                ifs,
                separator_length
            )) {
            return false;
        }
        if (!ash_expansion_append_text(
                shell,
                output,
                shell->positionals.values[i]
            )) {
            return false;
        }
    }
    return true;
}

static bool ash_append_special(
    struct ash_shell* shell,
    char parameter,
    struct bx_text_buffer* output
) {
    char number[32];
    switch (parameter) {
        case '?':
            snprintf(number, sizeof(number), "%d", shell->last_status);
            return ash_expansion_append_text(shell, output, number);
        case '$':
            snprintf(number, sizeof(number), "%ld", (long)shell->shell_pid);
            return ash_expansion_append_text(shell, output, number);
        case '#':
            snprintf(number, sizeof(number), "%d", shell->positionals.count);
            return ash_expansion_append_text(shell, output, number);
        case '-': {
            char letters[16];
            ash_shell_option_letters(shell, letters, sizeof(letters));
            return ash_expansion_append_text(shell, output, letters);
        }
        case '!':
            if (shell->last_async_pid <= 0) {
                return true;
            }
            snprintf(
                number,
                sizeof(number),
                "%ld",
                (long)shell->last_async_pid
            );
            return ash_expansion_append_text(shell, output, number);
        case '0':
            return ash_expansion_append_text(
                shell,
                output,
                ash_positional(shell, 0)
            );
        case '@':
        case '*':
            return ash_append_positionals_joined(shell, output);
        default:
            return false;
    }
}

static bool ash_expand_parameter(
    struct ash_shell* shell,
    const char* input,
    struct bx_text_buffer* output
) {
    size_t position = 1u;
    char character = input[position];
    if (character == '\0') {
        return ash_expansion_append_char(shell, output, '$');
    }

    if (strchr("?$#-!@*", character) != NULL) {
        return input[position + 1u] == '\0' &&
            ash_append_special(shell, character, output);
    }

    if (character == '{') {
        position++;
        if (strchr("?$#-!0@*", input[position]) != NULL) {
            char special = input[position++];
            if (input[position] != '}' || input[position + 1u] != '\0') {
                return ash_expansion_bad_substitution(shell);
            }
            return ash_append_special(shell, special, output);
        }

        size_t start = position;
        while (ash_is_name_char((unsigned char)input[position])) {
            position++;
        }
        if (position == start || input[position] != '}' ||
            input[position + 1u] != '\0') {
            return ash_expansion_bad_substitution(shell);
        }
        const char* value = ash_var_get_len(
            shell,
            input + start,
            position - start
        );
        return ash_expansion_append_text(
            shell,
            output,
            value != NULL ? value : ""
        );
    }

    if (isdigit((unsigned char)character)) {
        long index = 0;
        while (isdigit((unsigned char)input[position])) {
            if (index <= (LONG_MAX - 9) / 10) {
                index = index * 10 + (long)(input[position] - '0');
            }
            position++;
        }
        if (input[position] != '\0') {
            return ash_expansion_bad_substitution(shell);
        }
        return ash_expansion_append_text(
            shell,
            output,
            ash_positional(shell, index)
        );
    }

    if (!ash_is_name_start((unsigned char)character)) {
        return ash_expansion_append_text(shell, output, input);
    }
    size_t start = position;
    while (ash_is_name_char((unsigned char)input[position])) {
        position++;
    }
    if (input[position] != '\0') {
        return ash_expansion_bad_substitution(shell);
    }
    const char* value = ash_var_get_len(
        shell,
        input + start,
        position - start
    );
    return ash_expansion_append_text(
        shell,
        output,
        value != NULL ? value : ""
    );
}

static int ash_hex_value(unsigned char character) {
    if (character >= '0' && character <= '9') {
        return (int)(character - '0');
    }
    if (character >= 'a' && character <= 'f') {
        return (int)(character - 'a') + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return (int)(character - 'A') + 10;
    }
    return -1;
}

static bool ash_append_dollar_single(
    struct ash_shell* shell,
    struct bx_text_buffer* output,
    const char* text,
    size_t length
) {
    for (size_t i = 0u; i < length; i++) {
        unsigned char character = (unsigned char)text[i];
        if (character != '\\' || i + 1u == length) {
            if (!ash_expansion_append_char(shell, output, (char)character)) {
                return false;
            }
            continue;
        }

        unsigned char escaped = (unsigned char)text[++i];
        char decoded;
        switch (escaped) {
            case 'a': decoded = '\a'; break;
            case 'b': decoded = '\b'; break;
            case 'e':
            case 'E': decoded = 0x1b; break;
            case 'f': decoded = '\f'; break;
            case 'n': decoded = '\n'; break;
            case 'r': decoded = '\r'; break;
            case 't': decoded = '\t'; break;
            case 'v': decoded = '\v'; break;
            case '\\': decoded = '\\'; break;
            case '\'': decoded = '\''; break;
            case '"': decoded = '"'; break;
            case '\n': continue;
            case 'c':
                decoded = i + 1u < length ?
                    (char)((unsigned char)text[++i] & 0x1fu) : 'c';
                break;
            case 'x': {
                int value = 0;
                size_t digits = 0u;
                while (digits < 2u && i + 1u < length) {
                    int digit = ash_hex_value((unsigned char)text[i + 1u]);
                    if (digit < 0) {
                        break;
                    }
                    value = value * 16 + digit;
                    i++;
                    digits++;
                }
                if (digits == 0u) {
                    if (!ash_expansion_append_char(shell, output, '\\')) {
                        return false;
                    }
                    decoded = 'x';
                }
                else {
                    decoded = (char)(unsigned char)value;
                }
                break;
            }
            default:
                if (escaped >= '0' && escaped <= '7') {
                    unsigned int value = (unsigned int)(escaped - '0');
                    size_t digits = 1u;
                    while (digits < 3u && i + 1u < length &&
                           text[i + 1u] >= '0' && text[i + 1u] <= '7') {
                        value = value * 8u +
                            (unsigned int)(text[++i] - '0');
                        digits++;
                    }
                    decoded = (char)(unsigned char)value;
                }
                else {
                    if (!ash_expansion_append_char(shell, output, '\\')) {
                        return false;
                    }
                    decoded = (char)escaped;
                }
                break;
        }
        if (!ash_expansion_append_char(shell, output, decoded)) {
            return false;
        }
    }
    return true;
}

static bool ash_expand_part(
    struct ash_shell* shell,
    const struct ash_word_part* part,
    struct bx_text_buffer* output
) {
    if (part->kind == ASH_WORD_PARAMETER) {
        return ash_expand_parameter(shell, part->text, output);
    }
    if (part->kind == ASH_WORD_COMMAND_SUBSTITUTION ||
        part->kind == ASH_WORD_BACKQUOTE) {
        size_t prefix = part->kind == ASH_WORD_COMMAND_SUBSTITUTION ?
            2u : 1u;
        size_t suffix = 1u;
        if (part->length < prefix + suffix ||
            shell->command_substitution == NULL) {
            fprintf(
                stderr,
                "%s: command substitution is unavailable\n",
                shell->progname
            );
            return false;
        }

        char* substitution = NULL;
        bool expanded = shell->command_substitution(
            shell,
            part->text + prefix,
            part->length - prefix - suffix,
            &substitution
        );
        if (expanded) {
            expanded = ash_expansion_append_text(
                shell,
                output,
                substitution
            );
        }
        free(substitution);
        return expanded;
    }
    if (part->kind == ASH_WORD_TEXT &&
        part->quote == ASH_QUOTE_DOLLAR_SINGLE) {
        return ash_append_dollar_single(
            shell,
            output,
            part->text,
            part->length
        );
    }
    return ash_expansion_append_span(
        shell,
        output,
        part->text,
        part->length
    );
}

static bool ash_append_pattern_component(
    struct ash_shell* shell,
    struct bx_text_buffer* output,
    const struct bx_text_buffer* component,
    bool quoted
) {
    for (size_t i = 0u; i < component->length; i++) {
        char character = component->data[i];
        if (quoted &&
            (character == '\\' || character == '*' ||
             character == '?' || character == '[' ||
             character == ']' || character == '!' ||
             character == '^' || character == '-')) {
            if (!ash_expansion_append_char(shell, output, '\\')) {
                return false;
            }
        }
        if (!ash_expansion_append_char(shell, output, character)) {
            return false;
        }
    }
    return true;
}

bool ash_expand(
    struct ash_shell* shell,
    const struct ash_word* word,
    enum ash_expansion_context context,
    char** output_word
) {
    struct bx_text_buffer output;
    bx_text_buffer_init(&output);

    for (size_t i = 0u; i < word->count; i++) {
        const struct ash_word_part* part = &word->parts[i];
        struct bx_text_buffer component;
        bx_text_buffer_init(&component);
        if (!ash_expand_part(shell, part, &component)) {
            bx_text_buffer_destroy(&component);
            bx_text_buffer_destroy(&output);
            return false;
        }

        bool appended = context == ASH_EXPANSION_PATTERN ?
            ash_append_pattern_component(
                shell,
                &output,
                &component,
                part->quote != ASH_QUOTE_NONE
            ) :
            ash_expansion_append_span(
                shell,
                &output,
                component.data,
                component.length
            );
        bx_text_buffer_destroy(&component);
        if (!appended) {
            bx_text_buffer_destroy(&output);
            return false;
        }
    }

    *output_word = bx_text_buffer_take(&output);
    if (*output_word == NULL) {
        bx_text_buffer_destroy(&output);
        return ash_expansion_oom(shell);
    }
    return true;
}

bool ash_expand_word(
    struct ash_shell* shell,
    const struct ash_word* word,
    char** output_word
) {
    return ash_expand(shell, word, ASH_EXPANSION_WORD, output_word);
}

void ash_expanded_fields_init(struct ash_expanded_fields* fields) {
    *fields = (struct ash_expanded_fields){0};
}

void ash_expanded_fields_destroy(struct ash_expanded_fields* fields) {
    for (size_t i = 0u; i < fields->count; i++) {
        free(fields->values[i]);
    }
    free(fields->values);
    *fields = (struct ash_expanded_fields){0};
}

static bool ash_expanded_fields_push(
    struct ash_shell* shell,
    struct ash_expanded_fields* fields,
    const char* value
) {
    if (fields->count == fields->capacity) {
        size_t capacity = fields->capacity == 0u ?
            4u : fields->capacity * 2u;
        if (capacity < fields->capacity ||
            capacity > SIZE_MAX / sizeof(*fields->values)) {
            return ash_expansion_oom(shell);
        }
        char** values = realloc(
            fields->values,
            capacity * sizeof(*values)
        );
        if (values == NULL) {
            return ash_expansion_oom(shell);
        }
        fields->values = values;
        fields->capacity = capacity;
    }
    fields->values[fields->count] = strdup(value);
    if (fields->values[fields->count] == NULL) {
        return ash_expansion_oom(shell);
    }
    fields->count++;
    return true;
}

static bool ash_expanded_fields_append(
    struct ash_shell* shell,
    struct ash_expanded_fields* fields,
    const char* value
) {
    if (fields->count == 0u &&
        !ash_expanded_fields_push(shell, fields, "")) {
        return false;
    }
    size_t index = fields->count - 1u;
    size_t old_length = strlen(fields->values[index]);
    size_t added = strlen(value);
    if (added > SIZE_MAX - old_length - 1u) {
        return ash_expansion_oom(shell);
    }
    char* replacement = realloc(
        fields->values[index],
        old_length + added + 1u
    );
    if (replacement == NULL) {
        return ash_expansion_oom(shell);
    }
    memcpy(replacement + old_length, value, added + 1u);
    fields->values[index] = replacement;
    return true;
}

static bool ash_parameter_is(
    const struct ash_word_part* part,
    char parameter
) {
    return part->kind == ASH_WORD_PARAMETER &&
        ((part->length == 2u && part->text[0] == '$' &&
          part->text[1] == parameter) ||
         (part->length == 4u && part->text[0] == '$' &&
          part->text[1] == '{' && part->text[2] == parameter &&
          part->text[3] == '}'));
}

bool ash_expand_argument(
    struct ash_shell* shell,
    const struct ash_word* word,
    struct ash_expanded_fields* fields
) {
    ash_expanded_fields_init(fields);
    bool field_present = false;

    for (size_t i = 0u; i < word->count; i++) {
        const struct ash_word_part* part = &word->parts[i];
        if (ash_parameter_is(part, '@')) {
            if (shell->positionals.count == 0) {
                continue;
            }
            for (int j = 0; j < shell->positionals.count; j++) {
                bool added = j == 0 ?
                    ash_expanded_fields_append(
                        shell,
                        fields,
                        shell->positionals.values[j]
                    ) :
                    ash_expanded_fields_push(
                        shell,
                        fields,
                        shell->positionals.values[j]
                    );
                if (!added) {
                    ash_expanded_fields_destroy(fields);
                    return false;
                }
            }
            field_present = true;
            continue;
        }

        struct bx_text_buffer component;
        bx_text_buffer_init(&component);
        if (!ash_expand_part(shell, part, &component) ||
            !ash_expanded_fields_append(
                shell,
                fields,
                component.data != NULL ? component.data : ""
            )) {
            bx_text_buffer_destroy(&component);
            ash_expanded_fields_destroy(fields);
            return false;
        }
        if (component.length != 0u || part->quote != ASH_QUOTE_NONE) {
            field_present = true;
        }
        bx_text_buffer_destroy(&component);
    }

    if (!field_present) {
        ash_expanded_fields_destroy(fields);
    }
    return true;
}
