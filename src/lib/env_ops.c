#include "lib/env_ops.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern char **environ;

static bool bx_env_entry_has_name(const char *entry,
                                  const char *name,
                                  size_t name_len) {
    return entry != NULL &&
           strncmp(entry, name, name_len) == 0 &&
           entry[name_len] == '=';
}

static int bx_env_vector_reserve(struct bx_env_vector *environment,
                                 size_t needed) {
    if (needed <= environment->capacity)
        return 0;
    if (needed > SIZE_MAX / sizeof(*environment->entries))
        return EOVERFLOW;

    size_t capacity = environment->capacity == 0 ? 8u : environment->capacity;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2u) {
            capacity = needed;
            break;
        }
        capacity *= 2u;
    }
    if (capacity > SIZE_MAX / sizeof(*environment->entries))
        return EOVERFLOW;

    char **entries = realloc(
        environment->entries, capacity * sizeof(*entries));
    if (entries == NULL)
        return errno != 0 ? errno : ENOMEM;
    environment->entries = entries;
    environment->capacity = capacity;
    return 0;
}

int bx_env_vector_init_empty(struct bx_env_vector *environment) {
    if (environment == NULL)
        return EINVAL;
    memset(environment, 0, sizeof(*environment));
    int error = bx_env_vector_reserve(environment, 1u);
    if (error != 0)
        return error;
    environment->entries[0] = NULL;
    return 0;
}

int bx_env_vector_init_current(struct bx_env_vector *environment) {
    int error = bx_env_vector_init_empty(environment);
    if (error != 0)
        return error;

    for (char **entry = environ; entry != NULL && *entry != NULL; entry++) {
        if (environment->count == SIZE_MAX - 1u) {
            error = EOVERFLOW;
            goto fail;
        }
        error = bx_env_vector_reserve(environment, environment->count + 2u);
        if (error != 0)
            goto fail;

        char *copy = strdup(*entry);
        if (copy == NULL) {
            error = errno != 0 ? errno : ENOMEM;
            goto fail;
        }
        environment->entries[environment->count++] = copy;
        environment->entries[environment->count] = NULL;
    }
    return 0;

fail:
    bx_env_vector_destroy(environment);
    return error;
}

void bx_env_vector_destroy(struct bx_env_vector *environment) {
    if (environment == NULL)
        return;
    for (size_t index = 0; index < environment->count; index++)
        free(environment->entries[index]);
    free(environment->entries);
    memset(environment, 0, sizeof(*environment));
}

const char *bx_env_vector_get(const struct bx_env_vector *environment,
                              const char *name) {
    if (environment == NULL || name == NULL || strchr(name, '=') != NULL)
        return NULL;
    size_t name_len = strlen(name);
    for (size_t index = 0; index < environment->count; index++) {
        if (bx_env_entry_has_name(
                environment->entries[index], name, name_len)) {
            return environment->entries[index] + name_len + 1u;
        }
    }
    return NULL;
}

int bx_env_vector_set(struct bx_env_vector *environment,
                      const char *name,
                      const char *value) {
    if (environment == NULL || name == NULL || value == NULL ||
        strchr(name, '=') != NULL) {
        return EINVAL;
    }

    size_t name_len = strlen(name);
    size_t value_len = strlen(value);
    if (name_len > SIZE_MAX - value_len - 2u)
        return EOVERFLOW;
    size_t entry_size = name_len + value_len + 2u;
    char *entry = malloc(entry_size);
    if (entry == NULL)
        return errno != 0 ? errno : ENOMEM;
    memcpy(entry, name, name_len);
    entry[name_len] = '=';
    memcpy(entry + name_len + 1u, value, value_len + 1u);

    for (size_t index = 0; index < environment->count; index++) {
        if (!bx_env_entry_has_name(
                environment->entries[index], name, name_len)) {
            continue;
        }
        free(environment->entries[index]);
        environment->entries[index] = entry;
        return 0;
    }

    if (environment->count == SIZE_MAX - 1u) {
        free(entry);
        return EOVERFLOW;
    }
    int error = bx_env_vector_reserve(environment, environment->count + 2u);
    if (error != 0) {
        free(entry);
        return error;
    }
    environment->entries[environment->count++] = entry;
    environment->entries[environment->count] = NULL;
    return 0;
}

int bx_env_vector_append_raw(struct bx_env_vector *environment,
                             const char *entry) {
    if (environment == NULL || entry == NULL)
        return EINVAL;

    char *copy = strdup(entry);
    if (copy == NULL)
        return errno != 0 ? errno : ENOMEM;
    if (environment->count == SIZE_MAX - 1u) {
        free(copy);
        return EOVERFLOW;
    }
    int error = bx_env_vector_reserve(environment, environment->count + 2u);
    if (error != 0) {
        free(copy);
        return error;
    }
    environment->entries[environment->count++] = copy;
    environment->entries[environment->count] = NULL;
    return 0;
}

void bx_env_vector_unset(struct bx_env_vector *environment,
                         const char *name) {
    if (environment == NULL || name == NULL || strchr(name, '=') != NULL)
        return;

    size_t name_len = strlen(name);
    size_t output = 0;
    for (size_t index = 0; index < environment->count; index++) {
        if (bx_env_entry_has_name(
                environment->entries[index], name, name_len)) {
            free(environment->entries[index]);
            continue;
        }
        environment->entries[output++] = environment->entries[index];
    }
    environment->count = output;
    environment->entries[output] = NULL;
}

char *const *bx_env_vector_data(const struct bx_env_vector *environment) {
    return environment != NULL ? environment->entries : NULL;
}

const char *bx_envp_get(char *const *envp, const char *name) {
    if (envp == NULL || name == NULL || strchr(name, '=') != NULL)
        return NULL;
    size_t name_len = strlen(name);
    for (char *const *entry = envp; *entry != NULL; entry++) {
        if (bx_env_entry_has_name(*entry, name, name_len))
            return *entry + name_len + 1u;
    }
    return NULL;
}
