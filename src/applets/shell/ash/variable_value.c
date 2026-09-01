#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "applets/shell/ash/variable_value.h"

static char* ash_value_duplicate(const char* text) {
    if (text == NULL) {
        errno = EINVAL;
        return NULL;
    }
    size_t length = strlen(text);
    if (length == SIZE_MAX) {
        errno = ENOMEM;
        return NULL;
    }
    char* copy = malloc(length + 1u);
    if (copy != NULL) {
        memcpy(copy, text, length + 1u);
    }
    return copy;
}

bool ash_value_init_scalar(struct ash_value* value, const char* scalar) {
    if (value == NULL) {
        errno = EINVAL;
        return false;
    }
    *value = (struct ash_value){.kind = ASH_VALUE_SCALAR};
    value->data.scalar = ash_value_duplicate(scalar);
    if (value->data.scalar != NULL) {
        assert(ash_value_invariants(value));
    }
    return value->data.scalar != NULL;
}

void ash_value_init_indexed(struct ash_value* value) {
    *value = (struct ash_value){.kind = ASH_VALUE_INDEXED_ARRAY};
    assert(ash_value_invariants(value));
}

void ash_value_init_associative(struct ash_value* value) {
    *value = (struct ash_value){.kind = ASH_VALUE_ASSOCIATIVE_ARRAY};
    assert(ash_value_invariants(value));
}

void ash_value_destroy(struct ash_value* value) {
    if (value == NULL) {
        return;
    }
    switch (value->kind) {
        case ASH_VALUE_SCALAR:
            free(value->data.scalar);
            break;
        case ASH_VALUE_INDEXED_ARRAY:
            for (size_t i = 0u; i < value->data.indexed.count; i++) {
                free(value->data.indexed.elements[i].value);
            }
            free(value->data.indexed.elements);
            break;
        case ASH_VALUE_ASSOCIATIVE_ARRAY:
            for (size_t i = 0u; i < value->data.associative.bucket_count; i++) {
                struct ash_associative_element* item =
                    value->data.associative.buckets[i];
                while (item != NULL) {
                    struct ash_associative_element* next = item->next;
                    free(item->key);
                    free(item->value);
                    free(item);
                    item = next;
                }
            }
            free(value->data.associative.buckets);
            break;
    }
    *value = (struct ash_value){0};
}

const char* ash_value_get_scalar(const struct ash_value* value) {
    return value != NULL && value->kind == ASH_VALUE_SCALAR ?
        value->data.scalar : NULL;
}

bool ash_value_set_scalar(struct ash_value* value, const char* scalar) {
    struct ash_value candidate;
    if (!ash_value_init_scalar(&candidate, scalar)) {
        return false;
    }
    ash_value_destroy(value);
    *value = candidate;
    assert(ash_value_invariants(value));
    return true;
}

static size_t ash_indexed_lower_bound(
    const struct ash_indexed_array* array,
    size_t index
) {
    size_t first = 0u;
    size_t count = array->count;
    while (count != 0u) {
        size_t step = count / 2u;
        size_t middle = first + step;
        if (array->elements[middle].index < index) {
            first = middle + 1u;
            count -= step + 1u;
        }
        else {
            count = step;
        }
    }
    return first;
}

const char* ash_value_indexed_get(const struct ash_value* value, size_t index) {
    if (value == NULL || value->kind != ASH_VALUE_INDEXED_ARRAY) {
        return NULL;
    }
    size_t position = ash_indexed_lower_bound(&value->data.indexed, index);
    return position < value->data.indexed.count &&
        value->data.indexed.elements[position].index == index ?
        value->data.indexed.elements[position].value : NULL;
}

bool ash_value_indexed_set(
    struct ash_value* value,
    size_t index,
    const char* element
) {
    if (value == NULL || value->kind != ASH_VALUE_INDEXED_ARRAY) {
        errno = EINVAL;
        return false;
    }
    struct ash_indexed_array* array = &value->data.indexed;
    size_t position = ash_indexed_lower_bound(array, index);
    char* copy = ash_value_duplicate(element);
    if (copy == NULL) {
        return false;
    }
    if (position < array->count && array->elements[position].index == index) {
        free(array->elements[position].value);
        array->elements[position].value = copy;
        assert(ash_value_invariants(value));
        return true;
    }
    if (array->count == array->capacity) {
        size_t capacity = array->capacity == 0u ? 4u : array->capacity * 2u;
        if (capacity < array->capacity ||
            capacity > SIZE_MAX / sizeof(*array->elements)) {
            free(copy);
            errno = ENOMEM;
            return false;
        }
        struct ash_indexed_element* elements = realloc(
            array->elements,
            capacity * sizeof(*elements)
        );
        if (elements == NULL) {
            free(copy);
            return false;
        }
        array->elements = elements;
        array->capacity = capacity;
    }
    memmove(
        &array->elements[position + 1u],
        &array->elements[position],
        (array->count - position) * sizeof(*array->elements)
    );
    array->elements[position] = (struct ash_indexed_element){
        .index = index,
        .value = copy,
    };
    array->count++;
    assert(ash_value_invariants(value));
    return true;
}

bool ash_value_indexed_unset(struct ash_value* value, size_t index) {
    if (value == NULL || value->kind != ASH_VALUE_INDEXED_ARRAY) {
        return false;
    }
    struct ash_indexed_array* array = &value->data.indexed;
    size_t position = ash_indexed_lower_bound(array, index);
    if (position == array->count || array->elements[position].index != index) {
        return false;
    }
    free(array->elements[position].value);
    array->count--;
    memmove(
        &array->elements[position],
        &array->elements[position + 1u],
        (array->count - position) * sizeof(*array->elements)
    );
    assert(ash_value_invariants(value));
    return true;
}

static size_t ash_associative_hash(const char* key) {
    size_t hash = sizeof(size_t) == 8u ?
        (size_t)UINT64_C(1469598103934665603) : (size_t)UINT32_C(2166136261);
    const size_t prime = sizeof(size_t) == 8u ?
        (size_t)UINT64_C(1099511628211) : (size_t)UINT32_C(16777619);
    for (const unsigned char* p = (const unsigned char*)key; *p != '\0'; p++) {
        hash ^= *p;
        hash *= prime;
    }
    return hash;
}

static bool ash_associative_chain_acyclic(
    const struct ash_associative_element* head
) {
    const struct ash_associative_element* slow = head;
    const struct ash_associative_element* fast = head;
    while (fast != NULL && fast->next != NULL) {
        slow = slow->next;
        fast = fast->next->next;
        if (slow == fast) {
            return false;
        }
    }
    return true;
}

bool ash_value_invariants(const struct ash_value* value) {
    if (value == NULL) {
        return false;
    }
    switch (value->kind) {
        case ASH_VALUE_SCALAR:
            return value->data.scalar != NULL;
        case ASH_VALUE_INDEXED_ARRAY: {
            const struct ash_indexed_array* array = &value->data.indexed;
            if (array->count > array->capacity ||
                (array->capacity == 0u) != (array->elements == NULL)) {
                return false;
            }
            for (size_t i = 0u; i < array->count; i++) {
                if (array->elements[i].value == NULL ||
                    (i != 0u &&
                     array->elements[i - 1u].index >=
                        array->elements[i].index)) {
                    return false;
                }
            }
            return true;
        }
        case ASH_VALUE_ASSOCIATIVE_ARRAY: {
            const struct ash_associative_array* array =
                &value->data.associative;
            if ((array->bucket_count == 0u) !=
                (array->buckets == NULL)) {
                return false;
            }
            size_t count = 0u;
            for (size_t i = 0u; i < array->bucket_count; i++) {
                if (!ash_associative_chain_acyclic(array->buckets[i])) {
                    return false;
                }
                for (const struct ash_associative_element* item =
                         array->buckets[i];
                     item != NULL;
                     item = item->next) {
                    if (item->key == NULL || item->key[0] == '\0' ||
                        item->value == NULL ||
                        item->hash != ash_associative_hash(item->key) ||
                        item->hash % array->bucket_count != i ||
                        count == SIZE_MAX) {
                        return false;
                    }
                    count++;
                }
                for (const struct ash_associative_element* item =
                         array->buckets[i];
                     item != NULL;
                     item = item->next) {
                    for (const struct ash_associative_element* duplicate =
                             item->next;
                         duplicate != NULL;
                         duplicate = duplicate->next) {
                        if (item->hash == duplicate->hash &&
                            strcmp(item->key, duplicate->key) == 0) {
                            return false;
                        }
                    }
                }
            }
            return count == array->count;
        }
    }
    return false;
}

static bool ash_associative_resize(struct ash_associative_array* array, size_t count) {
    struct ash_associative_element** buckets = calloc(count, sizeof(*buckets));
    if (buckets == NULL) {
        return false;
    }
    for (size_t i = 0u; i < array->bucket_count; i++) {
        struct ash_associative_element* item = array->buckets[i];
        while (item != NULL) {
            struct ash_associative_element* next = item->next;
            size_t bucket = item->hash % count;
            item->next = buckets[bucket];
            buckets[bucket] = item;
            item = next;
        }
    }
    free(array->buckets);
    array->buckets = buckets;
    array->bucket_count = count;
    return true;
}

static struct ash_associative_element* ash_associative_find(
    const struct ash_associative_array* array,
    const char* key,
    size_t hash
) {
    if (array->bucket_count == 0u) {
        return NULL;
    }
    for (struct ash_associative_element* item =
             array->buckets[hash % array->bucket_count];
         item != NULL;
         item = item->next) {
        if (item->hash == hash && strcmp(item->key, key) == 0) {
            return item;
        }
    }
    return NULL;
}

const char* ash_value_associative_get(
    const struct ash_value* value,
    const char* key
) {
    if (value == NULL || value->kind != ASH_VALUE_ASSOCIATIVE_ARRAY ||
        key == NULL) {
        return NULL;
    }
    struct ash_associative_element* item = ash_associative_find(
        &value->data.associative,
        key,
        ash_associative_hash(key)
    );
    return item != NULL ? item->value : NULL;
}

bool ash_value_associative_set(
    struct ash_value* value,
    const char* key,
    const char* element
) {
    if (value == NULL || value->kind != ASH_VALUE_ASSOCIATIVE_ARRAY ||
        key == NULL || key[0] == '\0') {
        errno = EINVAL;
        return false;
    }
    struct ash_associative_array* array = &value->data.associative;
    size_t hash = ash_associative_hash(key);
    struct ash_associative_element* item =
        ash_associative_find(array, key, hash);
    char* copy = ash_value_duplicate(element);
    if (copy == NULL) {
        return false;
    }
    if (item != NULL) {
        free(item->value);
        item->value = copy;
        assert(ash_value_invariants(value));
        return true;
    }
    if (array->bucket_count == 0u &&
        !ash_associative_resize(array, 8u)) {
        free(copy);
        return false;
    }
    if (array->count >=
        array->bucket_count - array->bucket_count / 4u) {
        if (array->bucket_count > SIZE_MAX / 2u ||
            !ash_associative_resize(array, array->bucket_count * 2u)) {
            free(copy);
            errno = ENOMEM;
            return false;
        }
    }
    item = calloc(1u, sizeof(*item));
    char* key_copy = ash_value_duplicate(key);
    if (item == NULL || key_copy == NULL) {
        free(item);
        free(key_copy);
        free(copy);
        return false;
    }
    item->key = key_copy;
    item->value = copy;
    item->hash = hash;
    size_t bucket = hash % array->bucket_count;
    item->next = array->buckets[bucket];
    array->buckets[bucket] = item;
    array->count++;
    assert(ash_value_invariants(value));
    return true;
}

bool ash_value_associative_unset(struct ash_value* value, const char* key) {
    if (value == NULL || value->kind != ASH_VALUE_ASSOCIATIVE_ARRAY ||
        key == NULL || value->data.associative.bucket_count == 0u) {
        return false;
    }
    struct ash_associative_array* array = &value->data.associative;
    size_t hash = ash_associative_hash(key);
    struct ash_associative_element** link =
        &array->buckets[hash % array->bucket_count];
    while (*link != NULL) {
        struct ash_associative_element* item = *link;
        if (item->hash == hash && strcmp(item->key, key) == 0) {
            *link = item->next;
            free(item->key);
            free(item->value);
            free(item);
            array->count--;
            assert(ash_value_invariants(value));
            return true;
        }
        link = &item->next;
    }
    return false;
}

bool ash_value_clone(
    struct ash_value* destination,
    const struct ash_value* source
) {
    if (destination == NULL || source == NULL) {
        errno = EINVAL;
        return false;
    }
    switch (source->kind) {
        case ASH_VALUE_SCALAR:
            return ash_value_init_scalar(destination, source->data.scalar);
        case ASH_VALUE_INDEXED_ARRAY:
            ash_value_init_indexed(destination);
            for (size_t i = 0u; i < source->data.indexed.count; i++) {
                if (!ash_value_indexed_set(
                        destination,
                        source->data.indexed.elements[i].index,
                        source->data.indexed.elements[i].value
                    )) {
                    ash_value_destroy(destination);
                    return false;
                }
            }
            return true;
        case ASH_VALUE_ASSOCIATIVE_ARRAY:
            ash_value_init_associative(destination);
            for (size_t i = 0u;
                 i < source->data.associative.bucket_count;
                 i++) {
                for (const struct ash_associative_element* item =
                         source->data.associative.buckets[i];
                     item != NULL;
                     item = item->next) {
                    if (!ash_value_associative_set(
                            destination,
                            item->key,
                            item->value
                        )) {
                        ash_value_destroy(destination);
                        return false;
                    }
                }
            }
            return true;
    }
    errno = EINVAL;
    return false;
}
