#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "applets/shell/ash/arithmetic.h"
#include "applets/shell/ash/control.h"
#include "applets/shell/ash/diagnostic.h"
#include "applets/shell/ash/expansion.h"
#include "applets/shell/ash/lexer.h"
#include "applets/shell/ash/locale_state.h"
#include "applets/shell/ash/pathname_expansion.h"
#include "applets/shell/ash/pattern.h"
#include "applets/shell/ash/quote.h"
#include "applets/shell/ash/shell_context.h"
#include "applets/shell/ash/syntax.h"
#include "applets/shell/ash/variables.h"
#include "lib/path_ops.h"
#include "lib/text_buffer.h"

static bool ash_expansion_oom(const struct ash_shell* shell) {
    return ash_diag_oom(shell);
}

static bool ash_expansion_errexit(const struct ash_shell* shell) {
    return (shell->options & ASH_SHELL_OPTION_ERREXIT) != 0u && !shell->errexit_diagnostics_suppressed;
}

static bool ash_expansion_fail(struct ash_shell* shell, bool terminate_context) {
    bool errexit = ash_expansion_errexit(shell);
    if (shell->forked_execution || terminate_context || errexit) {
        shell->should_exit = true;
        shell->requested_exit_status = shell->forked_execution || errexit ? 1 : 127;
    }
    else {
        ash_control_discard_unit(shell, 1);
    }
    return false;
}

static bool ash_expansion_bad_substitution(struct ash_shell* shell, const char* input) {
    ash_diag_expansion(shell, "%s: bad substitution", input);
    return ash_expansion_fail(shell, ash_shell_policy_noninteractive_posix(&shell->policy));
}

static bool ash_pathname_expansion_enabled(
    const struct ash_shell* shell
) {
    return (shell->options & ASH_SHELL_OPTION_NOGLOB) == 0u;
}

static bool ash_word_may_expand_pathname(
    const struct ash_word* word
) {
    for (size_t i = 0u; i < word->count; i++) {
        const struct ash_word_part* part = &word->parts[i];
        if (ash_word_part_is_quoted(part)) {
            continue;
        }
        if (ash_word_part_is_expansion(part) ||
            (part->text != NULL &&
             strpbrk(part->text, "*?[") != NULL)) {
            return true;
        }
    }
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

static size_t ash_character_width(const char* value, size_t remaining, mbstate_t* state) {
    if (MB_CUR_MAX == 1u) {
        return 1u;
    }
    size_t width = mbrlen(value, remaining, state);
    if (width == 0u || width == (size_t)-1 || width == (size_t)-2) {
        /* Undefined malformed input consumes one byte and resets conversion. */
        *state = (mbstate_t){0};
        return 1u;
    }
    return width;
}

static size_t ash_parameter_character_count(const char* value) {
    size_t remaining = strlen(value);
    if (MB_CUR_MAX == 1u) {
        return remaining;
    }
    size_t count = 0u;
    mbstate_t state = {0};
    while (remaining != 0u) {
        size_t width = ash_character_width(value, remaining, &state);
        value += width;
        remaining -= width;
        count++;
    }
    return count;
}

static bool ash_append_parameter_value(struct ash_shell* shell, struct bx_text_buffer* output, const char* value, const char* name, size_t name_length, bool length) {
    if (value == NULL && (shell->options & ASH_SHELL_OPTION_NOUNSET) != 0u) {
        ash_diag_unbound_parameter(shell, name, name_length);
        return ash_expansion_fail(shell, !ash_shell_policy_has(&shell->policy, ASH_SHELL_POLICY_INTERACTIVE));
    }
    value = value != NULL ? value : "";
    if (length) {
        char number[sizeof(size_t) * CHAR_BIT + 1u];
        snprintf(number, sizeof(number), "%zu", ash_parameter_character_count(value));
        return ash_expansion_append_text(shell, output, number);
    }
    return ash_expansion_append_text(shell, output, value);
}

static const char* ash_positional(
    const struct ash_shell* shell,
    uintmax_t index
) {
    const struct ash_positional_frame* positionals =
        ash_scope_positionals(shell);
    if (positionals == NULL) {
        return NULL;
    }
    if (index == 0) {
        return positionals->argv0;
    }
    if (index > positionals->count) {
        return NULL;
    }
    return positionals->values[index - 1];
}

static bool ash_parameter_index(const char* digits, size_t length, uintmax_t* index) {
    *index = 0u;
    for (size_t i = 0u; i < length; i++) {
        unsigned int digit = (unsigned int)(digits[i] - '0');
        if (*index > ((uintmax_t)INTMAX_MAX - digit) / 10u) {
            return false;
        }
        *index = *index * 10u + digit;
    }
    return true;
}

static bool ash_append_numbered_parameter(struct ash_shell* shell, struct bx_text_buffer* output, const char* digits, size_t length, bool braced, bool measure) {
    const char* name = braced ? digits : digits - 1;
    size_t name_length = length + (braced ? 0u : 1u);
    uintmax_t index;
    if (!ash_parameter_index(digits, length, &index)) {
        /* Bash retries an overflowing braced index as a dollar digit. */
        const char* value = NULL;
        if (ash_shell_policy_valid(&shell->policy) && ash_shell_policy_is_bash(&shell->policy)) {
            value = ash_positional(shell, (unsigned int)(digits[0] - '0'));
            if (value == NULL && (shell->options & ASH_SHELL_OPTION_NOUNSET) != 0u) {
                char fallback[] = {'$', digits[0]};
                ash_diag_unbound_parameter(shell, fallback, sizeof(fallback));
                if (ash_expansion_errexit(shell)) {
                    return ash_expansion_fail(shell, true);
                }
            }
        }
        return ash_append_parameter_value(shell, output, value, name, name_length, measure);
    }
    return ash_append_parameter_value(shell, output, ash_positional(shell, index), name, name_length, measure);
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
    const struct ash_positional_frame* positionals =
        ash_scope_positionals(shell);
    if (positionals == NULL) {
        return true;
    }
    const char* ifs = ash_ifs_joiner(shell);
    size_t separator_length = 0u;
    if (positionals->count > 1u && ifs[0] != '\0') {
        mbstate_t state = {0};
        separator_length = ash_character_width(ifs, strnlen(ifs, MB_CUR_MAX), &state);
    }
    for (size_t i = 0u; i < positionals->count; i++) {
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
                positionals->values[i]
            )) {
            return false;
        }
    }
    return true;
}

static bool ash_append_special(struct ash_shell* shell, char parameter, struct bx_text_buffer* output, bool braced, bool length) {
    char number[32];
    switch (parameter) {
        case '?':
            snprintf(number, sizeof(number), "%d", shell->last_status);
            return ash_append_parameter_value(shell, output, number, NULL, 0u, length);
        case '$':
            snprintf(number, sizeof(number), "%ld", (long)shell->shell_pid);
            return ash_append_parameter_value(shell, output, number, NULL, 0u, length);
        case '#': {
            const struct ash_positional_frame* positionals =
                ash_scope_positionals(shell);
            snprintf(
                number,
                sizeof(number),
                "%zu",
                positionals != NULL ? positionals->count : 0u
            );
            return ash_append_parameter_value(shell, output, number, NULL, 0u, length);
        }
        case '-': {
            char letters[16];
            ash_shell_option_letters(shell, letters, sizeof(letters));
            return ash_append_parameter_value(shell, output, letters, NULL, 0u, length);
        }
        case '!':
            if (shell->last_async_pid <= 0) {
                return ash_append_parameter_value(shell, output, NULL, braced ? "!" : "$!", braced ? 1u : 2u, length);
            }
            snprintf(
                number,
                sizeof(number),
                "%ld",
                (long)shell->last_async_pid
            );
            return ash_append_parameter_value(shell, output, number, NULL, 0u, length);
        case '@':
        case '*':
            return ash_append_positionals_joined(shell, output);
        default:
            return false;
    }
}

struct ash_parameter_reference {
    size_t start;
    size_t end;
    bool numbered;
    bool special;
    bool measure;
};

static bool ash_parameter_reference_parse(const char* input, struct ash_parameter_reference* reference) {
    if (input[0] != '$' || input[1] != '{') {
        return false;
    }
    size_t position = 2u;
    bool count_operator = input[position] == '#' && (input[position + 1u] == ':' ||
                                                     (input[position + 1u] != '\0' && strchr("-+?=", input[position + 1u]) != NULL && input[position + 2u] != '\0' && input[position + 2u] != '}'));
    reference->measure = input[position] == '#' && input[position + 1u] != '}' && !count_operator;
    position += reference->measure ? 1u : 0u;
    reference->start = position;
    reference->numbered = isdigit((unsigned char)input[position]) != 0;
    reference->special = input[position] != '\0' && strchr("?$#-!@*", input[position]) != NULL;
    if (reference->numbered) {
        while (isdigit((unsigned char)input[position])) {
            position++;
        }
    }
    else if (reference->special) {
        position++;
    }
    else {
        if (!ash_is_name_start((unsigned char)input[position])) {
            return false;
        }
        while (ash_is_name_char((unsigned char)input[position])) {
            position++;
        }
    }
    reference->end = position;
    return true;
}

static bool ash_append_parameter_reference(struct ash_shell* shell, const char* input, const struct ash_parameter_reference* reference, struct bx_text_buffer* output) {
    const char* name = input + reference->start;
    size_t length = reference->end - reference->start;
    if (reference->numbered) {
        return ash_append_numbered_parameter(shell, output, name, length, true, reference->measure);
    }
    if (reference->special) {
        return ash_append_special(shell, name[0], output, true, reference->measure);
    }
    return ash_append_parameter_value(shell, output, ash_var_get_len(shell, name, length), name, length, reference->measure);
}

static bool ash_expand_parameter(struct ash_shell* shell, const struct ash_word_part* part, struct bx_text_buffer* output) {
    const char* input = part->text;
    size_t position = 1u;
    char character = input[position];
    if (character == '\0') {
        return ash_expansion_append_char(shell, output, '$');
    }

    if (strchr("?$#-!@*", character) != NULL) {
        return input[position + 1u] == '\0' && ash_append_special(shell, character, output, false, false);
    }

    if (character == '{') {
        struct ash_parameter_reference reference;
        if (!ash_parameter_reference_parse(input, &reference)) {
            return ash_expansion_bad_substitution(shell, input);
        }
        position = reference.end;
        if (input[position] != '}' || input[position + 1u] != '\0' || (reference.special && reference.measure && strchr("@*", input[reference.start]) != NULL)) {
            return ash_expansion_bad_substitution(shell, input);
        }
        return ash_append_parameter_reference(shell, input, &reference, output);
    }

    if (isdigit((unsigned char)character)) {
        if (input[position + 1u] != '\0') {
            return ash_expansion_bad_substitution(shell, input);
        }
        return ash_append_numbered_parameter(shell, output, input + position, 1u, false, false);
    }

    if (!ash_is_name_start((unsigned char)character)) {
        return ash_expansion_append_text(shell, output, input);
    }
    size_t start = position;
    while (ash_is_name_char((unsigned char)input[position])) {
        position++;
    }
    if (input[position] != '\0') {
        return ash_expansion_bad_substitution(shell, input);
    }
    const char* value = ash_var_get_len(
        shell,
        input + start,
        position - start
    );
    return ash_append_parameter_value(shell, output, value, input + start, position - start, false);
}

static bool ash_expand_part(
    struct ash_shell* shell,
    const struct ash_word_part* part,
    struct bx_text_buffer* output,
    int* substitution_status
) {
    if (part->kind == ASH_WORD_PROCESS_SUBSTITUTION) {
        ash_diag(shell, "process substitution is unavailable");
        return false;
    }
    if (part->kind == ASH_WORD_PARAMETER) {
        return ash_expand_parameter(shell, part, output);
    }
    if (part->kind == ASH_WORD_COMMAND_SUBSTITUTION ||
        part->kind == ASH_WORD_BACKQUOTE) {
        size_t prefix = part->kind == ASH_WORD_COMMAND_SUBSTITUTION ?
            2u : 1u;
        size_t suffix = 1u;
        if (part->length < prefix + suffix ||
            shell->command_substitution == NULL) {
            ash_diag(shell, "command substitution is unavailable");
            return false;
        }

        char* substitution = NULL;
        int status;
        bool expanded = shell->command_substitution(
            shell,
            part->text + prefix,
            part->length - prefix - suffix,
            &substitution,
            &status
        );
        if (expanded) {
            if (substitution_status != NULL) {
                *substitution_status = status;
            }
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
        return ash_quote_append_dollar_single(output, part->text, part->length) || ash_expansion_oom(shell);
    }
    return ash_expansion_append_span(
        shell,
        output,
        part->text,
        part->length
    );
}

static bool ash_append_pattern_span(
    struct ash_shell* shell,
    struct bx_text_buffer* output,
    const char* text,
    size_t length,
    bool quoted
) {
    for (size_t i = 0u; i < length; i++) {
        char character = text[i];
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

static bool ash_append_pattern_component(
    struct ash_shell* shell,
    struct bx_text_buffer* output,
    const struct bx_text_buffer* component,
    bool quoted
) {
    return ash_append_pattern_span(
        shell,
        output,
        component->data,
        component->length,
        quoted
    );
}

struct ash_tilde_prefix {
    const char* value;
    char* owned;
    size_t length;
};

static bool ash_tilde_prefix_expand(struct ash_shell* shell, const struct ash_word_part* part, bool word_end, bool assignment, struct ash_tilde_prefix* prefix) {
    *prefix = (struct ash_tilde_prefix){0};
    if (part->kind != ASH_WORD_TEXT || ash_word_part_is_quoted(part) || part->length == 0u || part->text[0] != '~') {
        return true;
    }
    size_t length = 1u;
    while (length < part->length && part->text[length] != '/' && !(assignment && part->text[length] == ':')) {
        unsigned char c = (unsigned char)part->text[length];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) {
            return true;
        }
        length++;
    }
    if (length == part->length && !word_end) {
        return true;
    }
    if (length == 1u) {
        prefix->value = ash_var_get(shell, "HOME");
        prefix->length = prefix->value != NULL ? 1u : 0u;
        return true;
    }
    if (part->text[1] == '-') {
        return true;
    }
    char* name = strndup(part->text, length);
    if (name == NULL) {
        return ash_expansion_oom(shell);
    }
    prefix->owned = bx_path_expand_tilde_dup(name, NULL);
    if (prefix->owned == NULL) {
        int error = errno;
        if (error == ENOMEM || error == EOVERFLOW) {
            (void)ash_expansion_oom(shell);
        }
        else {
            ash_diag_expansion(shell, "%s: tilde lookup: %s", name, strerror(error));
            (void)ash_expansion_fail(shell, ash_shell_policy_noninteractive_posix(&shell->policy));
        }
        free(name);
        return false;
    }
    free(name);
    prefix->value = prefix->owned;
    prefix->length = length;
    return true;
}

static bool ash_leading_tilde(struct ash_shell* shell, const struct ash_word* word, struct ash_tilde_prefix* prefix) {
    *prefix = (struct ash_tilde_prefix){0};
    return word->count == 0u || ash_tilde_prefix_expand(shell, &word->parts[0], word->count == 1u, false, prefix);
}

static bool ash_expand_assignment_text(struct ash_shell* shell, const struct ash_word_part* part, bool word_end, bool* value_started, bool* tilde_position, struct bx_text_buffer* output) {
    struct ash_word_part remainder = *part;
    if (!*value_started) {
        const char* equal = memchr(remainder.text, '=', remainder.length);
        size_t length = equal != NULL ? (size_t)(equal - remainder.text) + 1u : remainder.length;
        if (!ash_expansion_append_span(shell, output, remainder.text, length)) {
            return false;
        }
        remainder.text += length;
        remainder.length -= length;
        *value_started = equal != NULL;
        *tilde_position = equal != NULL;
    }
    while (remainder.length != 0u) {
        if (*tilde_position) {
            struct ash_tilde_prefix prefix;
            if (!ash_tilde_prefix_expand(shell, &remainder, word_end, true, &prefix)) {
                return false;
            }
            bool appended = prefix.value == NULL || ash_expansion_append_text(shell, output, prefix.value);
            free(prefix.owned);
            if (!appended) {
                return false;
            }
            remainder.text += prefix.length;
            remainder.length -= prefix.length;
        }
        const char* colon = memchr(remainder.text, ':', remainder.length);
        size_t length = colon != NULL ? (size_t)(colon - remainder.text) + 1u : remainder.length;
        if (!ash_expansion_append_span(shell, output, remainder.text, length)) {
            return false;
        }
        remainder.text += length;
        remainder.length -= length;
        *tilde_position = colon != NULL;
    }
    return true;
}

enum ash_parameter_selection_kind {
    ASH_PARAMETER_UNMODIFIED,
    ASH_PARAMETER_SELECTED_VALUE,
    ASH_PARAMETER_SELECTED_WORD,
    ASH_PARAMETER_SELECTED_EMPTY,
};

enum ash_operand_action {
    ASH_OPERAND_SUBSTITUTE,
    ASH_OPERAND_ASSIGN,
    ASH_OPERAND_ERROR,
    ASH_OPERAND_ARITHMETIC,
    ASH_OPERAND_REMOVE_PREFIX,
    ASH_OPERAND_REMOVE_SUFFIX,
};

struct ash_parameter_selection {
    enum ash_parameter_selection_kind kind;
    struct bx_text_buffer value;
    struct ash_word word;
    const char* target_name;
    size_t target_length;
    enum ash_operand_action action;
    const char* error_default;
    bool longest;
    bool result_quoted;
};

static void ash_parameter_selection_destroy(struct ash_parameter_selection* selection) {
    bx_text_buffer_destroy(&selection->value);
    ash_word_destroy(&selection->word);
}

static bool ash_parameter_operand_word(struct ash_shell* shell, const struct ash_word_part* part, size_t operand_position, bool double_quoted, struct ash_word* word);

static bool ash_parameter_reference_state(const struct ash_shell* shell, const char* input, const struct ash_parameter_reference* reference, bool* null) {
    const char* name = input + reference->start;
    const char* value;
    if (reference->numbered) {
        uintmax_t index;
        if (ash_parameter_index(name, reference->end - reference->start, &index)) {
            value = ash_positional(shell, index);
        }
        else {
            value = ash_shell_policy_valid(&shell->policy) && ash_shell_policy_is_bash(&shell->policy) ? ash_positional(shell, (unsigned int)(name[0] - '0')) : NULL;
        }
    }
    else if (reference->special) {
        *null = false;
        if (name[0] == '!') {
            return shell->last_async_pid > 0;
        }
        if (name[0] == '-') {
            char letters[16];
            ash_shell_option_letters(shell, letters, sizeof(letters));
            *null = letters[0] == '\0';
        }
        return true;
    }
    else {
        value = ash_var_get_len(shell, name, reference->end - reference->start);
    }
    *null = value != NULL && value[0] == '\0';
    return value != NULL;
}

static bool ash_select_parameter_operand(struct ash_shell* shell, const struct ash_word_part* part, struct ash_parameter_selection* selection) {
    *selection = (struct ash_parameter_selection){0};
    struct ash_parameter_reference reference;
    if (part->kind != ASH_WORD_PARAMETER || !ash_parameter_reference_parse(part->text, &reference) || reference.measure) {
        return true;
    }
    size_t position = reference.end;
    char removal = part->text[position];
    if (removal == '#' || removal == '%') {
        if (part->length == 0u || part->text[part->length - 1u] != '}' || (reference.special && strchr("#@*", part->text[reference.start]) != NULL)) {
            return ash_expansion_bad_substitution(shell, part->text);
        }
        selection->kind = ASH_PARAMETER_SELECTED_WORD;
        selection->action = removal == '#' ? ASH_OPERAND_REMOVE_PREFIX : ASH_OPERAND_REMOVE_SUFFIX;
        selection->longest = part->text[position + 1u] == removal;
        selection->result_quoted = ash_word_part_is_quoted(part);
        if (!ash_append_parameter_reference(shell, part->text, &reference, &selection->value) ||
            !ash_parameter_operand_word(shell, part, position + (selection->longest ? 2u : 1u), false, &selection->word)) {
            ash_parameter_selection_destroy(selection);
            return false;
        }
        return true;
    }
    bool colon = part->text[position] == ':';
    position += colon ? 1u : 0u;
    char operator = part->text[position];
    if (operator != '-' && operator != '+' && operator != '=' && operator != '?') {
        return true;
    }
    if (part->length == 0u || part->text[part->length - 1u] != '}' || (reference.special && strchr("@*", part->text[reference.start]) != NULL)) {
        return ash_expansion_bad_substitution(shell, part->text);
    }
    if (operator == '=' && (reference.numbered || reference.special)) {
        return ash_expansion_bad_substitution(shell, part->text);
    }
    bool null;
    bool set = ash_parameter_reference_state(shell, part->text, &reference, &null);
    bool missing = !set || (colon && null);
    bool use_word = operator == '+' ? !missing : missing;
    if (use_word) {
        selection->kind = ASH_PARAMETER_SELECTED_WORD;
        if (!ash_parameter_operand_word(shell, part, position + 1u, ash_word_part_is_quoted(part), &selection->word)) {
            ash_parameter_selection_destroy(selection);
            return false;
        }
        if (operator == '=' || operator == '?') {
            selection->action = operator == '=' ? ASH_OPERAND_ASSIGN : ASH_OPERAND_ERROR;
            if (operator == '?' && position + 1u == part->length - 1u) {
                selection->error_default = colon ? "parameter is unset or null" : "parameter is unset";
            }
            selection->target_name = part->text + reference.start;
            selection->target_length = reference.end - reference.start;
        }
        return true;
    }
    if (operator == '+') {
        selection->kind = ASH_PARAMETER_SELECTED_EMPTY;
        return true;
    }
    selection->kind = ASH_PARAMETER_SELECTED_VALUE;
    if (!ash_append_parameter_reference(shell, part->text, &reference, &selection->value)) {
        ash_parameter_selection_destroy(selection);
        return false;
    }
    return true;
}

struct ash_operand_frame {
    const struct ash_word* borrowed;
    struct ash_word owned;
    size_t index;
    size_t tilde_length;
    bool entered;
    bool assignment;
    bool value_started;
    bool tilde_position;
    bool operand;
    /* Computed and error operands capture without emitting outer fields. */
    bool capture_only;
    bool result_quoted;
    /* Targets borrow the suspended parent word's spelling. */
    const char* target_name;
    size_t target_length;
    enum ash_operand_action action;
    const char* error_default;
    size_t capture_index;
    /* Assignment captures plain data while its enclosing removal keeps quotes. */
    size_t pattern_index;
    struct bx_text_buffer capture;
    struct bx_text_buffer snapshot;
    bool longest;
};

struct ash_operand_frames {
    struct ash_operand_frame* values;
    size_t count;
    size_t capacity;
    struct ash_operand_frame inline_values[8];
};

static void ash_operand_frames_init(struct ash_operand_frames* frames, const struct ash_word* word, bool assignment) {
    *frames = (struct ash_operand_frames){.values = frames->inline_values, .count = 1u, .capacity = sizeof(frames->inline_values) / sizeof(frames->inline_values[0])};
    frames->values[0] = (struct ash_operand_frame){.borrowed = word, .assignment = assignment, .capture_index = SIZE_MAX, .pattern_index = SIZE_MAX};
}

static void ash_operand_frames_pop(struct ash_operand_frames* frames) {
    struct ash_operand_frame* frame = &frames->values[--frames->count];
    ash_word_destroy(&frame->owned);
    bx_text_buffer_destroy(&frame->capture);
    bx_text_buffer_destroy(&frame->snapshot);
}

static void ash_operand_frames_destroy(struct ash_operand_frames* frames) {
    while (frames->count != 0u) {
        ash_operand_frames_pop(frames);
    }
    if (frames->values != frames->inline_values) {
        free(frames->values);
    }
    *frames = (struct ash_operand_frames){0};
}

static bool ash_operand_frames_push(struct ash_shell* shell, struct ash_operand_frames* frames, struct ash_parameter_selection* selection) {
    if (frames->count == frames->capacity) {
        size_t capacity = frames->capacity * 2u;
        if (capacity < frames->capacity || capacity > SIZE_MAX / sizeof(*frames->values)) {
            return ash_expansion_oom(shell);
        }
        struct ash_operand_frame* grown;
        if (frames->values == frames->inline_values) {
            grown = malloc(capacity * sizeof(*grown));
            if (grown != NULL) {
                memcpy(grown, frames->values, frames->count * sizeof(*grown));
            }
        }
        else {
            grown = realloc(frames->values, capacity * sizeof(*grown));
        }
        if (grown == NULL) {
            return ash_expansion_oom(shell);
        }
        frames->values = grown;
        frames->capacity = capacity;
    }
    bool removal = selection->action == ASH_OPERAND_REMOVE_PREFIX || selection->action == ASH_OPERAND_REMOVE_SUFFIX;
    bool computed = removal || selection->action == ASH_OPERAND_ARITHMETIC;
    size_t capture_index = selection->target_name != NULL || computed ? frames->count : frames->values[frames->count - 1u].capture_index;
    size_t pattern_index =
        removal ? frames->count : (selection->action == ASH_OPERAND_ARITHMETIC || selection->action == ASH_OPERAND_ERROR ? SIZE_MAX : frames->values[frames->count - 1u].pattern_index);
    bool capture_only = selection->action == ASH_OPERAND_ERROR || computed || frames->values[frames->count - 1u].capture_only;
    frames->values[frames->count++] = (struct ash_operand_frame){
        .owned = selection->word,
        .operand = true,
        .target_name = selection->target_name,
        .target_length = selection->target_length,
        .action = selection->action,
        .error_default = selection->error_default,
        .capture_only = capture_only,
        .capture_index = capture_index,
        .pattern_index = pattern_index,
        .snapshot = selection->value,
        .longest = selection->longest,
        .result_quoted = selection->result_quoted,
    };
    selection->word = (struct ash_word){0};
    selection->value = (struct bx_text_buffer){0};
    return true;
}

static bool ash_operand_frames_push_arithmetic(struct ash_shell* shell, struct ash_operand_frames* frames, const struct ash_word_part* part) {
    if (part->length < 5u || memcmp(part->text, "$((", 3u) != 0 || memcmp(part->text + part->length - 2u, "))", 2u) != 0) {
        ash_diag_expansion(shell, "arithmetic: invalid expansion");
        return ash_expansion_fail(shell, ash_shell_policy_noninteractive_posix(&shell->policy));
    }
    const char* source = part->raw_source != NULL ? part->raw_source : part->text;
    size_t length = part->raw_source != NULL ? part->raw_length : part->length;
    size_t position = 0u;
    for (size_t i = 0u; i < 3u; i++) {
        while (length - position >= 2u && source[position] == '\\' && source[position + 1u] == '\n') {
            position += 2u;
        }
        if (position == length || source[position++] != "$(("[i]) {
            ash_diag_expansion(shell, "arithmetic: invalid expansion");
            return ash_expansion_fail(shell, ash_shell_policy_noninteractive_posix(&shell->policy));
        }
    }
    size_t end = length;
    for (size_t i = 0u; i < 2u; i++) {
        while (end >= 2u && source[end - 2u] == '\\' && source[end - 1u] == '\n') {
            end -= 2u;
        }
        if (end == 0u || source[--end] != ')') {
            ash_diag_expansion(shell, "arithmetic: invalid expansion");
            return ash_expansion_fail(shell, ash_shell_policy_noninteractive_posix(&shell->policy));
        }
    }
    if (end < position) {
        ash_diag_expansion(shell, "arithmetic: invalid expansion");
        return ash_expansion_fail(shell, ash_shell_policy_noninteractive_posix(&shell->policy));
    }
    struct ash_source_location origin = part->location;
    if (ash_source_location_is_none(&origin)) {
        origin = (struct ash_source_location){.source = "<arithmetic>", .line = 1u, .column = 1u};
    }
    if (!ash_source_location_valid(&origin) || length > SIZE_MAX - origin.offset) {
        ash_diag_expansion(shell, "arithmetic operand source position overflow");
        return ash_expansion_fail(shell, ash_shell_policy_noninteractive_posix(&shell->policy));
    }
    struct ash_lexer lexer;
    ash_lexer_init_at(&lexer, origin, source, position);
    lexer.here_document_source = part->here_document_source;
    lexer.here_document_strip_tabs = part->here_document_strip_tabs;
    if (!ash_lexer_discard_remaining(&lexer)) {
        ash_diag_expansion(shell, "arithmetic operand source position overflow");
        return ash_expansion_fail(shell, ash_shell_policy_noninteractive_posix(&shell->policy));
    }
    lexer.length = end;
    struct ash_parameter_selection selection = {.action = ASH_OPERAND_ARITHMETIC};
    bool expanded = ash_lexer_scan_expansion_string(&lexer, &selection.word, ASH_QUOTE_DOUBLE) == ASH_LEXER_TOKEN;
    if (!expanded) {
        if (lexer.error != NULL && strcmp(lexer.error, "out of memory") == 0) {
            (void)ash_expansion_oom(shell);
        }
        else {
            ash_diag_parse(shell, lexer.error_location, "%s", lexer.error != NULL ? lexer.error : "invalid arithmetic operand");
            (void)ash_expansion_fail(shell, ash_shell_policy_noninteractive_posix(&shell->policy));
        }
    }
    else {
        expanded = ash_operand_frames_push(shell, frames, &selection);
        if (expanded) {
            frames->values[frames->count - 1u].result_quoted = ash_word_part_is_quoted(part);
        }
    }
    ash_parameter_selection_destroy(&selection);
    return expanded;
}

static bool ash_operand_capture(struct ash_shell* shell, struct ash_operand_frames* frames, const char* text, size_t length, bool quoted) {
    size_t index = frames->values[frames->count - 1u].capture_index;
    size_t pattern = frames->values[frames->count - 1u].pattern_index;
    return (index == SIZE_MAX || index == pattern || length == 0u || ash_expansion_append_span(shell, &frames->values[index].capture, text, length)) &&
           (pattern == SIZE_MAX || ash_append_pattern_span(shell, &frames->values[pattern].capture, text, length, quoted));
}

struct ash_operand_completion {
    char number[sizeof(long) * CHAR_BIT + 2u];
    const char* text;
    size_t length;
    struct bx_text_buffer owned;
    bool produced;
    bool quoted;
};

static bool ash_parameter_remove_match(struct ash_shell* shell, struct ash_operand_frame* frame, struct ash_operand_completion* completion);

static bool ash_operand_frames_complete(struct ash_shell* shell, struct ash_operand_frames* frames, struct ash_operand_completion* completion) {
    *completion = (struct ash_operand_completion){0};
    struct ash_operand_frame* frame = &frames->values[frames->count - 1u];
    if (frame->action == ASH_OPERAND_ARITHMETIC) {
        struct ash_arithmetic_error error;
        long number;
        const char* expression = frame->capture.data != NULL ? frame->capture.data : "";
        if (!ash_arithmetic_evaluate(shell, expression, frame->capture.length, &number, &error)) {
            if (error.reported) {
                return false;
            }
            if (error.name != NULL) {
                ash_diag_parameter_error(shell, error.name, error.name_length, error.message);
            }
            else {
                ash_diag_expansion(shell, "arithmetic: %s", error.message);
            }
            return ash_expansion_fail(shell, ash_shell_policy_noninteractive_posix(&shell->policy));
        }
        snprintf(completion->number, sizeof(completion->number), "%ld", number);
        completion->text = completion->number;
        completion->length = strlen(completion->text);
        completion->produced = true;
        completion->quoted = frame->result_quoted;
        ash_operand_frames_pop(frames);
        return ash_operand_capture(shell, frames, completion->text, completion->length, completion->quoted);
    }
    if (frame->action == ASH_OPERAND_REMOVE_PREFIX || frame->action == ASH_OPERAND_REMOVE_SUFFIX) {
        if (!ash_parameter_remove_match(shell, frame, completion)) {
            return false;
        }
        ash_operand_frames_pop(frames);
        return ash_operand_capture(shell, frames, completion->text, completion->length, completion->quoted);
    }
    if (frame->action == ASH_OPERAND_ERROR) {
        const char* message = frame->error_default != NULL ? frame->error_default : (frame->capture.data != NULL ? frame->capture.data : "");
        ash_diag_parameter_error(shell, frame->target_name, frame->target_length, message);
        return ash_expansion_fail(shell, !ash_shell_policy_has(&shell->policy, ASH_SHELL_POLICY_INTERACTIVE));
    }
    if (frame->action == ASH_OPERAND_ASSIGN) {
        const char* value = frame->capture.data != NULL ? frame->capture.data : "";
        if (!ash_var_set_with_export(shell, frame->target_name, frame->target_length, value, false)) {
            return false;
        }
        size_t parent = frames->values[frames->count - 2u].capture_index;
        if (parent != SIZE_MAX && parent != frames->values[frames->count - 2u].pattern_index && frame->capture.length != 0u &&
            !ash_expansion_append_span(shell, &frames->values[parent].capture, frame->capture.data, frame->capture.length)) {
            return false;
        }
    }
    ash_operand_frames_pop(frames);
    return true;
}

static const struct ash_word* ash_operand_frame_word(const struct ash_operand_frame* frame) {
    return frame->borrowed != NULL ? frame->borrowed : &frame->owned;
}

static bool ash_expansion_refresh_locale(struct ash_shell* shell) {
    int error = ash_locale_refresh(shell);
    if (error != 0) {
        ash_exec_error(shell, "locale", error);
        return ash_expansion_fail(shell, ash_shell_policy_noninteractive_posix(&shell->policy));
    }
    return true;
}

static bool ash_expand_buffers(struct ash_shell* shell, const struct ash_word* word, struct bx_text_buffer* value, struct bx_text_buffer* pattern, bool assignment, int* substitution_status) {
    if (!ash_expansion_refresh_locale(shell)) {
        return false;
    }
    struct ash_operand_frames frames;
    ash_operand_frames_init(&frames, word, assignment);
    while (frames.count != 0u) {
        struct ash_operand_frame* frame = &frames.values[frames.count - 1u];
        const struct ash_word* active_word = ash_operand_frame_word(frame);
        struct bx_text_buffer* result_value = frame->capture_only ? NULL : value;
        struct bx_text_buffer* result_pattern = frame->capture_only ? NULL : pattern;
        if (!frame->entered) {
            struct ash_tilde_prefix prefix = {0};
            if (!frame->assignment && !ash_leading_tilde(shell, active_word, &prefix)) {
                goto fail;
            }
            bool prefix_appended = prefix.value == NULL || ((result_value == NULL || ash_expansion_append_text(shell, result_value, prefix.value)) &&
                                                            (result_pattern == NULL || ash_append_pattern_span(shell, result_pattern, prefix.value, strlen(prefix.value), true)) &&
                                                            ash_operand_capture(shell, &frames, prefix.value, strlen(prefix.value), true));
            free(prefix.owned);
            if (!prefix_appended) {
                goto fail;
            }
            frame->tilde_length = prefix.length;
            frame->entered = true;
        }
        if (frame->index == active_word->count) {
            struct ash_operand_completion completion;
            bool appended = ash_operand_frames_complete(shell, &frames, &completion);
            if (appended && completion.produced && !frames.values[frames.count - 1u].capture_only) {
                appended = (value == NULL || ash_expansion_append_span(shell, value, completion.text, completion.length)) &&
                           (pattern == NULL || ash_append_pattern_span(shell, pattern, completion.text, completion.length, completion.quoted));
            }
            bx_text_buffer_destroy(&completion.owned);
            if (!appended) {
                goto fail;
            }
            continue;
        }
        size_t index = frame->index++;
        const struct ash_word_part* part = &active_word->parts[index];
        if (frame->assignment && part->kind == ASH_WORD_TEXT && !ash_word_part_is_quoted(part)) {
            if (!ash_expand_assignment_text(shell, part, frame->index == active_word->count, &frame->value_started, &frame->tilde_position, value)) {
                goto fail;
            }
            continue;
        }
        frame->tilde_position = false;
        struct ash_word_part adjusted;
        if (index == 0u && frame->tilde_length != 0u) {
            adjusted = *part;
            adjusted.text += frame->tilde_length;
            adjusted.length -= frame->tilde_length;
            part = &adjusted;
        }
        if (part->kind == ASH_WORD_ARITHMETIC) {
            if (!ash_operand_frames_push_arithmetic(shell, &frames, part)) {
                goto fail;
            }
            continue;
        }
        struct ash_parameter_selection selection;
        if (!ash_select_parameter_operand(shell, part, &selection)) {
            goto fail;
        }
        if (selection.kind != ASH_PARAMETER_UNMODIFIED) {
            bool appended = selection.kind == ASH_PARAMETER_SELECTED_WORD
                                ? ash_operand_frames_push(shell, &frames, &selection)
                                : ((result_value == NULL || ash_expansion_append_span(shell, result_value, selection.value.data, selection.value.length)) &&
                                   (result_pattern == NULL || ash_append_pattern_component(shell, result_pattern, &selection.value, ash_word_part_is_quoted(part))) &&
                                   ash_operand_capture(shell, &frames, selection.value.data, selection.value.length, ash_word_part_is_quoted(part)));
            ash_parameter_selection_destroy(&selection);
            if (!appended) {
                goto fail;
            }
            continue;
        }
        struct bx_text_buffer component;
        bx_text_buffer_init(&component);
        if (!ash_expand_part(shell, part, &component, substitution_status)) {
            bx_text_buffer_destroy(&component);
            goto fail;
        }

        bool appended = (result_value == NULL || ash_expansion_append_span(shell, result_value, component.data, component.length)) &&
                        (result_pattern == NULL || ash_append_pattern_component(shell, result_pattern, &component, ash_word_part_is_quoted(part))) &&
                        ash_operand_capture(shell, &frames, component.data, component.length, ash_word_part_is_quoted(part));
        bx_text_buffer_destroy(&component);
        if (!appended) {
            goto fail;
        }
    }
    ash_operand_frames_destroy(&frames);
    return true;
fail:
    ash_operand_frames_destroy(&frames);
    return false;
}

static bool ash_parameter_operand_word(struct ash_shell* shell, const struct ash_word_part* part, size_t operand_position, bool double_quoted, struct ash_word* word) {
    const char* source = part->raw_source != NULL ? part->raw_source : part->text;
    size_t length = part->raw_source != NULL ? part->raw_length : part->length;
    if (length == 0u || source[length - 1u] != '}') {
        return ash_expansion_bad_substitution(shell, part->text);
    }
    size_t position = 0u;
    for (size_t logical = 0u; logical < operand_position;) {
        if (position >= length - 1u) {
            return ash_expansion_bad_substitution(shell, part->text);
        }
        if (source[position] == '\\' && source[position + 1u] == '\n') {
            position += 2u;
        }
        else {
            position++;
            logical++;
        }
    }
    struct ash_source_location origin = part->location;
    if (ash_source_location_is_none(&origin)) {
        origin = (struct ash_source_location){.source = "<parameter>", .line = 1u, .column = 1u};
    }
    if (!ash_source_location_valid(&origin) || length > SIZE_MAX - origin.offset) {
        ash_diag_expansion(shell, "parameter operand source position overflow");
        return ash_expansion_fail(shell, ash_shell_policy_noninteractive_posix(&shell->policy));
    }
    struct ash_lexer lexer;
    ash_lexer_init_at(&lexer, origin, source, position);
    lexer.here_document_source = part->here_document_source;
    lexer.here_document_strip_tabs = part->here_document_strip_tabs;
    if (!ash_lexer_discard_remaining(&lexer)) {
        ash_diag_expansion(shell, "parameter operand source position overflow");
        return ash_expansion_fail(shell, ash_shell_policy_noninteractive_posix(&shell->policy));
    }
    lexer.length = length - 1u;
    if (ash_lexer_scan_parameter_operand(&lexer, word, double_quoted) != ASH_LEXER_TOKEN) {
        if (lexer.error != NULL && strcmp(lexer.error, "out of memory") == 0) {
            return ash_expansion_oom(shell);
        }
        ash_diag_parse(shell, lexer.error_location, "%s", lexer.error != NULL ? lexer.error : "invalid parameter operand");
        return ash_expansion_fail(shell, ash_shell_policy_noninteractive_posix(&shell->policy));
    }
    return true;
}

static bool ash_parameter_remove_match(struct ash_shell* shell, struct ash_operand_frame* frame, struct ash_operand_completion* completion) {
    struct ash_pattern pattern = {0};
    bool prefix = frame->action == ASH_OPERAND_REMOVE_PREFIX;
    size_t length = frame->snapshot.length;
    char empty[] = "";
    char* text = frame->snapshot.data != NULL ? frame->snapshot.data : empty;
    bool expanded = false;
    const struct ash_pattern_options options = {.purpose = ASH_PATTERN_PARAMETER_REMOVE, .domain = ASH_PATTERN_STRING};
    enum ash_pattern_compile_result compiled = ash_pattern_compile(frame->capture.data != NULL ? frame->capture.data : "", frame->capture.length, &options, &pattern);
    if (compiled != ASH_PATTERN_COMPILE_OK) {
        if (compiled == ASH_PATTERN_COMPILE_NO_MEMORY) {
            (void)ash_expansion_oom(shell);
        }
        else {
            ash_diag_expansion(shell, "invalid parameter removal pattern");
            (void)ash_expansion_fail(shell, ash_shell_policy_noninteractive_posix(&shell->policy));
        }
        goto out;
    }
    /* Only this owned snapshot is temporarily terminated for prefix matching. */
    size_t selected = prefix ? 0u : length;
    mbstate_t state = {0};
    for (size_t position = 0u;;) {
        char saved = text[position];
        if (prefix) {
            text[position] = '\0';
        }
        enum ash_pattern_match_result match = ash_pattern_match(&pattern, text + (prefix ? 0u : position));
        text[position] = saved;
        if (match == ASH_PATTERN_MATCH_ERROR || match == ASH_PATTERN_MATCH_UNSUPPORTED) {
            ash_diag_expansion(shell, "parameter removal pattern matching failed");
            (void)ash_expansion_fail(shell, ash_shell_policy_noninteractive_posix(&shell->policy));
            goto out;
        }
        if (match == ASH_PATTERN_MATCH) {
            selected = position;
            if (prefix != frame->longest) {
                break;
            }
        }
        if (position == length) {
            break;
        }
        position += ash_character_width(text + position, length - position, &state);
    }
    completion->owned = frame->snapshot;
    frame->snapshot = (struct bx_text_buffer){0};
    completion->text = completion->owned.data != NULL ? completion->owned.data + (prefix ? selected : 0u) : "";
    completion->length = prefix ? length - selected : selected;
    completion->produced = true;
    completion->quoted = frame->result_quoted;
    expanded = true;
out:
    ash_pattern_destroy(&pattern);
    return expanded;
}

bool ash_expand(struct ash_shell* shell, const struct ash_word* word, enum ash_expansion_context context, char** output_word, int* substitution_status) {
    *output_word = NULL;
    struct bx_text_buffer output;
    bx_text_buffer_init(&output);
    bool pattern = context == ASH_EXPANSION_PATTERN;
    if (!ash_expand_buffers(shell, word, pattern ? NULL : &output, pattern ? &output : NULL, context == ASH_EXPANSION_ASSIGNMENT, substitution_status)) {
        bx_text_buffer_destroy(&output);
        return false;
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
    char** output_word,
    int* substitution_status
) {
    return ash_expand(
        shell, word, ASH_EXPANSION_WORD, output_word, substitution_status
    );
}

void ash_expanded_fields_init(struct ash_expanded_fields* fields) {
    *fields = (struct ash_expanded_fields){0};
}

bool ash_expand_here_document(struct ash_shell* shell,
                              const char* body,
                              size_t length,
                              const struct ash_source_location* location,
                              bool strip_tabs,
                              struct bx_text_buffer* output,
                              int* substitution_status) {
    bx_text_buffer_init(output);
    if (!ash_source_location_valid(location) || length > SIZE_MAX - location->offset) {
        ash_diag_expansion(shell, "here-document source position overflow");
        return ash_expansion_fail(shell, ash_shell_policy_noninteractive_posix(&shell->policy));
    }
    struct ash_lexer lexer;
    ash_lexer_init_at(&lexer, *location, body, length);
    struct ash_word word;
    enum ash_lexer_result result = ash_lexer_scan_here_document(&lexer, &word, strip_tabs);
    bool expanded = false;
    if (result != ASH_LEXER_TOKEN) {
        if (lexer.error != NULL && strcmp(lexer.error, "out of memory") == 0) {
            (void)ash_expansion_oom(shell);
        }
        else {
            ash_diag_expansion(shell, "here-document: %s", lexer.error != NULL ? lexer.error : "invalid expansion");
            (void)ash_expansion_fail(shell, ash_shell_policy_noninteractive_posix(&shell->policy));
        }
    }
    else {
        expanded = ash_expand_buffers(shell, &word, output, NULL, false, substitution_status);
    }
    ash_word_destroy(&word);
    if (!expanded) {
        bx_text_buffer_destroy(output);
    }
    return expanded;
}

void ash_expanded_fields_destroy(struct ash_expanded_fields* fields) {
    for (size_t i = 0u; i < fields->count; i++) {
        free(fields->values[i]);
    }
    free(fields->values);
    *fields = (struct ash_expanded_fields){0};
}

static bool ash_expanded_fields_reserve_one(
    struct ash_shell* shell,
    struct ash_expanded_fields* fields
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
    return true;
}

static size_t ash_ifs_delimiter_length(const char* ifs, size_t ifs_length, const char* text, size_t remaining) {
    if (MB_CUR_MAX == 1u) {
        return strchr(ifs, (unsigned char)text[0]) != NULL ? 1u : 0u;
    }
    mbstate_t state = {0};
    for (size_t position = 0u; position < ifs_length;) {
        size_t width = ash_character_width(ifs + position, ifs_length - position, &state);
        if (width <= remaining && memcmp(ifs + position, text, width) == 0) {
            return width;
        }
        position += width;
    }
    return 0u;
}

static bool ash_ifs_whitespace(const char* ifs, unsigned char character) {
    return (character == ' ' || character == '\t' || character == '\n') && strchr(ifs, (int)character) != NULL;
}

static bool ash_expanded_fields_push_span(
    struct ash_shell* shell,
    struct ash_expanded_fields* fields,
    const char* value,
    size_t length
) {
    if (length == SIZE_MAX) {
        return ash_expansion_oom(shell);
    }
    char* copy = malloc(length + 1u);
    if (copy == NULL) {
        return ash_expansion_oom(shell);
    }
    memcpy(copy, value, length);
    copy[length] = '\0';

    if (!ash_expanded_fields_reserve_one(shell, fields)) {
        free(copy);
        return false;
    }
    fields->values[fields->count++] = copy;
    return true;
}

static bool ash_expanded_fields_push(
    struct ash_shell* shell,
    struct ash_expanded_fields* fields,
    const char* value
) {
    return ash_expanded_fields_push_span(
        shell,
        fields,
        value,
        strlen(value)
    );
}

struct ash_expansion_component {
    struct bx_text_buffer text;
    bool split;
    bool quoted;
    bool new_field;
};

struct ash_expansion_components {
    struct ash_expansion_component* values;
    size_t count;
    size_t capacity;
};

static void ash_expansion_components_destroy(struct ash_expansion_components* components) {
    for (size_t i = 0u; i < components->count; i++) {
        bx_text_buffer_destroy(&components->values[i].text);
    }
    free(components->values);
}

static bool ash_expansion_components_push(struct ash_shell* shell, struct ash_expansion_components* components, struct bx_text_buffer* text, bool split, bool quoted, bool new_field) {
    if (components->count == components->capacity) {
        size_t capacity = components->capacity == 0u ? 4u : components->capacity * 2u;
        if (capacity < components->capacity || capacity > SIZE_MAX / sizeof(*components->values)) {
            return ash_expansion_oom(shell);
        }
        struct ash_expansion_component* grown = realloc(components->values, capacity * sizeof(*grown));
        if (grown == NULL) {
            return ash_expansion_oom(shell);
        }
        components->values = grown;
        components->capacity = capacity;
    }
    /* Retain the existing per-segment C-string policy for decoded NULs. */
    text->length = strnlen(text->data != NULL ? text->data : "", text->length);
    components->values[components->count++] = (struct ash_expansion_component){*text, split, quoted, new_field};
    *text = (struct bx_text_buffer){0};
    return true;
}

struct ash_split_candidate {
    struct bx_text_buffer value;
    struct bx_text_buffer pattern;
    bool present;
    bool whitespace;
};

static bool ash_split_candidate_emit(struct ash_shell* shell, struct ash_split_candidate* candidate, struct ash_expanded_fields* fields, struct ash_expanded_fields* patterns, bool force) {
    if (candidate->present || force) {
        if (!ash_expanded_fields_push_span(shell, fields, candidate->value.data != NULL ? candidate->value.data : "", candidate->value.length) ||
            (patterns != NULL && !ash_expanded_fields_push_span(shell, patterns, candidate->pattern.data != NULL ? candidate->pattern.data : "", candidate->pattern.length))) {
            return false;
        }
    }
    bx_text_buffer_clear(&candidate->value);
    bx_text_buffer_clear(&candidate->pattern);
    candidate->present = false;
    candidate->whitespace = false;
    return true;
}

static bool ash_split_candidate_append(struct ash_shell* shell, struct ash_split_candidate* candidate, bool pattern, const char* text, size_t length, bool quoted) {
    if (!ash_expansion_append_span(shell, &candidate->value, text, length) || (pattern && !ash_append_pattern_span(shell, &candidate->pattern, text, length, quoted))) {
        return false;
    }
    candidate->present = true;
    return true;
}

static bool ash_split_components(struct ash_shell* shell, const struct ash_expansion_components* components, struct ash_expanded_fields* fields, struct ash_expanded_fields* patterns) {
    const char* ifs = ash_var_get(shell, "IFS");
    if (ifs == NULL) {
        ifs = " \t\n";
    }
    size_t ifs_length = strlen(ifs);
    struct ash_split_candidate candidate = {0};
    bool ok = true;
    for (size_t i = 0u; ok && i < components->count; i++) {
        const struct ash_expansion_component* component = &components->values[i];
        if (component->new_field && !ash_split_candidate_emit(shell, &candidate, fields, patterns, false)) {
            ok = false;
            break;
        }
        const char* text = component->text.data != NULL ? component->text.data : "";
        size_t length = component->text.length;
        if (!component->split) {
            if (length == 0u && !component->quoted) {
                continue;
            }
            if (candidate.whitespace && !ash_split_candidate_emit(shell, &candidate, fields, patterns, false)) {
                ok = false;
                break;
            }
            ok = ash_split_candidate_append(shell, &candidate, patterns != NULL, text, length, component->quoted);
            continue;
        }
        for (size_t position = 0u; ok && position < length;) {
            size_t delimiter = ash_ifs_delimiter_length(ifs, ifs_length, text + position, length - position);
            if (delimiter != 0u) {
                if (ash_ifs_whitespace(ifs, (unsigned char)text[position])) {
                    candidate.whitespace = true;
                }
                else {
                    ok = ash_split_candidate_emit(shell, &candidate, fields, patterns, true);
                }
                position += delimiter;
                continue;
            }
            if (candidate.whitespace && !ash_split_candidate_emit(shell, &candidate, fields, patterns, false)) {
                ok = false;
                break;
            }
            size_t start = position++;
            while (position < length && ash_ifs_delimiter_length(ifs, ifs_length, text + position, length - position) == 0u) {
                position++;
            }
            ok = ash_split_candidate_append(shell, &candidate, patterns != NULL, text + start, position - start, false);
        }
    }
    if (ok) {
        ok = ash_split_candidate_emit(shell, &candidate, fields, patterns, false);
    }
    bx_text_buffer_destroy(&candidate.value);
    bx_text_buffer_destroy(&candidate.pattern);
    return ok;
}

static bool ash_expansion_pathname_failure(
    struct ash_shell* shell,
    const char* subject,
    int error
) {
    if (error == ENOMEM) {
        return ash_expansion_oom(shell);
    }
    ash_diag(
        shell,
        "%s: pathname expansion failed: %s",
        subject,
        strerror(error)
    );
    return false;
}

static bool ash_expand_pathnames(
    struct ash_shell* shell,
    struct ash_expanded_fields* fields,
    const struct ash_expanded_fields* patterns
) {
    if (fields->count != patterns->count) {
        errno = EINVAL;
        return false;
    }

    struct ash_expanded_fields expanded;
    ash_expanded_fields_init(&expanded);
    for (size_t i = 0u; i < fields->count; i++) {
        if (!ash_pathname_pattern_may_expand(patterns->values[i])) {
            if (!ash_expanded_fields_push(
                    shell,
                    &expanded,
                    fields->values[i]
                )) {
                ash_expanded_fields_destroy(&expanded);
                return false;
            }
            continue;
        }

        struct ash_pathname_matches matches;
        enum ash_pathname_expansion_result result = ash_pathname_expand(
            patterns->values[i],
            &matches
        );
        if (result == ASH_PATHNAME_EXPANSION_ERROR) {
            int error = errno;
            ash_expanded_fields_destroy(&expanded);
            return ash_expansion_pathname_failure(
                shell,
                fields->values[i],
                error
            );
        }
        if (result == ASH_PATHNAME_EXPANSION_NO_MATCH) {
            if (!ash_expanded_fields_push(
                    shell,
                    &expanded,
                    fields->values[i]
                )) {
                ash_expanded_fields_destroy(&expanded);
                return false;
            }
            continue;
        }

        for (size_t j = 0u; j < matches.count; j++) {
            if (!ash_expanded_fields_push(
                    shell,
                    &expanded,
                    matches.values[j]
                )) {
                ash_pathname_matches_destroy(&matches);
                ash_expanded_fields_destroy(&expanded);
                return false;
            }
        }
        ash_pathname_matches_destroy(&matches);
    }

    ash_expanded_fields_destroy(fields);
    *fields = expanded;
    return true;
}

enum ash_redirection_expansion_result ash_expand_redirection(
    struct ash_shell* shell,
    const struct ash_word* word,
    char** output,
    int* substitution_status
) {
    *output = NULL;
    bool needs_pattern = ash_shell_policy_globs_redirections(&shell->policy) &&
        ash_pathname_expansion_enabled(shell) &&
        ash_word_may_expand_pathname(word);
    struct bx_text_buffer value_buffer;
    struct bx_text_buffer pattern_buffer;
    bx_text_buffer_init(&value_buffer);
    bx_text_buffer_init(&pattern_buffer);
    if (!ash_expand_buffers(shell, word, &value_buffer, needs_pattern ? &pattern_buffer : NULL, false, substitution_status)) {
        bx_text_buffer_destroy(&value_buffer);
        bx_text_buffer_destroy(&pattern_buffer);
        return ASH_REDIRECTION_EXPANSION_ERROR;
    }
    char* value = bx_text_buffer_take(&value_buffer);
    char* pattern = value != NULL && needs_pattern ?
        bx_text_buffer_take(&pattern_buffer) : NULL;
    if (value == NULL || (needs_pattern && pattern == NULL)) {
        free(value);
        free(pattern);
        bx_text_buffer_destroy(&value_buffer);
        bx_text_buffer_destroy(&pattern_buffer);
        (void)ash_expansion_oom(shell);
        return ASH_REDIRECTION_EXPANSION_ERROR;
    }
    if (!needs_pattern || !ash_pathname_pattern_may_expand(pattern)) {
        free(pattern);
        *output = value;
        return ASH_REDIRECTION_EXPANSION_OK;
    }

    struct ash_pathname_matches matches;
    enum ash_pathname_expansion_result result = ash_pathname_expand(
        pattern,
        &matches
    );
    free(pattern);
    if (result == ASH_PATHNAME_EXPANSION_ERROR) {
        int error = errno;
        (void)ash_expansion_pathname_failure(shell, value, error);
        free(value);
        return ASH_REDIRECTION_EXPANSION_ERROR;
    }
    if (result == ASH_PATHNAME_EXPANSION_NO_MATCH) {
        *output = value;
        return ASH_REDIRECTION_EXPANSION_OK;
    }
    if (matches.count != 1u) {
        ash_diag(shell, "ambiguous redirect");
        ash_pathname_matches_destroy(&matches);
        free(value);
        return ASH_REDIRECTION_EXPANSION_AMBIGUOUS;
    }

    free(value);
    *output = matches.values[0];
    free(matches.values);
    return ASH_REDIRECTION_EXPANSION_OK;
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

static bool ash_part_requires_splitting(const struct ash_word_part* part) {
    return !ash_word_part_is_quoted(part) &&
        ash_word_part_is_expansion(part);
}

static bool ash_collect_components(struct ash_shell* shell, const struct ash_word* word, struct ash_expansion_components* components, int* substitution_status) {
    struct bx_text_buffer component = {0};
    struct ash_operand_frames frames;
    ash_operand_frames_init(&frames, word, false);
    while (frames.count != 0u) {
        struct ash_operand_frame* frame = &frames.values[frames.count - 1u];
        const struct ash_word* active_word = ash_operand_frame_word(frame);
        if (!frame->entered) {
            struct ash_tilde_prefix prefix;
            if (!ash_leading_tilde(shell, active_word, &prefix)) {
                goto fail;
            }
            if (prefix.value != NULL) {
                bool added = ash_expansion_append_text(shell, &component, prefix.value) && ash_operand_capture(shell, &frames, prefix.value, strlen(prefix.value), true);
                free(prefix.owned);
                if (!added || (!frame->capture_only && !ash_expansion_components_push(shell, components, &component, false, true, false))) {
                    goto fail;
                }
            }
            bx_text_buffer_destroy(&component);
            frame->tilde_length = prefix.length;
            frame->entered = true;
        }
        if (frame->index == active_word->count) {
            struct ash_operand_completion completion;
            bool collected = ash_operand_frames_complete(shell, &frames, &completion);
            if (collected && completion.produced && !frames.values[frames.count - 1u].capture_only) {
                collected = ash_expansion_append_span(shell, &component, completion.text, completion.length) &&
                            ash_expansion_components_push(shell, components, &component, !completion.quoted, completion.quoted, false);
            }
            bx_text_buffer_destroy(&completion.owned);
            if (!collected) {
                goto fail;
            }
            bx_text_buffer_destroy(&component);
            continue;
        }
        size_t index = frame->index++;
        const struct ash_word_part* part = &active_word->parts[index];
        struct ash_word_part adjusted;
        if (index == 0u && frame->tilde_length != 0u) {
            adjusted = *part;
            adjusted.text += frame->tilde_length;
            adjusted.length -= frame->tilde_length;
            part = &adjusted;
        }
        if (part->kind == ASH_WORD_ARITHMETIC) {
            if (!ash_operand_frames_push_arithmetic(shell, &frames, part)) {
                goto fail;
            }
            continue;
        }
        bool quoted = ash_word_part_is_quoted(part);
        struct ash_parameter_selection selection;
        if (!ash_select_parameter_operand(shell, part, &selection)) {
            goto fail;
        }
        if (selection.kind != ASH_PARAMETER_UNMODIFIED) {
            bool collected;
            if (selection.kind == ASH_PARAMETER_SELECTED_WORD) {
                collected = ash_operand_frames_push(shell, &frames, &selection);
            }
            else {
                component = selection.value;
                selection.value = (struct bx_text_buffer){0};
                collected = ash_operand_capture(shell, &frames, component.data, component.length, ash_word_part_is_quoted(part)) &&
                            (frame->capture_only || ash_expansion_components_push(shell, components, &component, !quoted, quoted, false));
                bx_text_buffer_destroy(&component);
            }
            ash_parameter_selection_destroy(&selection);
            if (!collected) {
                goto fail;
            }
            continue;
        }
        if (frame->capture_index == SIZE_MAX && (ash_parameter_is(part, '@') || (ash_parameter_is(part, '*') && !quoted))) {
            const struct ash_positional_frame* positionals = ash_scope_positionals(shell);
            for (size_t j = 0u; positionals != NULL && j < positionals->count; j++) {
                if (!ash_expansion_append_text(shell, &component, positionals->values[j]) || !ash_expansion_components_push(shell, components, &component, !quoted, quoted, j != 0u)) {
                    goto fail;
                }
            }
            continue;
        }
        bool split = ash_part_requires_splitting(part) || (frame->operand && !quoted && part->kind == ASH_WORD_TEXT);
        if (!ash_expand_part(shell, part, &component, substitution_status) || !ash_operand_capture(shell, &frames, component.data, component.length, ash_word_part_is_quoted(part)) ||
            (!frame->capture_only && !ash_expansion_components_push(shell, components, &component, split, quoted, false))) {
            goto fail;
        }
        bx_text_buffer_destroy(&component);
    }
    ash_operand_frames_destroy(&frames);
    return true;
fail:
    bx_text_buffer_destroy(&component);
    ash_operand_frames_destroy(&frames);
    return false;
}

bool ash_expand_argument(struct ash_shell* shell, const struct ash_word* word, struct ash_expanded_fields* fields, int* substitution_status) {
    ash_expanded_fields_init(fields);
    if (!ash_expansion_refresh_locale(shell)) {
        return false;
    }
    bool pathname_expansion = ash_pathname_expansion_enabled(shell) && ash_word_may_expand_pathname(word);
    struct ash_expanded_fields patterns = {0};
    struct ash_expanded_fields* active_patterns = pathname_expansion ? &patterns : NULL;
    struct ash_expansion_components components = {0};
    if (!ash_collect_components(shell, word, &components, substitution_status) || !ash_split_components(shell, &components, fields, active_patterns) ||
        (pathname_expansion && !ash_expand_pathnames(shell, fields, &patterns))) {
        goto fail;
    }
    ash_expansion_components_destroy(&components);
    ash_expanded_fields_destroy(&patterns);
    return true;

fail:
    ash_expansion_components_destroy(&components);
    ash_expanded_fields_destroy(&patterns);
    ash_expanded_fields_destroy(fields);
    return false;
}
