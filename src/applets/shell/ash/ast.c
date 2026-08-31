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
    struct ash_ast* node = calloc(1u, sizeof(*node));
    if (node == NULL) {
        return NULL;
    }
    node->kind = kind;
    node->location = location;
    return node;
}

static int ash_redirection_clone(
    struct ash_redirection* destination,
    const struct ash_redirection* source
) {
    *destination = (struct ash_redirection){
        .operator = source->operator,
        .location = source->location,
    };
    if (source->io_number != NULL) {
        destination->io_number = strdup(source->io_number);
        if (destination->io_number == NULL) {
            return -1;
        }
    }
    if (ash_word_clone(&destination->target, &source->target) != 0) {
        free(destination->io_number);
        *destination = (struct ash_redirection){0};
        return -1;
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
            ash_ast_add_trailing_redirection(
                destination,
                &redirection
            ) != 0) {
            ash_redirection_destroy(&redirection);
            return false;
        }
    }
    return true;
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
                        ash_ast_simple_add_redirection(
                            copy,
                            &redirection
                        ) != 0) {
                        ash_redirection_destroy(&redirection);
                        goto fail;
                    }
                }
                else {
                    struct ash_word word;
                    if (ash_word_clone(&word, &item->value.word) != 0 ||
                        ash_ast_simple_add_word(
                            copy,
                            &word,
                            item->kind == ASH_SIMPLE_ASSIGNMENT
                        ) != 0) {
                        ash_word_destroy(&word);
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
                if (child == NULL || ash_ast_list_add(copy, child) != 0) {
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
                    ash_ast_and_or_add(copy, child, operator_before) != 0) {
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
                    ash_ast_pipeline_add(copy, child, operator_before) != 0) {
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
            copy->value.for_loop.name = strdup(source->value.for_loop.name);
            if (copy->value.for_loop.name == NULL) {
                goto fail;
            }
            copy->value.for_loop.explicit_words =
                source->value.for_loop.explicit_words;
            for (size_t i = 0u; i < source->value.for_loop.word_count; i++) {
                struct ash_word word;
                if (ash_word_clone(
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
                    struct ash_word* words = realloc(
                        copy->value.for_loop.words,
                        capacity * sizeof(*words)
                    );
                    if (words == NULL) {
                        ash_word_destroy(&word);
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
            if (ash_word_clone(
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
                struct ash_case_clause clause = {0};
                for (size_t j = 0u;
                     j < source_clause->pattern_count;
                     j++) {
                    struct ash_word pattern;
                    if (ash_word_clone(
                            &pattern,
                            &source_clause->patterns[j]
                        ) != 0 ||
                        ash_case_clause_add_pattern(
                            &clause,
                            &pattern
                        ) != 0) {
                        ash_word_destroy(&pattern);
                        ash_case_clause_destroy(&clause);
                        goto fail;
                    }
                }
                clause.body = ash_ast_clone(source_clause->body);
                if (clause.body == NULL ||
                    ash_ast_case_add_clause(copy, &clause) != 0) {
                    ash_case_clause_destroy(&clause);
                    goto fail;
                }
            }
            break;
        case ASH_AST_FUNCTION:
            copy->value.function.name = strdup(source->value.function.name);
            if (copy->value.function.name == NULL) {
                goto fail;
            }
            CLONE_CHILD(
                copy->value.function.body,
                source->value.function.body
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
    free(redirection->io_number);
    ash_word_destroy(&redirection->target);
    *redirection = (struct ash_redirection){0};
}

void ash_case_clause_destroy(struct ash_case_clause* clause) {
    if (clause == NULL) {
        return;
    }
    for (size_t i = 0u; i < clause->pattern_count; i++) {
        ash_word_destroy(&clause->patterns[i]);
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
                    ash_word_destroy(&item->value.word);
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
            free(node->value.for_loop.name);
            for (size_t i = 0u; i < node->value.for_loop.word_count; i++) {
                ash_word_destroy(&node->value.for_loop.words[i]);
            }
            free(node->value.for_loop.words);
            ash_ast_destroy(node->value.for_loop.body);
            break;
        case ASH_AST_CASE:
            ash_word_destroy(&node->value.case_command.subject);
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
    }

    ash_ast_destroy_redirections(
        node->trailing_redirections,
        node->trailing_redirection_count
    );
    free(node);
}

int ash_ast_simple_add_word(
    struct ash_ast* node,
    struct ash_word* word,
    bool assignment
) {
    if (node == NULL || node->kind != ASH_AST_SIMPLE) {
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
        .location = word->location,
        .value.word = *word,
    };
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

int ash_ast_simple_add_redirection(
    struct ash_ast* node,
    struct ash_redirection* redirection
) {
    if (node == NULL || node->kind != ASH_AST_SIMPLE) {
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

int ash_ast_add_trailing_redirection(
    struct ash_ast* node,
    struct ash_redirection* redirection
) {
    return ash_redirection_array_add(
        &node->trailing_redirections,
        &node->trailing_redirection_count,
        &node->trailing_redirection_capacity,
        redirection
    );
}

int ash_ast_list_add(struct ash_ast* node, struct ash_ast* command) {
    if (node == NULL || node->kind != ASH_AST_LIST) {
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
        (struct ash_list_entry){.command = command};
    return 0;
}

int ash_ast_and_or_add(
    struct ash_ast* node,
    struct ash_ast* pipeline,
    enum ash_and_or_operator operator_before
) {
    if (node == NULL || node->kind != ASH_AST_AND_OR) {
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
    node->value.and_or.pipelines[node->value.and_or.count++] = pipeline;
    return 0;
}

int ash_ast_pipeline_add(
    struct ash_ast* node,
    struct ash_ast* command,
    enum ash_pipe_operator operator_before
) {
    if (node == NULL || node->kind != ASH_AST_PIPELINE) {
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
    node->value.pipeline.commands[node->value.pipeline.count++] = command;
    return 0;
}

int ash_case_clause_add_pattern(
    struct ash_case_clause* clause,
    struct ash_word* pattern
) {
    if (ash_ast_grow(
            (void**)&clause->patterns,
            &clause->pattern_capacity,
            clause->pattern_count + 1u,
            sizeof(*clause->patterns)
        ) != 0) {
        return -1;
    }
    clause->patterns[clause->pattern_count++] = *pattern;
    *pattern = (struct ash_word){0};
    return 0;
}

int ash_ast_case_add_clause(
    struct ash_ast* node,
    struct ash_case_clause* clause
) {
    if (node == NULL || node->kind != ASH_AST_CASE) {
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

bool ash_word_is_unquoted_literal(const struct ash_word* word, const char* text) {
    size_t text_length = strlen(text);
    size_t offset = 0u;
    for (size_t i = 0u; i < word->count; i++) {
        const struct ash_word_part* part = &word->parts[i];
        if (part->kind != ASH_WORD_TEXT || part->quote != ASH_QUOTE_NONE ||
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
        if (part->kind != ASH_WORD_TEXT || part->quote != ASH_QUOTE_NONE) {
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
