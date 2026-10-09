#ifndef BX_APPLETS_SHELL_ASH_ARITHMETIC_H
#define BX_APPLETS_SHELL_ASH_ARITHMETIC_H

#include <stddef.h>
#include <stdbool.h>

struct ash_shell;

struct ash_arithmetic_error {
    const char* message;
    const char* name;
    size_t name_length;
    /* The variable owner has already emitted this failure. */
    bool reported;
};

/* Failure leaves result unchanged, not completed writes; names borrow expression. */
bool ash_arithmetic_evaluate(struct ash_shell* shell, const char* expression, size_t length, long* result, struct ash_arithmetic_error* diagnostic);

#endif /* BX_APPLETS_SHELL_ASH_ARITHMETIC_H */
