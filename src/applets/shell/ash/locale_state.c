#include <errno.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>

#include "applets/shell/ash/locale_state.h"
#include "applets/shell/ash/shell_context.h"
#include "applets/shell/ash/variables.h"

static const struct {
    const char* name;
    int mask;
} ash_locale_categories[ASH_LOCALE_CATEGORY_COUNT] = {
    [ASH_LOCALE_CTYPE] = {"LC_CTYPE", LC_CTYPE_MASK},
    [ASH_LOCALE_COLLATE] = {"LC_COLLATE", LC_COLLATE_MASK},
};

static const char* ash_locale_request(const char* all, const char* category, const char* lang) {
    return all != NULL && all[0] != '\0' ? all : (category != NULL && category[0] != '\0' ? category : (lang != NULL && lang[0] != '\0' ? lang : "C"));
}

static locale_t ash_locale_select(int mask, const char* name, locale_t base) {
    errno = 0;
    locale_t candidate = newlocale(mask, name, base);
    if (candidate == (locale_t)0 && (errno == ENOENT || errno == EINVAL)) {
        /* Invalid or unavailable locale names have unspecified shell behavior. */
        candidate = newlocale(mask, "C", base);
    }
    return candidate;
}

int ash_locale_scope_enter(struct ash_locale_scope* scope) {
    if (scope->active != (locale_t)0) {
        return EINVAL;
    }
    errno = 0;
    locale_t candidate = newlocale(LC_ALL_MASK, "", (locale_t)0);
    bool fallback = candidate == (locale_t)0 && (errno == ENOENT || errno == EINVAL);
    if (fallback) {
        candidate = newlocale(LC_ALL_MASK, "C", (locale_t)0);
    }
    if (candidate == (locale_t)0) {
        return errno != 0 ? errno : EINVAL;
    }
    struct ash_locale_scope owned = {.active = candidate, .dirty = fallback};
    for (size_t i = 0u; i < ASH_LOCALE_CATEGORY_COUNT; i++) {
        owned.requests[i] = strdup(fallback ? "C" : ash_locale_request(getenv("LC_ALL"), getenv(ash_locale_categories[i].name), getenv("LANG")));
        if (owned.requests[i] == NULL) {
            for (size_t j = 0u; j < i; j++) {
                free(owned.requests[j]);
            }
            freelocale(candidate);
            return ENOMEM;
        }
    }
    locale_t previous = uselocale(candidate);
    if (previous == (locale_t)0) {
        int error = errno != 0 ? errno : EINVAL;
        freelocale(candidate);
        for (size_t i = 0u; i < ASH_LOCALE_CATEGORY_COUNT; i++) {
            free(owned.requests[i]);
        }
        return error;
    }
    owned.previous = previous;
    *scope = owned;
    return 0;
}

int ash_locale_scope_leave(struct ash_locale_scope* scope) {
    if (scope->active == (locale_t)0) {
        return 0;
    }
    if (uselocale(scope->previous) == (locale_t)0) {
        return errno != 0 ? errno : EINVAL;
    }
    freelocale(scope->active);
    for (size_t i = 0u; i < ASH_LOCALE_CATEGORY_COUNT; i++) {
        free(scope->requests[i]);
    }
    *scope = (struct ash_locale_scope){0};
    return 0;
}

bool ash_locale_variable(const char* name, size_t length) {
    if ((length == 6u && memcmp(name, "LC_ALL", length) == 0) || (length == 4u && memcmp(name, "LANG", length) == 0)) {
        return true;
    }
    for (size_t i = 0u; i < ASH_LOCALE_CATEGORY_COUNT; i++) {
        if (length == strlen(ash_locale_categories[i].name) && memcmp(name, ash_locale_categories[i].name, length) == 0) {
            return true;
        }
    }
    return false;
}

int ash_locale_refresh(struct ash_shell* shell) {
    struct ash_locale_scope* scope = shell->locale_scope;
    if (scope == NULL || !scope->dirty) {
        return 0;
    }
    char* copies[ASH_LOCALE_CATEGORY_COUNT] = {0};
    const char* all = ash_var_get(shell, "LC_ALL");
    const char* lang = ash_var_get(shell, "LANG");
    bool changed = false;
    int error = 0;
    for (size_t i = 0u; i < ASH_LOCALE_CATEGORY_COUNT; i++) {
        const char* requested = ash_locale_request(all, ash_var_get(shell, ash_locale_categories[i].name), lang);
        if (strcmp(scope->requests[i], requested) == 0) {
            continue;
        }
        copies[i] = strdup(requested);
        if (copies[i] == NULL) {
            error = ENOMEM;
            goto free_copies;
        }
        changed = true;
    }
    if (!changed) {
        scope->dirty = false;
        return 0;
    }
    locale_t candidate = duplocale(scope->active);
    if (candidate == (locale_t)0) {
        error = errno != 0 ? errno : ENOMEM;
        goto free_copies;
    }
    for (size_t i = 0u; i < ASH_LOCALE_CATEGORY_COUNT; i++) {
        if (copies[i] == NULL) {
            continue;
        }
        locale_t updated = ash_locale_select(ash_locale_categories[i].mask, copies[i], candidate);
        if (updated == (locale_t)0) {
            error = errno != 0 ? errno : EINVAL;
            freelocale(candidate);
            goto free_copies;
        }
        candidate = updated;
    }
    if (uselocale(candidate) == (locale_t)0) {
        error = errno != 0 ? errno : EINVAL;
        freelocale(candidate);
        goto free_copies;
    }
    locale_t old = scope->active;
    scope->active = candidate;
    for (size_t i = 0u; i < ASH_LOCALE_CATEGORY_COUNT; i++) {
        if (copies[i] != NULL) {
            free(scope->requests[i]);
            scope->requests[i] = copies[i];
        }
    }
    scope->dirty = false;
    freelocale(old);
    return 0;

free_copies:
    for (size_t i = 0u; i < ASH_LOCALE_CATEGORY_COUNT; i++) {
        free(copies[i]);
    }
    return error;
}
