#include <stdlib.h>

#include "find_internal.h"

struct find_expr *find_expr_new(enum find_expr_kind kind) {
    struct find_expr *expr = calloc(1, sizeof(*expr));
    if (!expr)
        return NULL;
    expr->kind = kind;
    return expr;
}

bool find_exec_items_append(struct find_exec_items *items, char *text) {
    if (items->count >= items->cap) {
        int new_cap = items->cap == 0 ? 16 : items->cap * 2;
        char **tmp = realloc(items->v, (size_t)new_cap * sizeof(*items->v));
        if (!tmp)
            return false;
        items->v = tmp;
        items->cap = new_cap;
    }

    items->v[items->count++] = text;
    return true;
}

void find_exec_items_free(struct find_exec_items *items) {
    if (!items)
        return;
    for (int i = 0; i < items->count; i++)
        free(items->v[i]);
    free(items->v);
    items->v = NULL;
    items->count = 0;
    items->cap = 0;
}

void find_expr_free(struct find_expr *expr) {
    if (!expr)
        return;
    find_expr_free(expr->left);
    find_expr_free(expr->right);
    if (expr->regex_compiled)
        regfree(&expr->regex);
    free(expr->exec_argv);
    find_exec_items_free(&expr->exec_items);
    free(expr);
}

struct find_expr *find_make_binary(enum find_expr_kind kind,
                                   struct find_expr *left,
                                   struct find_expr *right) {
    struct find_expr *expr = find_expr_new(kind);
    if (!expr) {
        find_expr_free(left);
        find_expr_free(right);
        return NULL;
    }
    expr->left = left;
    expr->right = right;
    return expr;
}
