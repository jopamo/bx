#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "applets/shell/ash/ast.h"
#include "applets/shell/ash/functions.h"
#include "applets/shell/ash/shell_context.h"

const struct ash_function* ash_function_find(
    const struct ash_shell* shell,
    const char* name
) {
    for (const struct ash_function* function = shell->functions;
         function != NULL;
         function = function->next) {
        if (strcmp(function->name, name) == 0) {
            return function;
        }
    }
    return NULL;
}

bool ash_function_define(
    struct ash_shell* shell,
    const char* name,
    const struct ash_ast* body
) {
    struct ash_ast* body_copy = ash_ast_clone(body);
    if (body_copy == NULL) {
        fprintf(stderr, "%s: out of memory\n", shell->progname);
        return false;
    }

    for (struct ash_function* function = shell->functions;
         function != NULL;
         function = function->next) {
        if (strcmp(function->name, name) == 0) {
            struct ash_ast* old_body = function->body;
            function->body = body_copy;
            ash_ast_destroy(old_body);
            return true;
        }
    }

    struct ash_function* function = calloc(1u, sizeof(*function));
    char* name_copy = strdup(name);
    if (function == NULL || name_copy == NULL) {
        fprintf(stderr, "%s: out of memory\n", shell->progname);
        free(function);
        free(name_copy);
        ash_ast_destroy(body_copy);
        return false;
    }
    function->name = name_copy;
    function->body = body_copy;
    function->next = shell->functions;
    shell->functions = function;
    return true;
}

void ash_function_unset(struct ash_shell* shell, const char* name) {
    struct ash_function** link = &shell->functions;
    while (*link != NULL) {
        struct ash_function* function = *link;
        if (strcmp(function->name, name) == 0) {
            *link = function->next;
            free(function->name);
            ash_ast_destroy(function->body);
            free(function);
            return;
        }
        link = &function->next;
    }
}

void ash_functions_destroy(struct ash_shell* shell) {
    while (shell->functions != NULL) {
        struct ash_function* function = shell->functions;
        shell->functions = function->next;
        free(function->name);
        ash_ast_destroy(function->body);
        free(function);
    }
}
