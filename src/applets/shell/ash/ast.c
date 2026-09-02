#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "applets/shell/ash/ast.h"

static int ash_ast_grow(
    void** items,
    size_t* capacity,
    size_t needed,
    size_t item_size
) {
    if (*capacity >= needed) {
        return 0;
    }

    size_t grown = (*capacity == 0u) ? 4u : *capacity;
    while (grown < needed) {
        if (grown > SIZE_MAX / 2u) {
            grown = needed;
            break;
        }
        grown *= 2u;
    }
    if (item_size != 0u && grown > SIZE_MAX / item_size) {
        errno = ENOMEM;
        return -1;
    }

    void* replacement = realloc(*items, grown * item_size);
    if (replacement == NULL) {
        return -1;
    }
    *items = replacement;
    *capacity = grown;
    return 0;
}

struct ash_ast* ash_ast_create(
    enum ash_ast_kind kind,
    struct ash_source_location location
) {
    if (kind < ASH_AST_SIMPLE || kind > ASH_AST_COPROC) {
        errno = EINVAL;
        return NULL;
    }
    struct ash_ast* node = calloc(1u, sizeof(*node));
    if (node == NULL) {
        return NULL;
    }
    node->kind = kind;
    node->location = location;
    return node;
}

void ash_ast_word_init(
    struct ash_ast_word* word,
    struct ash_source_location location
) {
    *word = (struct ash_ast_word){0};
    ash_word_init(&word->syntax, location);
    word->has_syntax = true;
}

void ash_ast_word_destroy(struct ash_ast_word* word) {
    if (word == NULL) {
        return;
    }
    ash_word_destroy(&word->syntax);
    for (size_t i = 0u; i < word->process_substitution_count; i++) {
        ash_ast_destroy(word->process_substitutions[i].command);
    }
    free(word->process_substitutions);
    *word = (struct ash_ast_word){0};
}

int ash_ast_word_clone(
    struct ash_ast_word* destination,
    const struct ash_ast_word* source
) {
    if (!source->has_syntax) {
        *destination = (struct ash_ast_word){0};
        return 0;
    }
    ash_ast_word_init(destination, source->syntax.location);
    if (ash_word_clone(&destination->syntax, &source->syntax) != 0) {
        return -1;
    }
    for (size_t i = 0u; i < source->process_substitution_count; i++) {
        const struct ash_process_substitution* source_substitution =
            &source->process_substitutions[i];
        struct ash_ast* command = ash_ast_clone(
            source_substitution->command
        );
        if (command == NULL ||
            ash_ast_word_take_process_substitution(
                destination,
                source_substitution->part_index,
                source_substitution->direction,
                &command
            ) != 0) {
            ash_ast_destroy(command);
            ash_ast_word_destroy(destination);
            return -1;
        }
    }
    return 0;
}

int ash_ast_word_take_syntax(
    struct ash_ast_word* destination,
    struct ash_word* syntax
) {
    if (destination == NULL || syntax == NULL ||
        destination->has_syntax) {
        errno = EINVAL;
        return -1;
    }
    *destination = (struct ash_ast_word){
        .syntax = *syntax,
        .has_syntax = true,
    };
    *syntax = (struct ash_word){0};
    return 0;
}

int ash_ast_word_take_process_substitution(
    struct ash_ast_word* word,
    size_t part_index,
    enum ash_process_substitution_direction direction,
    struct ash_ast** command
) {
    if (word == NULL || !word->has_syntax ||
        command == NULL || *command == NULL ||
        part_index >= word->syntax.count ||
        word->syntax.parts[part_index].kind !=
            ASH_WORD_PROCESS_SUBSTITUTION ||
        direction < ASH_PROCESS_SUBSTITUTION_READ ||
        direction > ASH_PROCESS_SUBSTITUTION_WRITE) {
        errno = EINVAL;
        return -1;
    }
    if (word->process_substitution_count != 0u &&
        word->process_substitutions[
            word->process_substitution_count - 1u
        ].part_index >= part_index) {
        errno = EINVAL;
        return -1;
    }
    if (ash_ast_grow(
            (void**)&word->process_substitutions,
            &word->process_substitution_capacity,
            word->process_substitution_count + 1u,
            sizeof(*word->process_substitutions)
        ) != 0) {
        return -1;
    }
    word->process_substitutions[word->process_substitution_count++] =
        (struct ash_process_substitution){
            .direction = direction,
            .part_index = part_index,
            .command = *command,
            .location = word->syntax.parts[part_index].location,
        };
    *command = NULL;
    return 0;
}

int ash_arithmetic_expression_copy(
    struct ash_arithmetic_expression* expression,
    struct ash_source_location location,
    const char* text,
    size_t length
) {
    if (expression == NULL || (text == NULL && length != 0u) ||
        length == SIZE_MAX) {
        errno = length == SIZE_MAX ? ENOMEM : EINVAL;
        return -1;
    }
    char* copy = malloc(length + 1u);
    if (copy == NULL) {
        return -1;
    }
    if (length != 0u) {
        memcpy(copy, text, length);
    }
    copy[length] = '\0';
    *expression = (struct ash_arithmetic_expression){
        .text = copy,
        .length = length,
        .location = location,
    };
    return 0;
}

void ash_arithmetic_expression_destroy(
    struct ash_arithmetic_expression* expression
) {
    if (expression == NULL) {
        return;
    }
    free(expression->text);
    *expression = (struct ash_arithmetic_expression){0};
}

static int ash_arithmetic_expression_clone(
    struct ash_arithmetic_expression* destination,
    const struct ash_arithmetic_expression* source
) {
    return ash_arithmetic_expression_copy(
        destination,
        source->location,
        source->text,
        source->length
    );
}

static bool ash_condition_kind_valid(enum ash_condition_kind kind) {
    return kind >= ASH_CONDITION_WORD && kind <= ASH_CONDITION_GROUP;
}

struct ash_condition* ash_condition_create(
    enum ash_condition_kind kind,
    struct ash_source_location location
) {
    if (!ash_condition_kind_valid(kind)) {
        errno = EINVAL;
        return NULL;
    }
    struct ash_condition* condition = calloc(1u, sizeof(*condition));
    if (condition != NULL) {
        condition->kind = kind;
        condition->location = location;
    }
    return condition;
}

void ash_condition_destroy(struct ash_condition* condition) {
    if (condition == NULL) {
        return;
    }
    switch (condition->kind) {
        case ASH_CONDITION_WORD:
            ash_ast_word_destroy(&condition->value.word);
            break;
        case ASH_CONDITION_UNARY:
            ash_ast_word_destroy(&condition->value.unary.operand);
            break;
        case ASH_CONDITION_BINARY:
            ash_ast_word_destroy(&condition->value.binary.left);
            ash_ast_word_destroy(&condition->value.binary.right);
            break;
        case ASH_CONDITION_NOT:
        case ASH_CONDITION_AND:
        case ASH_CONDITION_OR:
        case ASH_CONDITION_GROUP:
            ash_condition_destroy(condition->value.branches.left);
            ash_condition_destroy(condition->value.branches.right);
            break;
    }
    free(condition);
}

struct ash_condition* ash_condition_clone(
    const struct ash_condition* source
) {
    if (source == NULL) {
        return NULL;
    }
    struct ash_condition* copy = ash_condition_create(
        source->kind,
        source->location
    );
    if (copy == NULL) {
        return NULL;
    }
    switch (source->kind) {
        case ASH_CONDITION_WORD:
            if (ash_ast_word_clone(
                    &copy->value.word,
                    &source->value.word
                ) != 0) {
                goto fail;
            }
            break;
        case ASH_CONDITION_UNARY:
            copy->value.unary.operator = source->value.unary.operator;
            if (ash_ast_word_clone(
                    &copy->value.unary.operand,
                    &source->value.unary.operand
                ) != 0) {
                goto fail;
            }
            break;
        case ASH_CONDITION_BINARY:
            copy->value.binary.operator = source->value.binary.operator;
            if (ash_ast_word_clone(
                    &copy->value.binary.left,
                    &source->value.binary.left
                ) != 0 ||
                ash_ast_word_clone(
                    &copy->value.binary.right,
                    &source->value.binary.right
                ) != 0) {
                goto fail;
            }
            break;
        case ASH_CONDITION_NOT:
        case ASH_CONDITION_AND:
        case ASH_CONDITION_OR:
        case ASH_CONDITION_GROUP:
            copy->value.branches.left = ash_condition_clone(
                source->value.branches.left
            );
            copy->value.branches.right = ash_condition_clone(
                source->value.branches.right
            );
            if ((source->value.branches.left != NULL &&
                 copy->value.branches.left == NULL) ||
                (source->value.branches.right != NULL &&
                 copy->value.branches.right == NULL)) {
                goto fail;
            }
            break;
    }
    return copy;

fail:
    ash_condition_destroy(copy);
    return NULL;
}

int ash_condition_take_word(
    struct ash_condition* condition,
    struct ash_word* word
) {
    if (condition == NULL || condition->kind != ASH_CONDITION_WORD ||
        word == NULL || condition->value.word.has_syntax) {
        errno = EINVAL;
        return -1;
    }
    return ash_ast_word_take_syntax(&condition->value.word, word);
}

int ash_condition_take_unary(
    struct ash_condition* condition,
    enum ash_condition_unary_operator operator,
    struct ash_word* operand
) {
    if (condition == NULL || condition->kind != ASH_CONDITION_UNARY ||
        operand == NULL ||
        operator < ASH_CONDITION_STRING_NONEMPTY ||
        operator > ASH_CONDITION_FILE_SOCKET ||
        condition->value.unary.operand.has_syntax) {
        errno = EINVAL;
        return -1;
    }
    condition->value.unary.operator = operator;
    return ash_ast_word_take_syntax(
        &condition->value.unary.operand,
        operand
    );
}

int ash_condition_take_binary(
    struct ash_condition* condition,
    enum ash_condition_binary_operator operator,
    struct ash_word* left,
    struct ash_word* right
) {
    if (condition == NULL || condition->kind != ASH_CONDITION_BINARY ||
        left == NULL || right == NULL ||
        operator < ASH_CONDITION_STRING_EQUAL ||
        operator > ASH_CONDITION_FILE_SAME ||
        condition->value.binary.left.has_syntax ||
        condition->value.binary.right.has_syntax) {
        errno = EINVAL;
        return -1;
    }
    condition->value.binary.operator = operator;
    if (ash_ast_word_take_syntax(
            &condition->value.binary.left,
            left
        ) != 0 ||
        ash_ast_word_take_syntax(
            &condition->value.binary.right,
            right
        ) != 0) {
        return -1;
    }
    return 0;
}

int ash_condition_take_branches(
    struct ash_condition* condition,
    struct ash_condition** left,
    struct ash_condition** right
) {
    bool unary_branch = condition != NULL &&
        (condition->kind == ASH_CONDITION_NOT ||
         condition->kind == ASH_CONDITION_GROUP);
    bool binary_branch = condition != NULL &&
        (condition->kind == ASH_CONDITION_AND ||
         condition->kind == ASH_CONDITION_OR);
    if ((!unary_branch && !binary_branch) ||
        left == NULL || *left == NULL ||
        (unary_branch && right != NULL && *right != NULL) ||
        (binary_branch && (right == NULL || *right == NULL)) ||
        condition->value.branches.left != NULL ||
        condition->value.branches.right != NULL) {
        errno = EINVAL;
        return -1;
    }
    condition->value.branches.left = *left;
    *left = NULL;
    if (binary_branch) {
        condition->value.branches.right = *right;
        *right = NULL;
    }
    return 0;
}

static void ash_here_document_destroy(
    struct ash_here_document* document
) {
    if (document == NULL) {
        return;
    }
    free(document->delimiter);
    free(document->body);
    free(document);
}

static char* ash_here_document_copy_bytes(
    const char* source,
    size_t length
) {
    if (source == NULL || length == SIZE_MAX) {
        errno = source == NULL ? EINVAL : ENOMEM;
        return NULL;
    }
    char* copy = malloc(length + 1u);
    if (copy != NULL) {
        memcpy(copy, source, length + 1u);
    }
    return copy;
}

static struct ash_here_document* ash_here_document_clone(
    const struct ash_here_document* source
) {
    bool state_valid = source != NULL &&
        ((source->state == ASH_HERE_DOCUMENT_PENDING &&
          source->body == NULL &&
          source->body_length == 0u) ||
         (source->state == ASH_HERE_DOCUMENT_COMPLETE &&
          source->body != NULL));
    if (!state_valid || source->delimiter == NULL) {
        errno = EINVAL;
        return NULL;
    }

    struct ash_here_document* document = malloc(sizeof(*document));
    if (document == NULL) {
        return NULL;
    }
    *document = *source;
    document->delimiter = ash_here_document_copy_bytes(
        source->delimiter,
        source->delimiter_length
    );
    document->body = source->body != NULL ?
        ash_here_document_copy_bytes(
            source->body,
            source->body_length
        ) :
        NULL;
    if (document->delimiter == NULL ||
        (source->body != NULL && document->body == NULL)) {
        ash_here_document_destroy(document);
        return NULL;
    }
    return document;
}

static int ash_redirection_clone(
    struct ash_redirection* destination,
    const struct ash_redirection* source
) {
    *destination = (struct ash_redirection){0};
    bool here_document_operator =
        source->operator == ASH_TOKEN_DLESS ||
        source->operator == ASH_TOKEN_DLESS_DASH;
    if (here_document_operator !=
        (source->here_document != NULL)) {
        errno = EINVAL;
        return -1;
    }
    *destination = (struct ash_redirection){
        .operator = source->operator,
        .prefix.kind = source->prefix.kind,
        .prefix.location = source->prefix.location,
        .location = source->location,
    };
    if (source->prefix.text != NULL) {
        destination->prefix.text = strdup(source->prefix.text);
        if (destination->prefix.text == NULL) {
            return -1;
        }
    }
    if (ash_ast_word_clone(
            &destination->target,
            &source->target
        ) != 0) {
        free(destination->prefix.text);
        *destination = (struct ash_redirection){0};
        return -1;
    }
    if (source->here_document != NULL) {
        destination->here_document = ash_here_document_clone(
            source->here_document
        );
        if (destination->here_document == NULL) {
            ash_redirection_destroy(destination);
            return -1;
        }
    }
    return 0;
}

static bool ash_ast_clone_trailing(
    struct ash_ast* destination,
    const struct ash_ast* source
) {
    for (size_t i = 0u; i < source->trailing_redirection_count; i++) {
        struct ash_redirection redirection;
        if (ash_redirection_clone(
                &redirection,
                &source->trailing_redirections[i]
            ) != 0 ||
            ash_ast_take_trailing_redirection(
                destination,
                &redirection
            ) != 0) {
            ash_redirection_destroy(&redirection);
            return false;
        }
    }
    return true;
}

static int ash_ast_simple_take_ast_word(
    struct ash_ast* node,
    struct ash_ast_word* word,
    bool assignment
) {
    if (node == NULL || node->kind != ASH_AST_SIMPLE || word == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (ash_ast_grow(
            (void**)&node->value.simple.items,
            &node->value.simple.capacity,
            node->value.simple.count + 1u,
            sizeof(*node->value.simple.items)
        ) != 0) {
        return -1;
    }
    struct ash_simple_item* item =
        &node->value.simple.items[node->value.simple.count++];
    *item = (struct ash_simple_item){
        .kind = assignment ? ASH_SIMPLE_ASSIGNMENT : ASH_SIMPLE_WORD,
        .location = word->syntax.location,
        .value.word = *word,
    };
    *word = (struct ash_ast_word){0};
    return 0;
}

static int ash_case_clause_take_ast_word(
    struct ash_case_clause* clause,
    struct ash_ast_word* pattern
) {
    if (clause == NULL || pattern == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (ash_ast_grow(
            (void**)&clause->patterns,
            &clause->pattern_capacity,
            clause->pattern_count + 1u,
            sizeof(*clause->patterns)
        ) != 0) {
        return -1;
    }
    clause->patterns[clause->pattern_count++] = *pattern;
    *pattern = (struct ash_ast_word){0};
    return 0;
}

struct ash_ast* ash_ast_clone(const struct ash_ast* source) {
    if (source == NULL) {
        return NULL;
    }
    struct ash_ast* copy = ash_ast_create(source->kind, source->location);
    if (copy == NULL || !ash_ast_clone_trailing(copy, source)) {
        ash_ast_destroy(copy);
        return NULL;
    }

#define CLONE_CHILD(destination, child) \
    do { \
        (destination) = ash_ast_clone(child); \
        if ((child) != NULL && (destination) == NULL) goto fail; \
    } while (0)

    switch (source->kind) {
        case ASH_AST_SIMPLE:
            for (size_t i = 0u; i < source->value.simple.count; i++) {
                const struct ash_simple_item* item =
                    &source->value.simple.items[i];
                if (item->kind == ASH_SIMPLE_REDIRECTION) {
                    struct ash_redirection redirection;
                    if (ash_redirection_clone(
                            &redirection,
                            &item->value.redirection
                        ) != 0 ||
                        ash_ast_simple_take_redirection(
                            copy,
                            &redirection
                        ) != 0) {
                        ash_redirection_destroy(&redirection);
                        goto fail;
                    }
                }
                else {
                    struct ash_ast_word word;
                    if (ash_ast_word_clone(
                            &word,
                            &item->value.word
                        ) != 0 ||
                        ash_ast_simple_take_ast_word(
                            copy,
                            &word,
                            item->kind == ASH_SIMPLE_ASSIGNMENT
                        ) != 0) {
                        ash_ast_word_destroy(&word);
                        goto fail;
                    }
                }
            }
            break;
        case ASH_AST_LIST:
            for (size_t i = 0u; i < source->value.list.count; i++) {
                struct ash_ast* child = ash_ast_clone(
                    source->value.list.entries[i].command
                );
                if (child == NULL || ash_ast_list_take(copy, &child) != 0) {
                    ash_ast_destroy(child);
                    goto fail;
                }
                copy->value.list.entries[i].asynchronous =
                    source->value.list.entries[i].asynchronous;
            }
            break;
        case ASH_AST_AND_OR:
            for (size_t i = 0u; i < source->value.and_or.count; i++) {
                struct ash_ast* child = ash_ast_clone(
                    source->value.and_or.pipelines[i]
                );
                enum ash_and_or_operator operator_before = i == 0u ?
                    ASH_AND_IF : source->value.and_or.operators[i - 1u];
                if (child == NULL ||
                    ash_ast_and_or_take(copy, &child, operator_before) != 0) {
                    ash_ast_destroy(child);
                    goto fail;
                }
            }
            break;
        case ASH_AST_PIPELINE:
            copy->value.pipeline.negated = source->value.pipeline.negated;
            for (size_t i = 0u; i < source->value.pipeline.count; i++) {
                struct ash_ast* child = ash_ast_clone(
                    source->value.pipeline.commands[i]
                );
                enum ash_pipe_operator operator_before = i == 0u ?
                    ASH_PIPE_STDOUT :
                    source->value.pipeline.operators[i - 1u];
                if (child == NULL ||
                    ash_ast_pipeline_take(copy, &child, operator_before) != 0) {
                    ash_ast_destroy(child);
                    goto fail;
                }
            }
            break;
        case ASH_AST_SUBSHELL:
        case ASH_AST_BRACE_GROUP:
            CLONE_CHILD(copy->value.group.body, source->value.group.body);
            break;
        case ASH_AST_IF:
            CLONE_CHILD(
                copy->value.conditional.condition,
                source->value.conditional.condition
            );
            CLONE_CHILD(
                copy->value.conditional.then_branch,
                source->value.conditional.then_branch
            );
            CLONE_CHILD(
                copy->value.conditional.else_branch,
                source->value.conditional.else_branch
            );
            break;
        case ASH_AST_WHILE:
        case ASH_AST_UNTIL:
            CLONE_CHILD(copy->value.loop.condition, source->value.loop.condition);
            CLONE_CHILD(copy->value.loop.body, source->value.loop.body);
            break;
        case ASH_AST_FOR:
        case ASH_AST_SELECT:
            if (source->value.for_loop.name != NULL) {
                copy->value.for_loop.name = strdup(
                    source->value.for_loop.name
                );
                if (copy->value.for_loop.name == NULL) {
                    goto fail;
                }
            }
            copy->value.for_loop.explicit_words =
                source->value.for_loop.explicit_words;
            for (size_t i = 0u; i < source->value.for_loop.word_count; i++) {
                struct ash_ast_word word;
                if (ash_ast_word_clone(
                        &word,
                        &source->value.for_loop.words[i]
                    ) != 0) {
                    goto fail;
                }
                if (copy->value.for_loop.word_count ==
                    copy->value.for_loop.word_capacity) {
                    size_t capacity =
                        copy->value.for_loop.word_capacity == 0u ?
                            4u : copy->value.for_loop.word_capacity * 2u;
                    struct ash_ast_word* words = realloc(
                        copy->value.for_loop.words,
                        capacity * sizeof(*words)
                    );
                    if (words == NULL) {
                        ash_ast_word_destroy(&word);
                        goto fail;
                    }
                    copy->value.for_loop.words = words;
                    copy->value.for_loop.word_capacity = capacity;
                }
                copy->value.for_loop.words[
                    copy->value.for_loop.word_count++
                ] = word;
            }
            CLONE_CHILD(
                copy->value.for_loop.body,
                source->value.for_loop.body
            );
            break;
        case ASH_AST_CASE:
            if (ash_ast_word_clone(
                    &copy->value.case_command.subject,
                    &source->value.case_command.subject
                ) != 0) {
                goto fail;
            }
            for (size_t i = 0u;
                 i < source->value.case_command.clause_count;
                 i++) {
                const struct ash_case_clause* source_clause =
                    &source->value.case_command.clauses[i];
                struct ash_case_clause clause = {
                    .terminator = source_clause->terminator,
                };
                for (size_t j = 0u;
                     j < source_clause->pattern_count;
                     j++) {
                    struct ash_ast_word pattern;
                    if (ash_ast_word_clone(
                            &pattern,
                            &source_clause->patterns[j]
                        ) != 0 ||
                        ash_case_clause_take_ast_word(
                            &clause,
                            &pattern
                        ) != 0) {
                        ash_ast_word_destroy(&pattern);
                        ash_case_clause_destroy(&clause);
                        goto fail;
                    }
                }
                clause.body = ash_ast_clone(source_clause->body);
                if (clause.body == NULL ||
                    ash_ast_case_take_clause(copy, &clause) != 0) {
                    ash_case_clause_destroy(&clause);
                    goto fail;
                }
            }
            break;
        case ASH_AST_FUNCTION:
            if (source->value.function.name != NULL) {
                copy->value.function.name = strdup(
                    source->value.function.name
                );
                if (copy->value.function.name == NULL) {
                    goto fail;
                }
            }
            CLONE_CHILD(
                copy->value.function.body,
                source->value.function.body
            );
            copy->value.function.syntax = source->value.function.syntax;
            break;
        case ASH_AST_ARITHMETIC_COMMAND:
            if (ash_arithmetic_expression_clone(
                    &copy->value.arithmetic_command.expression,
                    &source->value.arithmetic_command.expression
                ) != 0) {
                goto fail;
            }
            break;
        case ASH_AST_CONDITIONAL_COMMAND:
            copy->value.conditional_command.root = ash_condition_clone(
                source->value.conditional_command.root
            );
            if (source->value.conditional_command.root != NULL &&
                copy->value.conditional_command.root == NULL) {
                goto fail;
            }
            break;
        case ASH_AST_C_STYLE_FOR:
            if (ash_arithmetic_expression_clone(
                    &copy->value.c_style_for.initializer,
                    &source->value.c_style_for.initializer
                ) != 0 ||
                ash_arithmetic_expression_clone(
                    &copy->value.c_style_for.condition,
                    &source->value.c_style_for.condition
                ) != 0 ||
                ash_arithmetic_expression_clone(
                    &copy->value.c_style_for.updater,
                    &source->value.c_style_for.updater
                ) != 0) {
                goto fail;
            }
            CLONE_CHILD(
                copy->value.c_style_for.body,
                source->value.c_style_for.body
            );
            break;
        case ASH_AST_TIME:
            copy->value.time_command.posix_format =
                source->value.time_command.posix_format;
            CLONE_CHILD(
                copy->value.time_command.pipeline,
                source->value.time_command.pipeline
            );
            break;
        case ASH_AST_COPROC:
            if (source->value.coproc.name != NULL) {
                copy->value.coproc.name = strdup(
                    source->value.coproc.name
                );
                if (copy->value.coproc.name == NULL) {
                    goto fail;
                }
            }
            CLONE_CHILD(
                copy->value.coproc.command,
                source->value.coproc.command
            );
            break;
    }
#undef CLONE_CHILD
    return copy;

fail:
#undef CLONE_CHILD
    ash_ast_destroy(copy);
    return NULL;
}

void ash_redirection_destroy(struct ash_redirection* redirection) {
    if (redirection == NULL) {
        return;
    }
    free(redirection->prefix.text);
    ash_ast_word_destroy(&redirection->target);
    ash_here_document_destroy(redirection->here_document);
    *redirection = (struct ash_redirection){0};
}

void ash_case_clause_destroy(struct ash_case_clause* clause) {
    if (clause == NULL) {
        return;
    }
    for (size_t i = 0u; i < clause->pattern_count; i++) {
        ash_ast_word_destroy(&clause->patterns[i]);
    }
    free(clause->patterns);
    ash_ast_destroy(clause->body);
    *clause = (struct ash_case_clause){0};
}

static void ash_ast_destroy_redirections(
    struct ash_redirection* redirections,
    size_t count
) {
    for (size_t i = 0u; i < count; i++) {
        ash_redirection_destroy(&redirections[i]);
    }
    free(redirections);
}

void ash_ast_destroy(struct ash_ast* node) {
    if (node == NULL) {
        return;
    }

    switch (node->kind) {
        case ASH_AST_SIMPLE:
            for (size_t i = 0u; i < node->value.simple.count; i++) {
                struct ash_simple_item* item = &node->value.simple.items[i];
                if (item->kind == ASH_SIMPLE_REDIRECTION) {
                    ash_redirection_destroy(&item->value.redirection);
                }
                else {
                    ash_ast_word_destroy(&item->value.word);
                }
            }
            free(node->value.simple.items);
            break;
        case ASH_AST_LIST:
            for (size_t i = 0u; i < node->value.list.count; i++) {
                ash_ast_destroy(node->value.list.entries[i].command);
            }
            free(node->value.list.entries);
            break;
        case ASH_AST_AND_OR:
            for (size_t i = 0u; i < node->value.and_or.count; i++) {
                ash_ast_destroy(node->value.and_or.pipelines[i]);
            }
            free(node->value.and_or.pipelines);
            free(node->value.and_or.operators);
            break;
        case ASH_AST_PIPELINE:
            for (size_t i = 0u; i < node->value.pipeline.count; i++) {
                ash_ast_destroy(node->value.pipeline.commands[i]);
            }
            free(node->value.pipeline.commands);
            free(node->value.pipeline.operators);
            break;
        case ASH_AST_SUBSHELL:
        case ASH_AST_BRACE_GROUP:
            ash_ast_destroy(node->value.group.body);
            break;
        case ASH_AST_IF:
            ash_ast_destroy(node->value.conditional.condition);
            ash_ast_destroy(node->value.conditional.then_branch);
            ash_ast_destroy(node->value.conditional.else_branch);
            break;
        case ASH_AST_WHILE:
        case ASH_AST_UNTIL:
            ash_ast_destroy(node->value.loop.condition);
            ash_ast_destroy(node->value.loop.body);
            break;
        case ASH_AST_FOR:
        case ASH_AST_SELECT:
            free(node->value.for_loop.name);
            for (size_t i = 0u; i < node->value.for_loop.word_count; i++) {
                ash_ast_word_destroy(&node->value.for_loop.words[i]);
            }
            free(node->value.for_loop.words);
            ash_ast_destroy(node->value.for_loop.body);
            break;
        case ASH_AST_CASE:
            ash_ast_word_destroy(&node->value.case_command.subject);
            for (size_t i = 0u;
                 i < node->value.case_command.clause_count;
                 i++) {
                ash_case_clause_destroy(
                    &node->value.case_command.clauses[i]
                );
            }
            free(node->value.case_command.clauses);
            break;
        case ASH_AST_FUNCTION:
            free(node->value.function.name);
            ash_ast_destroy(node->value.function.body);
            break;
        case ASH_AST_ARITHMETIC_COMMAND:
            ash_arithmetic_expression_destroy(
                &node->value.arithmetic_command.expression
            );
            break;
        case ASH_AST_CONDITIONAL_COMMAND:
            ash_condition_destroy(node->value.conditional_command.root);
            break;
        case ASH_AST_C_STYLE_FOR:
            ash_arithmetic_expression_destroy(
                &node->value.c_style_for.initializer
            );
            ash_arithmetic_expression_destroy(
                &node->value.c_style_for.condition
            );
            ash_arithmetic_expression_destroy(
                &node->value.c_style_for.updater
            );
            ash_ast_destroy(node->value.c_style_for.body);
            break;
        case ASH_AST_TIME:
            ash_ast_destroy(node->value.time_command.pipeline);
            break;
        case ASH_AST_COPROC:
            free(node->value.coproc.name);
            ash_ast_destroy(node->value.coproc.command);
            break;
    }

    ash_ast_destroy_redirections(
        node->trailing_redirections,
        node->trailing_redirection_count
    );
    free(node);
}

int ash_ast_simple_take_word(
    struct ash_ast* node,
    struct ash_word* word,
    bool assignment
) {
    if (node == NULL || node->kind != ASH_AST_SIMPLE || word == NULL) {
        errno = EINVAL;
        return -1;
    }
    struct ash_ast_word ast_word = {
        .syntax = *word,
        .has_syntax = true,
    };
    if (ash_ast_simple_take_ast_word(
            node,
            &ast_word,
            assignment
        ) != 0) {
        return -1;
    }
    *word = (struct ash_word){0};
    return 0;
}

static int ash_redirection_array_add(
    struct ash_redirection** items,
    size_t* count,
    size_t* capacity,
    struct ash_redirection* redirection
) {
    if (ash_ast_grow(
            (void**)items,
            capacity,
            *count + 1u,
            sizeof(**items)
        ) != 0) {
        return -1;
    }
    (*items)[(*count)++] = *redirection;
    *redirection = (struct ash_redirection){0};
    return 0;
}

int ash_ast_simple_take_redirection(
    struct ash_ast* node,
    struct ash_redirection* redirection
) {
    if (node == NULL || node->kind != ASH_AST_SIMPLE ||
        redirection == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (ash_ast_grow(
            (void**)&node->value.simple.items,
            &node->value.simple.capacity,
            node->value.simple.count + 1u,
            sizeof(*node->value.simple.items)
        ) != 0) {
        return -1;
    }

    struct ash_simple_item* item =
        &node->value.simple.items[node->value.simple.count++];
    *item = (struct ash_simple_item){
        .kind = ASH_SIMPLE_REDIRECTION,
        .location = redirection->location,
        .value.redirection = *redirection,
    };
    *redirection = (struct ash_redirection){0};
    return 0;
}

int ash_ast_take_trailing_redirection(
    struct ash_ast* node,
    struct ash_redirection* redirection
) {
    if (node == NULL || redirection == NULL) {
        errno = EINVAL;
        return -1;
    }
    return ash_redirection_array_add(
        &node->trailing_redirections,
        &node->trailing_redirection_count,
        &node->trailing_redirection_capacity,
        redirection
    );
}

int ash_ast_list_take(struct ash_ast* node, struct ash_ast** command) {
    if (node == NULL || node->kind != ASH_AST_LIST ||
        command == NULL || *command == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (ash_ast_grow(
            (void**)&node->value.list.entries,
            &node->value.list.capacity,
            node->value.list.count + 1u,
            sizeof(*node->value.list.entries)
        ) != 0) {
        return -1;
    }
    node->value.list.entries[node->value.list.count++] =
        (struct ash_list_entry){.command = *command};
    *command = NULL;
    return 0;
}

int ash_ast_and_or_take(
    struct ash_ast* node,
    struct ash_ast** pipeline,
    enum ash_and_or_operator operator_before
) {
    if (node == NULL || node->kind != ASH_AST_AND_OR ||
        pipeline == NULL || *pipeline == NULL) {
        errno = EINVAL;
        return -1;
    }
    size_t needed = node->value.and_or.count + 1u;
    if (ash_ast_grow(
            (void**)&node->value.and_or.pipelines,
            &node->value.and_or.capacity,
            needed,
            sizeof(*node->value.and_or.pipelines)
        ) != 0) {
        return -1;
    }
    if (needed > 1u) {
        size_t operator_capacity = node->value.and_or.capacity - 1u;
        enum ash_and_or_operator* operators = realloc(
            node->value.and_or.operators,
            operator_capacity * sizeof(*operators)
        );
        if (operators == NULL) {
            return -1;
        }
        node->value.and_or.operators = operators;
        node->value.and_or.operators[needed - 2u] = operator_before;
    }
    node->value.and_or.pipelines[node->value.and_or.count++] = *pipeline;
    *pipeline = NULL;
    return 0;
}

int ash_ast_pipeline_take(
    struct ash_ast* node,
    struct ash_ast** command,
    enum ash_pipe_operator operator_before
) {
    if (node == NULL || node->kind != ASH_AST_PIPELINE ||
        command == NULL || *command == NULL) {
        errno = EINVAL;
        return -1;
    }
    size_t needed = node->value.pipeline.count + 1u;
    if (ash_ast_grow(
            (void**)&node->value.pipeline.commands,
            &node->value.pipeline.capacity,
            needed,
            sizeof(*node->value.pipeline.commands)
        ) != 0) {
        return -1;
    }
    if (needed > 1u) {
        size_t operator_capacity = node->value.pipeline.capacity - 1u;
        enum ash_pipe_operator* operators = realloc(
            node->value.pipeline.operators,
            operator_capacity * sizeof(*operators)
        );
        if (operators == NULL) {
            return -1;
        }
        node->value.pipeline.operators = operators;
        node->value.pipeline.operators[needed - 2u] = operator_before;
    }
    node->value.pipeline.commands[node->value.pipeline.count++] = *command;
    *command = NULL;
    return 0;
}

static bool ash_ast_is_word_loop(const struct ash_ast* node) {
    return node != NULL &&
        (node->kind == ASH_AST_FOR || node->kind == ASH_AST_SELECT);
}

int ash_ast_for_take_name(struct ash_ast* node, char** name) {
    if (!ash_ast_is_word_loop(node) || name == NULL || *name == NULL ||
        node->value.for_loop.name != NULL) {
        errno = EINVAL;
        return -1;
    }
    node->value.for_loop.name = *name;
    *name = NULL;
    return 0;
}

int ash_ast_for_take_word(struct ash_ast* node, struct ash_word* word) {
    if (!ash_ast_is_word_loop(node) || word == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (ash_ast_grow(
            (void**)&node->value.for_loop.words,
            &node->value.for_loop.word_capacity,
            node->value.for_loop.word_count + 1u,
            sizeof(*node->value.for_loop.words)
        ) != 0) {
        return -1;
    }
    struct ash_ast_word* destination =
        &node->value.for_loop.words[node->value.for_loop.word_count];
    *destination = (struct ash_ast_word){0};
    if (ash_ast_word_take_syntax(destination, word) != 0) {
        return -1;
    }
    node->value.for_loop.word_count++;
    return 0;
}

int ash_ast_for_take_body(
    struct ash_ast* node,
    struct ash_ast** body,
    bool explicit_words
) {
    if (!ash_ast_is_word_loop(node) || body == NULL || *body == NULL ||
        node->value.for_loop.name == NULL ||
        node->value.for_loop.body != NULL) {
        errno = EINVAL;
        return -1;
    }
    node->value.for_loop.body = *body;
    node->value.for_loop.explicit_words = explicit_words;
    *body = NULL;
    return 0;
}

int ash_ast_case_take_subject(
    struct ash_ast* node,
    struct ash_word* subject
) {
    if (node == NULL || node->kind != ASH_AST_CASE || subject == NULL ||
        node->value.case_command.subject.has_syntax) {
        errno = EINVAL;
        return -1;
    }
    return ash_ast_word_take_syntax(
        &node->value.case_command.subject,
        subject
    );
}

int ash_case_clause_take_pattern(
    struct ash_case_clause* clause,
    struct ash_word* pattern
) {
    if (clause == NULL || pattern == NULL) {
        errno = EINVAL;
        return -1;
    }
    struct ash_ast_word ast_word = {
        .syntax = *pattern,
        .has_syntax = true,
    };
    if (ash_case_clause_take_ast_word(clause, &ast_word) != 0) {
        return -1;
    }
    *pattern = (struct ash_word){0};
    return 0;
}

int ash_ast_case_take_clause(
    struct ash_ast* node,
    struct ash_case_clause* clause
) {
    if (node == NULL || node->kind != ASH_AST_CASE || clause == NULL ||
        clause->pattern_count == 0u || clause->body == NULL ||
        clause->terminator < ASH_CASE_TERMINATE ||
        clause->terminator > ASH_CASE_TEST_NEXT) {
        errno = EINVAL;
        return -1;
    }
    if (ash_ast_grow(
            (void**)&node->value.case_command.clauses,
            &node->value.case_command.clause_capacity,
            node->value.case_command.clause_count + 1u,
            sizeof(*node->value.case_command.clauses)
        ) != 0) {
        return -1;
    }
    node->value.case_command.clauses[
        node->value.case_command.clause_count++
    ] = *clause;
    *clause = (struct ash_case_clause){0};
    return 0;
}

int ash_ast_take_condition(
    struct ash_ast* node,
    struct ash_condition** condition
) {
    if (node == NULL || node->kind != ASH_AST_CONDITIONAL_COMMAND ||
        condition == NULL || *condition == NULL ||
        node->value.conditional_command.root != NULL) {
        errno = EINVAL;
        return -1;
    }
    node->value.conditional_command.root = *condition;
    *condition = NULL;
    return 0;
}

int ash_ast_arithmetic_command_copy_expression(
    struct ash_ast* node,
    struct ash_source_location location,
    const char* text,
    size_t length
) {
    if (node == NULL || node->kind != ASH_AST_ARITHMETIC_COMMAND ||
        node->value.arithmetic_command.expression.text != NULL) {
        errno = EINVAL;
        return -1;
    }
    return ash_arithmetic_expression_copy(
        &node->value.arithmetic_command.expression,
        location,
        text,
        length
    );
}

int ash_ast_c_style_for_copy_expressions_take_body(
    struct ash_ast* node,
    struct ash_source_location location,
    const char* initializer,
    size_t initializer_length,
    const char* condition,
    size_t condition_length,
    const char* updater,
    size_t updater_length,
    struct ash_ast** body
) {
    if (node == NULL || node->kind != ASH_AST_C_STYLE_FOR ||
        body == NULL || *body == NULL ||
        node->value.c_style_for.body != NULL ||
        node->value.c_style_for.initializer.text != NULL ||
        node->value.c_style_for.condition.text != NULL ||
        node->value.c_style_for.updater.text != NULL) {
        errno = EINVAL;
        return -1;
    }
    struct ash_arithmetic_expression initializer_copy = {0};
    struct ash_arithmetic_expression condition_copy = {0};
    struct ash_arithmetic_expression updater_copy = {0};
    if (ash_arithmetic_expression_copy(
            &initializer_copy,
            location,
            initializer,
            initializer_length
        ) != 0 ||
        ash_arithmetic_expression_copy(
            &condition_copy,
            location,
            condition,
            condition_length
        ) != 0 ||
        ash_arithmetic_expression_copy(
            &updater_copy,
            location,
            updater,
            updater_length
        ) != 0) {
        ash_arithmetic_expression_destroy(&initializer_copy);
        ash_arithmetic_expression_destroy(&condition_copy);
        ash_arithmetic_expression_destroy(&updater_copy);
        return -1;
    }
    node->value.c_style_for.initializer = initializer_copy;
    node->value.c_style_for.condition = condition_copy;
    node->value.c_style_for.updater = updater_copy;
    node->value.c_style_for.body = *body;
    *body = NULL;
    return 0;
}

int ash_ast_take_time_pipeline(
    struct ash_ast* node,
    struct ash_ast** pipeline,
    bool posix_format
) {
    if (node == NULL || node->kind != ASH_AST_TIME ||
        pipeline == NULL || *pipeline == NULL ||
        (*pipeline)->kind != ASH_AST_PIPELINE ||
        node->value.time_command.pipeline != NULL) {
        errno = EINVAL;
        return -1;
    }
    node->value.time_command.pipeline = *pipeline;
    node->value.time_command.posix_format = posix_format;
    *pipeline = NULL;
    return 0;
}

int ash_ast_take_coproc_command(
    struct ash_ast* node,
    const char* name,
    struct ash_ast** command
) {
    if (node == NULL || node->kind != ASH_AST_COPROC ||
        command == NULL || *command == NULL ||
        node->value.coproc.command != NULL ||
        node->value.coproc.name != NULL) {
        errno = EINVAL;
        return -1;
    }
    char* name_copy = NULL;
    if (name != NULL) {
        name_copy = strdup(name);
        if (name_copy == NULL) {
            return -1;
        }
    }
    node->value.coproc.name = name_copy;
    node->value.coproc.command = *command;
    *command = NULL;
    return 0;
}

int ash_ast_function_take(
    struct ash_ast* node,
    char** name,
    enum ash_function_syntax syntax,
    struct ash_ast** body
) {
    if (node == NULL || node->kind != ASH_AST_FUNCTION ||
        name == NULL || *name == NULL ||
        body == NULL || *body == NULL ||
        syntax < ASH_FUNCTION_POSIX ||
        syntax > ASH_FUNCTION_KEYWORD_WITH_PARENS ||
        node->value.function.name != NULL ||
        node->value.function.body != NULL) {
        errno = EINVAL;
        return -1;
    }
    node->value.function.name = *name;
    node->value.function.syntax = syntax;
    node->value.function.body = *body;
    *name = NULL;
    *body = NULL;
    return 0;
}

bool ash_word_is_unquoted_literal(const struct ash_word* word, const char* text) {
    size_t text_length = strlen(text);
    size_t offset = 0u;
    for (size_t i = 0u; i < word->count; i++) {
        const struct ash_word_part* part = &word->parts[i];
        if (part->kind != ASH_WORD_TEXT ||
            ash_word_part_is_quoted(part) ||
            part->length > text_length - offset ||
            memcmp(part->text, text + offset, part->length) != 0) {
            return false;
        }
        offset += part->length;
    }
    return offset == text_length;
}

bool ash_word_is_assignment(const struct ash_word* word) {
    if (word->count == 0u) {
        return false;
    }

    bool first = true;
    bool saw_name = false;
    for (size_t i = 0u; i < word->count; i++) {
        const struct ash_word_part* part = &word->parts[i];
        if (part->kind != ASH_WORD_TEXT ||
            ash_word_part_is_quoted(part)) {
            return false;
        }
        for (size_t j = 0u; j < part->length; j++) {
            unsigned char ch = (unsigned char)part->text[j];
            if (ch == '=') {
                return saw_name;
            }
            bool valid = first ?
                ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_') :
                ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                 (ch >= '0' && ch <= '9') || ch == '_');
            if (!valid) {
                return false;
            }
            first = false;
            saw_name = true;
        }
    }
    return false;
}
