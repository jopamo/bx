#ifndef BX_APPLETS_SHELL_ASH_LOCALE_STATE_H
#define BX_APPLETS_SHELL_ASH_LOCALE_STATE_H

#include <locale.h>
#include <stdbool.h>
#include <stddef.h>

struct ash_shell;

enum ash_locale_category { ASH_LOCALE_CTYPE, ASH_LOCALE_COLLATE, ASH_LOCALE_CATEGORY_COUNT };

struct ash_locale_scope {
    locale_t active;
    locale_t previous;
    /* Applied requests, including invalid-name fallback. */
    char* requests[ASH_LOCALE_CATEGORY_COUNT];
    bool dirty;
};

/* Enter a zero-initialized scope without changing the process-global locale. */
int ash_locale_scope_enter(struct ash_locale_scope* scope);
/* Restore before freeing. A failed restore leaves the scope owned and active. */
int ash_locale_scope_leave(struct ash_locale_scope* scope);
bool ash_locale_variable(const char* name, size_t length);
int ash_locale_refresh(struct ash_shell* shell);

#endif /* BX_APPLETS_SHELL_ASH_LOCALE_STATE_H */
