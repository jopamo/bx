#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "applets/shell/ash/arithmetic.h"
#include "applets/shell/ash/shell_context.h"
#include "applets/shell/ash/variables.h"

enum ash_arithmetic_operator {
    ARITH_OPEN,
    ARITH_ASSIGN,
    ARITH_ASSIGN_OR,
    ARITH_ASSIGN_XOR,
    ARITH_ASSIGN_AND,
    ARITH_ASSIGN_SHL,
    ARITH_ASSIGN_SHR,
    ARITH_ASSIGN_ADD,
    ARITH_ASSIGN_SUB,
    ARITH_ASSIGN_MUL,
    ARITH_ASSIGN_DIV,
    ARITH_ASSIGN_MOD,
    ARITH_QUESTION,
    ARITH_CONDITIONAL,
    ARITH_LOGICAL_OR,
    ARITH_LOGICAL_AND,
    ARITH_OR,
    ARITH_XOR,
    ARITH_AND,
    ARITH_EQ,
    ARITH_NE,
    ARITH_LT,
    ARITH_LE,
    ARITH_GT,
    ARITH_GE,
    ARITH_SHL,
    ARITH_SHR,
    ARITH_ADD,
    ARITH_SUB,
    ARITH_MUL,
    ARITH_DIV,
    ARITH_MOD,
    ARITH_POS,
    ARITH_NEG,
    ARITH_NOT,
    ARITH_INVERT,
};

struct ash_arithmetic_entry {
    long value;
    /* Retain the source name without reading an assignment target. */
    const char* name;
    size_t name_length;
    bool resolved;
    enum ash_arithmetic_operator operator;
    /* Lazy reductions restore the enclosing evaluation state. */
    bool suppressed;
};

struct ash_arithmetic_stack {
    struct ash_arithmetic_entry* entries;
    size_t count;
    size_t capacity;
    struct ash_arithmetic_entry initial[16];
};

static void ash_arithmetic_stack_init(struct ash_arithmetic_stack* stack) {
    *stack = (struct ash_arithmetic_stack){.entries = stack->initial, .capacity = sizeof(stack->initial) / sizeof(stack->initial[0])};
}

static void ash_arithmetic_stack_destroy(struct ash_arithmetic_stack* stack) {
    if (stack->entries != stack->initial) {
        free(stack->entries);
    }
}

static bool ash_arithmetic_push(struct ash_arithmetic_stack* stack, struct ash_arithmetic_entry entry) {
    if (stack->count == stack->capacity) {
        size_t capacity = stack->capacity * 2u;
        if (capacity < stack->capacity || capacity > SIZE_MAX / sizeof(*stack->entries)) {
            return false;
        }
        struct ash_arithmetic_entry* grown;
        if (stack->entries == stack->initial) {
            grown = malloc(capacity * sizeof(*grown));
            if (grown != NULL) {
                memcpy(grown, stack->entries, stack->count * sizeof(*grown));
            }
        }
        else {
            grown = realloc(stack->entries, capacity * sizeof(*grown));
        }
        if (grown == NULL) {
            return false;
        }
        stack->entries = grown;
        stack->capacity = capacity;
    }
    stack->entries[stack->count++] = entry;
    return true;
}

static bool ash_arithmetic_assignment(enum ash_arithmetic_operator operator) {
    return operator >= ARITH_ASSIGN && operator <= ARITH_ASSIGN_MOD;
}

static unsigned int ash_arithmetic_precedence(enum ash_arithmetic_operator operator) {
    switch (operator) {
        case ARITH_OPEN:
            return 0u;
        case ARITH_ASSIGN:
        case ARITH_ASSIGN_OR:
        case ARITH_ASSIGN_XOR:
        case ARITH_ASSIGN_AND:
        case ARITH_ASSIGN_SHL:
        case ARITH_ASSIGN_SHR:
        case ARITH_ASSIGN_ADD:
        case ARITH_ASSIGN_SUB:
        case ARITH_ASSIGN_MUL:
        case ARITH_ASSIGN_DIV:
        case ARITH_ASSIGN_MOD:
            return 1u;
        case ARITH_QUESTION:
        case ARITH_CONDITIONAL:
            return 2u;
        case ARITH_LOGICAL_OR:
            return 3u;
        case ARITH_LOGICAL_AND:
            return 4u;
        case ARITH_OR:
            return 5u;
        case ARITH_XOR:
            return 6u;
        case ARITH_AND:
            return 7u;
        case ARITH_EQ:
        case ARITH_NE:
            return 8u;
        case ARITH_LT:
        case ARITH_LE:
        case ARITH_GT:
        case ARITH_GE:
            return 9u;
        case ARITH_SHL:
        case ARITH_SHR:
            return 10u;
        case ARITH_ADD:
        case ARITH_SUB:
            return 11u;
        case ARITH_MUL:
        case ARITH_DIV:
        case ARITH_MOD:
            return 12u;
        case ARITH_POS:
        case ARITH_NEG:
        case ARITH_NOT:
        case ARITH_INVERT:
            return 13u;
    }
    return 0u;
}

static const char* ash_arithmetic_variable(const struct ash_shell* shell, const char* name, size_t length, long* result);

static const char* ash_arithmetic_resolve(const struct ash_shell* shell, struct ash_arithmetic_entry* entry, struct ash_arithmetic_error* diagnostic) {
    if (entry->name == NULL || entry->resolved) {
        return NULL;
    }
    const char* error = ash_arithmetic_variable(shell, entry->name, entry->name_length, &entry->value);
    if (error != NULL) {
        diagnostic->name = entry->name;
        diagnostic->name_length = entry->name_length;
    }
    else {
        entry->resolved = true;
    }
    return error;
}

static const char* ash_arithmetic_reduce(struct ash_shell* shell,
                                         struct ash_arithmetic_stack* operators,
                                         struct ash_arithmetic_stack* values,
                                         bool* suppressed,
                                         struct ash_arithmetic_error* diagnostic) {
    struct ash_arithmetic_entry entry = operators->entries[--operators->count];
    enum ash_arithmetic_operator operator = entry.operator;
    if (operator == ARITH_OPEN) {
        return "unmatched parenthesis";
    }
    if (operator == ARITH_QUESTION) {
        return "missing conditional colon";
    }
    bool unary = operator >= ARITH_POS;
    if (values->count < (operator == ARITH_CONDITIONAL ? 3u : (unary ? 1u : 2u))) {
        return "missing operand";
    }
    struct ash_arithmetic_entry right_entry = values->entries[--values->count];
    struct ash_arithmetic_entry left_entry = unary ? (struct ash_arithmetic_entry){0} : values->entries[--values->count];
    if (operator == ARITH_CONDITIONAL) {
        values->count--;
    }
    bool assignment = ash_arithmetic_assignment(operator);
    if (assignment && left_entry.name == NULL) {
        return "invalid assignment target";
    }
    long value = 0;
    if (!entry.suppressed) {
        bool logical = operator == ARITH_LOGICAL_OR || operator == ARITH_LOGICAL_AND;
        bool use_left = operator != ARITH_ASSIGN && !unary && (operator != ARITH_CONDITIONAL || entry.value != 0);
        bool use_right = operator != ARITH_CONDITIONAL || entry.value == 0;
        if (logical) {
            use_right = operator == ARITH_LOGICAL_OR ? left_entry.value == 0 : left_entry.value != 0;
        }
        const char* error = use_left ? ash_arithmetic_resolve(shell, &left_entry, diagnostic) : NULL;
        if (error == NULL && use_right) {
            error = ash_arithmetic_resolve(shell, &right_entry, diagnostic);
        }
        if (error != NULL) {
            return error;
        }
        long left = left_entry.value;
        long right = right_entry.value;
        static const enum ash_arithmetic_operator compound_operations[] = {
            ARITH_OR, ARITH_XOR, ARITH_AND, ARITH_SHL, ARITH_SHR, ARITH_ADD, ARITH_SUB, ARITH_MUL, ARITH_DIV, ARITH_MOD,
        };
        enum ash_arithmetic_operator operation = assignment && operator != ARITH_ASSIGN ? compound_operations[operator - ARITH_ASSIGN_OR] : operator;
        switch (operation) {
            case ARITH_ASSIGN:
                value = right;
                break;
            case ARITH_CONDITIONAL:
                value = entry.value != 0 ? left : right;
                break;
            case ARITH_QUESTION:
                return "missing conditional colon";
            case ARITH_LOGICAL_OR:
                value = left != 0 || right != 0;
                break;
            case ARITH_LOGICAL_AND:
                value = left != 0 && right != 0;
                break;
            case ARITH_ADD:
                if (__builtin_add_overflow(left, right, &value))
                    return "integer overflow";
                break;
            case ARITH_SUB:
                if (__builtin_sub_overflow(left, right, &value))
                    return "integer overflow";
                break;
            case ARITH_MUL:
                if (__builtin_mul_overflow(left, right, &value))
                    return "integer overflow";
                break;
            case ARITH_DIV:
            case ARITH_MOD:
                if (right == 0)
                    return "division by zero";
                if (left == LONG_MIN && right == -1)
                    return "integer overflow";
                value = operation == ARITH_DIV ? left / right : left % right;
                break;
            case ARITH_SHL:
            case ARITH_SHR:
                if (right < 0 || (unsigned long)right >= sizeof(long) * CHAR_BIT)
                    return "invalid shift count";
                if (operation == ARITH_SHL) {
                    if (left < 0 || left > (LONG_MAX >> right))
                        return "invalid left shift";
                    value = left << right;
                }
                else {
                    value = left >> right;
                }
                break;
            case ARITH_OR:
                value = left | right;
                break;
            case ARITH_XOR:
                value = left ^ right;
                break;
            case ARITH_AND:
                value = left & right;
                break;
            case ARITH_EQ:
                value = left == right;
                break;
            case ARITH_NE:
                value = left != right;
                break;
            case ARITH_LT:
                value = left < right;
                break;
            case ARITH_LE:
                value = left <= right;
                break;
            case ARITH_GT:
                value = left > right;
                break;
            case ARITH_GE:
                value = left >= right;
                break;
            case ARITH_POS:
                value = right;
                break;
            case ARITH_NEG:
                if (right == LONG_MIN)
                    return "integer overflow";
                value = -right;
                break;
            case ARITH_NOT:
                value = !right;
                break;
            case ARITH_INVERT:
                value = ~right;
                break;
            case ARITH_OPEN:
                return "unmatched parenthesis";
            default:
                return "unsupported operator";
        }
        if (assignment) {
            char text[sizeof(long) * CHAR_BIT + 2u];
            snprintf(text, sizeof(text), "%ld", value);
            if (!ash_var_set_with_export(shell, left_entry.name, left_entry.name_length, text, false)) {
                diagnostic->reported = true;
                return "variable assignment failed";
            }
        }
    }
    if (operator == ARITH_LOGICAL_OR || operator == ARITH_LOGICAL_AND || operator == ARITH_CONDITIONAL) {
        *suppressed = entry.suppressed;
    }
    values->entries[values->count++] = (struct ash_arithmetic_entry){.value = value};
    return NULL;
}

static int ash_arithmetic_digit(unsigned char character) {
    if (character >= '0' && character <= '9')
        return character - '0';
    if (character >= 'a' && character <= 'f')
        return character - 'a' + 10;
    if (character >= 'A' && character <= 'F')
        return character - 'A' + 10;
    return -1;
}

static const char* ash_arithmetic_constant(const char* expression, size_t length, size_t* position, unsigned long limit, bool evaluate, unsigned long* result) {
    size_t offset = *position;
    unsigned int base = expression[offset] == '0' ? 8u : 10u;
    if (length - offset >= 2u && expression[offset] == '0' && (expression[offset + 1u] == 'x' || expression[offset + 1u] == 'X')) {
        base = 16u;
        offset += 2u;
    }
    size_t start = offset;
    unsigned long value = 0u;
    while (offset < length) {
        int digit = ash_arithmetic_digit((unsigned char)expression[offset]);
        if (digit < 0 || (unsigned int)digit >= base) {
            break;
        }
        if (evaluate) {
            if (value > (limit - (unsigned int)digit) / base) {
                return "integer constant out of range";
            }
            value = value * base + (unsigned int)digit;
        }
        offset++;
    }
    if (offset == start) {
        return "invalid integer constant";
    }
    bool unsigned_suffix = false;
    if (offset < length && (expression[offset] == 'u' || expression[offset] == 'U')) {
        unsigned_suffix = true;
        offset++;
    }
    if (offset < length && (expression[offset] == 'l' || expression[offset] == 'L')) {
        char letter = expression[offset++];
        if (offset < length && expression[offset] == letter) {
            offset++;
        }
    }
    if (!unsigned_suffix && offset < length && (expression[offset] == 'u' || expression[offset] == 'U')) {
        offset++;
    }
    if (offset < length && ((expression[offset] >= '0' && expression[offset] <= '9') || (expression[offset] >= 'a' && expression[offset] <= 'z') ||
                            (expression[offset] >= 'A' && expression[offset] <= 'Z') || expression[offset] == '_')) {
        return "invalid integer constant";
    }
    *position = offset;
    *result = value;
    return NULL;
}

static bool ash_arithmetic_binary(const char* expression, size_t length, size_t* position, enum ash_arithmetic_operator* operator) {
    static const struct {
        const char* spelling;
        enum ash_arithmetic_operator operator;
    } spellings[] = {
        {"<<=", ARITH_ASSIGN_SHL}, {">>=", ARITH_ASSIGN_SHR}, {"+=", ARITH_ASSIGN_ADD}, {"-=", ARITH_ASSIGN_SUB}, {"*=", ARITH_ASSIGN_MUL}, {"/=", ARITH_ASSIGN_DIV},
        {"%=", ARITH_ASSIGN_MOD},  {"&=", ARITH_ASSIGN_AND},  {"^=", ARITH_ASSIGN_XOR}, {"|=", ARITH_ASSIGN_OR},  {"||", ARITH_LOGICAL_OR}, {"&&", ARITH_LOGICAL_AND},
        {"==", ARITH_EQ},          {"!=", ARITH_NE},          {"<=", ARITH_LE},         {">=", ARITH_GE},         {"<<", ARITH_SHL},        {">>", ARITH_SHR},
        {"|", ARITH_OR},           {"^", ARITH_XOR},          {"&", ARITH_AND},         {"<", ARITH_LT},          {">", ARITH_GT},          {"+", ARITH_ADD},
        {"-", ARITH_SUB},          {"*", ARITH_MUL},          {"/", ARITH_DIV},         {"%", ARITH_MOD},         {"=", ARITH_ASSIGN},
    };
    for (size_t i = 0u; i < sizeof(spellings) / sizeof(spellings[0]); i++) {
        size_t width = strlen(spellings[i].spelling);
        if (width <= length - *position && memcmp(expression + *position, spellings[i].spelling, width) == 0) {
            *position += width;
            *operator = spellings[i].operator;
            return true;
        }
    }
    return false;
}

static const char* ash_arithmetic_variable(const struct ash_shell* shell, const char* name, size_t length, long* result) {
    const char* input = ash_var_get_len(shell, name, length);
    if (input == NULL) {
        if ((shell->options & ASH_SHELL_OPTION_NOUNSET) != 0u) {
            return "unbound variable";
        }
        *result = 0;
        return NULL;
    }
    if (input[0] == '\0') {
        *result = 0;
        return NULL;
    }
    bool negative = input[0] == '-';
    size_t position = negative || input[0] == '+' ? 1u : 0u;
    size_t input_length = strlen(input);
    if (position == input_length) {
        return "invalid integer constant";
    }
    unsigned long magnitude;
    const char* error = ash_arithmetic_constant(input, input_length, &position, (unsigned long)LONG_MAX + (negative ? 1u : 0u), true, &magnitude);
    if (error != NULL) {
        return error;
    }
    if (position != input_length) {
        return "invalid integer constant";
    }
    *result = negative ? (magnitude == (unsigned long)LONG_MAX + 1u ? LONG_MIN : -(long)magnitude) : (long)magnitude;
    return NULL;
}

bool ash_arithmetic_evaluate(struct ash_shell* shell, const char* expression, size_t length, long* result, struct ash_arithmetic_error* diagnostic) {
    *diagnostic = (struct ash_arithmetic_error){0};
    struct ash_arithmetic_stack operators;
    struct ash_arithmetic_stack values;
    ash_arithmetic_stack_init(&operators);
    ash_arithmetic_stack_init(&values);
    const char* error = NULL;
    bool operand = true;
    bool suppressed = false;
    size_t position = 0u;
    while (position < length && error == NULL) {
        unsigned char character = (unsigned char)expression[position];
        if (strchr(" \t\n\r\v\f", character) != NULL && character != '\0') {
            position++;
            continue;
        }
        if (length - position >= 2u && (character == '+' || character == '-') && expression[position + 1u] == (char)character) {
            error = "unsupported operator";
            break;
        }
        if (operand) {
            enum ash_arithmetic_operator operator;
            if (character == '(' || character == '+' || character == '-' || character == '!' || character == '~') {
                operator = character == '(' ? ARITH_OPEN : character == '+' ? ARITH_POS : character == '-' ? ARITH_NEG : character == '!' ? ARITH_NOT : ARITH_INVERT;
                if (!ash_arithmetic_push(&operators, (struct ash_arithmetic_entry){.operator = operator, .suppressed = suppressed})) {
                    error = "out of memory";
                }
                position++;
                continue;
            }
            struct ash_arithmetic_entry value = {0};
            if (ash_is_name_start(character)) {
                size_t start = position++;
                while (position < length && ash_is_name_char((unsigned char)expression[position])) {
                    position++;
                }
                value.name = expression + start;
                value.name_length = position - start;
            }
            else if (character >= '0' && character <= '9') {
                unsigned long magnitude;
                bool negative = operators.count != 0u && operators.entries[operators.count - 1u].operator == ARITH_NEG;
                error = ash_arithmetic_constant(expression, length, &position, (unsigned long)LONG_MAX + (negative ? 1u : 0u), !suppressed, &magnitude);
                if (error == NULL) {
                    if (magnitude > (unsigned long)LONG_MAX) {
                        operators.count--;
                        value.value = LONG_MIN;
                    }
                    else {
                        value.value = (long)magnitude;
                    }
                }
            }
            else {
                error = "invalid operand";
            }
            if (error == NULL && !ash_arithmetic_push(&values, value)) {
                error = "out of memory";
            }
            operand = false;
            continue;
        }
        if (character == ')') {
            while (operators.count != 0u && operators.entries[operators.count - 1u].operator != ARITH_OPEN && error == NULL) {
                error = ash_arithmetic_reduce(shell, &operators, &values, &suppressed, diagnostic);
            }
            if (error == NULL) {
                if (operators.count == 0u) {
                    error = "unmatched parenthesis";
                }
                else {
                    operators.count--;
                }
            }
            position++;
            continue;
        }
        if (character == ':') {
            while (operators.count != 0u && operators.entries[operators.count - 1u].operator != ARITH_QUESTION && operators.entries[operators.count - 1u].operator != ARITH_OPEN && error == NULL) {
                error = ash_arithmetic_reduce(shell, &operators, &values, &suppressed, diagnostic);
            }
            if (error == NULL) {
                if (operators.count == 0u || operators.entries[operators.count - 1u].operator != ARITH_QUESTION) {
                    error = "unmatched conditional colon";
                }
                else {
                    struct ash_arithmetic_entry* question = &operators.entries[operators.count - 1u];
                    question->operator = ARITH_CONDITIONAL;
                    suppressed = question->suppressed || question->value != 0;
                }
            }
            position++;
            operand = true;
            continue;
        }
        if (character == '?') {
            while (operators.count != 0u && ash_arithmetic_precedence(operators.entries[operators.count - 1u].operator) > ash_arithmetic_precedence(ARITH_QUESTION) && error == NULL) {
                error = ash_arithmetic_reduce(shell, &operators, &values, &suppressed, diagnostic);
            }
            if (error == NULL && !suppressed) {
                error = ash_arithmetic_resolve(shell, &values.entries[values.count - 1u], diagnostic);
            }
            if (error == NULL) {
                long condition = values.entries[values.count - 1u].value;
                if (!ash_arithmetic_push(&operators, (struct ash_arithmetic_entry){.operator = ARITH_QUESTION, .value = condition, .suppressed = suppressed})) {
                    error = "out of memory";
                }
                suppressed = suppressed || condition == 0;
            }
            position++;
            operand = true;
            continue;
        }
        enum ash_arithmetic_operator operator;
        if (!ash_arithmetic_binary(expression, length, &position, &operator)) {
            error = "unsupported operator";
            break;
        }
        while (operators.count != 0u && operators.entries[operators.count - 1u].operator != ARITH_QUESTION &&
               (ash_arithmetic_precedence(operators.entries[operators.count - 1u].operator) > ash_arithmetic_precedence(operator) ||
                (!ash_arithmetic_assignment(operator) && ash_arithmetic_precedence(operators.entries[operators.count - 1u].operator) == ash_arithmetic_precedence(operator))) &&
               error == NULL) {
            error = ash_arithmetic_reduce(shell, &operators, &values, &suppressed, diagnostic);
        }
        bool logical = operator == ARITH_LOGICAL_OR || operator == ARITH_LOGICAL_AND;
        if (error == NULL && logical && !suppressed) {
            error = ash_arithmetic_resolve(shell, &values.entries[values.count - 1u], diagnostic);
        }
        if (error == NULL && !ash_arithmetic_push(&operators, (struct ash_arithmetic_entry){.operator = operator, .suppressed = suppressed})) {
            error = "out of memory";
        }
        if (error == NULL && logical) {
            long left = values.entries[values.count - 1u].value;
            suppressed = suppressed || (operator == ARITH_LOGICAL_OR ? left != 0 : left == 0);
        }
        operand = true;
    }
    if (error == NULL && operand) {
        error = "missing operand";
    }
    while (operators.count != 0u && error == NULL) {
        error = ash_arithmetic_reduce(shell, &operators, &values, &suppressed, diagnostic);
    }
    if (error == NULL) {
        if (values.count != 1u) {
            error = "invalid expression";
        }
        else {
            error = ash_arithmetic_resolve(shell, &values.entries[0], diagnostic);
            if (error == NULL) {
                *result = values.entries[0].value;
            }
        }
    }
    ash_arithmetic_stack_destroy(&values);
    ash_arithmetic_stack_destroy(&operators);
    diagnostic->message = error;
    return error == NULL;
}
