#ifndef BX_APPLETS_SHELL_ASH_VARIABLE_VALUE_H
#define BX_APPLETS_SHELL_ASH_VARIABLE_VALUE_H

#include <stdbool.h>
#include <stddef.h>

enum ash_value_kind {
    ASH_VALUE_SCALAR = 0,
    ASH_VALUE_INDEXED_ARRAY,
    ASH_VALUE_ASSOCIATIVE_ARRAY,
};

struct ash_indexed_element {
    size_t index;
    char* value;
};

struct ash_indexed_array {
    /* Sorted by index: sparse lookup is logarithmic and iteration is stable. */
    struct ash_indexed_element* elements;
    size_t count;
    size_t capacity;
};

struct ash_associative_element {
    char* key;
    char* value;
    size_t hash;
    struct ash_associative_element* next;
};

struct ash_associative_array {
    /* Chained hash table: keyed lookup does not degrade to a full scan. */
    struct ash_associative_element** buckets;
    size_t bucket_count;
    size_t count;
};

struct ash_value {
    enum ash_value_kind kind;
    union {
        char* scalar;
        struct ash_indexed_array indexed;
        struct ash_associative_array associative;
    } data;
};

bool ash_value_init_scalar(struct ash_value* value, const char* scalar);
void ash_value_init_indexed(struct ash_value* value);
void ash_value_init_associative(struct ash_value* value);
bool ash_value_clone(struct ash_value* destination, const struct ash_value* source);
void ash_value_destroy(struct ash_value* value);

const char* ash_value_get_scalar(const struct ash_value* value);
bool ash_value_set_scalar(struct ash_value* value, const char* scalar);
const char* ash_value_indexed_get(const struct ash_value* value, size_t index);
bool ash_value_indexed_set(struct ash_value* value, size_t index, const char* element);
bool ash_value_indexed_unset(struct ash_value* value, size_t index);
const char* ash_value_associative_get(const struct ash_value* value, const char* key);
bool ash_value_associative_set(struct ash_value* value, const char* key, const char* element);
bool ash_value_associative_unset(struct ash_value* value, const char* key);

#endif /* BX_APPLETS_SHELL_ASH_VARIABLE_VALUE_H */
