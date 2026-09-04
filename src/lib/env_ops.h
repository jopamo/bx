#ifndef BX_LIB_ENV_OPS_H
#define BX_LIB_ENV_OPS_H

#include <stddef.h>

struct bx_env_vector {
    char **entries;
    size_t count;
    size_t capacity;
};

int bx_env_vector_init_empty(struct bx_env_vector *environment);
int bx_env_vector_init_current(struct bx_env_vector *environment);
void bx_env_vector_destroy(struct bx_env_vector *environment);

const char *bx_env_vector_get(
    const struct bx_env_vector *environment,
    const char *name);
int bx_env_vector_set(
    struct bx_env_vector *environment,
    const char *name,
    const char *value);
int bx_env_vector_append_raw(
    struct bx_env_vector *environment,
    const char *entry);
void bx_env_vector_unset(
    struct bx_env_vector *environment,
    const char *name);
char *const *bx_env_vector_data(const struct bx_env_vector *environment);

const char *bx_envp_get(char *const *envp, const char *name);

#endif
