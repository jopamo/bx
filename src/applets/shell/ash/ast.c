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
