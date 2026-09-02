#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "applets/shell/ash/aliases.h"
#include "applets/shell/ash/lexer.h"
#include "applets/shell/ash/syntax.h"

#define ASH_ALIAS_MIN_BUCKETS 16u

struct ash_alias {
    char* name;
    char* value;
    size_t name_length;
    size_t value_length;
    uint64_t hash;
    bool value_ends_blank;
    bool requires_tail;
    bool requires_extglob_tail;
    struct ash_alias* next;
};

struct ash_alias_table {
    struct ash_alias** buckets;
    size_t bucket_count;
    size_t count;
};

static uint64_t ash_alias_hash_bytes(
    uint64_t hash,
    const char* text,
    size_t length
) {
    for (size_t i = 0u; i < length; i++) {
        hash ^= (unsigned char)text[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static uint64_t ash_alias_hash(const char* name, size_t length) {
    return ash_alias_hash_bytes(
        UINT64_C(14695981039346656037),
        name,
        length
    );
}

static size_t ash_alias_bucket(
    uint64_t hash,
    size_t bucket_count
) {
    return (size_t)(hash & (uint64_t)(bucket_count - 1u));
}

bool ash_alias_name_valid(const char* name) {
    if (name == NULL || name[0] == '\0') {
        return false;
    }
    static const char forbidden[] =
        "/$`= \t\r\n\\'\"()<>;&|";
    return strpbrk(name, forbidden) == NULL;
}

static bool ash_alias_table_shape_valid(
    const struct ash_alias_table* table
) {
    return table != NULL &&
        table->buckets != NULL &&
        table->bucket_count >= ASH_ALIAS_MIN_BUCKETS &&
        (table->bucket_count & (table->bucket_count - 1u)) == 0u &&
        table->count != 0u;
}

static struct ash_alias* ash_alias_find_key(
    struct ash_alias* alias,
    const char* name,
    size_t length,
    uint64_t hash
) {
    for (; alias != NULL; alias = alias->next) {
        if (alias->hash == hash &&
            alias->name_length == length &&
            memcmp(alias->name, name, length) == 0) {
            return alias;
        }
    }
    return NULL;
}

const struct ash_alias* ash_alias_find(
    const struct ash_alias_table* table,
    const char* name
) {
    if (table == NULL || name == NULL ||
        !ash_alias_table_shape_valid(table)) {
        return NULL;
    }
    size_t length = strlen(name);
    uint64_t hash = ash_alias_hash(name, length);
    size_t bucket = ash_alias_bucket(hash, table->bucket_count);
    return ash_alias_find_key(
        table->buckets[bucket],
        name,
        length,
        hash
    );
}

static struct ash_alias* ash_alias_find_mutable(
    struct ash_alias_table* table,
    const char* name
) {
    if (table == NULL || name == NULL ||
        !ash_alias_table_shape_valid(table)) {
        return NULL;
    }
    size_t length = strlen(name);
    uint64_t hash = ash_alias_hash(name, length);
    size_t bucket = ash_alias_bucket(hash, table->bucket_count);
    return ash_alias_find_key(
        table->buckets[bucket],
        name,
        length,
        hash
    );
}

static bool ash_alias_word_key(
    const struct ash_word* word,
    uint64_t* hash,
    size_t* length
) {
    if (word == NULL || hash == NULL || length == NULL) {
        return false;
    }
    *hash = UINT64_C(14695981039346656037);
    *length = 0u;
    for (size_t i = 0u; i < word->count; i++) {
        const struct ash_word_part* part = &word->parts[i];
        if (part->kind != ASH_WORD_TEXT ||
            ash_word_part_is_quoted(part) ||
            part->length > SIZE_MAX - *length) {
            return false;
        }
        *hash = ash_alias_hash_bytes(
            *hash,
            part->text,
            part->length
        );
        *length += part->length;
    }
    return *length != 0u;
}

static bool ash_alias_word_matches(
    const struct ash_alias* alias,
    const struct ash_word* word
) {
    size_t offset = 0u;
    for (size_t i = 0u; i < word->count; i++) {
        const struct ash_word_part* part = &word->parts[i];
        if (part->length != 0u &&
            memcmp(
                alias->name + offset,
                part->text,
                part->length
            ) != 0) {
            return false;
        }
        offset += part->length;
    }
    return offset == alias->name_length;
}

const struct ash_alias* ash_alias_find_word(
    const struct ash_alias_table* table,
    const struct ash_word* word
) {
    if (table == NULL || !ash_alias_table_shape_valid(table)) {
        return NULL;
    }
    uint64_t hash;
    size_t length;
    if (!ash_alias_word_key(word, &hash, &length)) {
        return NULL;
    }
    size_t bucket = ash_alias_bucket(hash, table->bucket_count);
    for (const struct ash_alias* alias = table->buckets[bucket];
         alias != NULL;
         alias = alias->next) {
        if (alias->hash == hash &&
            alias->name_length == length &&
            ash_alias_word_matches(alias, word)) {
            return alias;
        }
    }
    return NULL;
}

bool ash_alias_table_contains(
    const struct ash_alias_table* table,
    const struct ash_alias* alias
) {
    if (table == NULL || alias == NULL ||
        !ash_alias_table_shape_valid(table)) {
        return false;
    }
    size_t bucket = ash_alias_bucket(
        alias->hash,
        table->bucket_count
    );
    for (const struct ash_alias* current = table->buckets[bucket];
         current != NULL;
         current = current->next) {
        if (current == alias) {
            return true;
        }
    }
    return false;
}

static struct ash_alias_table* ash_alias_table_create(void) {
    struct ash_alias_table* table = calloc(1u, sizeof(*table));
    if (table == NULL) {
        return NULL;
    }
    table->buckets = calloc(
        ASH_ALIAS_MIN_BUCKETS,
        sizeof(table->buckets[0])
    );
    if (table->buckets == NULL) {
        free(table);
        return NULL;
    }
    table->bucket_count = ASH_ALIAS_MIN_BUCKETS;
    return table;
}

static bool ash_alias_table_reserve(
    struct ash_alias_table* table
) {
    size_t threshold =
        table->bucket_count - table->bucket_count / 4u;
    if (table->count < threshold) {
        return true;
    }
    if (table->bucket_count > SIZE_MAX / 2u ||
        table->bucket_count * 2u >
            SIZE_MAX / sizeof(table->buckets[0])) {
        errno = ENOMEM;
        return false;
    }

    size_t new_count = table->bucket_count * 2u;
    struct ash_alias** buckets = calloc(
        new_count,
        sizeof(buckets[0])
    );
    if (buckets == NULL) {
        return false;
    }
    for (size_t i = 0u; i < table->bucket_count; i++) {
        struct ash_alias* alias = table->buckets[i];
        while (alias != NULL) {
            struct ash_alias* next = alias->next;
            size_t bucket = ash_alias_bucket(alias->hash, new_count);
            alias->next = buckets[bucket];
            buckets[bucket] = alias;
            alias = next;
        }
    }
    free(table->buckets);
    table->buckets = buckets;
    table->bucket_count = new_count;
    return true;
}

static bool ash_alias_value_ends_blank_bytes(
    const char* value,
    size_t length
) {
    return length != 0u &&
        (value[length - 1u] == ' ' ||
         value[length - 1u] == '\t');
}

static bool ash_alias_value_metadata(
    const char* value,
    size_t* length,
    bool* ends_blank,
    bool* requires_tail,
    bool* requires_extglob_tail
) {
    *length = strlen(value);
    enum ash_lexer_fragment_result fragment =
        ash_lexer_classify_fragment(value, *length);
    if (fragment == ASH_LEXER_FRAGMENT_ERROR) {
        return false;
    }
    *ends_blank = ash_alias_value_ends_blank_bytes(value, *length);
    *requires_tail =
        fragment == ASH_LEXER_FRAGMENT_NEEDS_TAIL;
    fragment = ash_lexer_classify_fragment_with_options(
        value,
        *length,
        &(const struct ash_lexer_options){
            .flags = ASH_LEXER_EXTGLOB,
        }
    );
    if (fragment == ASH_LEXER_FRAGMENT_ERROR) {
        return false;
    }
    *requires_extglob_tail =
        fragment == ASH_LEXER_FRAGMENT_NEEDS_TAIL;
    return true;
}

static struct ash_alias* ash_alias_create(
    const char* name,
    const char* value
) {
    size_t value_length;
    bool value_ends_blank;
    bool requires_tail;
    bool requires_extglob_tail;
    if (!ash_alias_value_metadata(
            value,
            &value_length,
            &value_ends_blank,
            &requires_tail,
            &requires_extglob_tail
        )) {
        return NULL;
    }
    struct ash_alias* alias = calloc(1u, sizeof(*alias));
    char* name_copy = strdup(name);
    char* value_copy = strdup(value);
    if (alias == NULL || name_copy == NULL || value_copy == NULL) {
        free(alias);
        free(name_copy);
        free(value_copy);
        return NULL;
    }

    size_t name_length = strlen(name);
    *alias = (struct ash_alias){
        .name = name_copy,
        .value = value_copy,
        .name_length = name_length,
        .value_length = value_length,
        .hash = ash_alias_hash(name, name_length),
        .value_ends_blank = value_ends_blank,
        .requires_tail = requires_tail,
        .requires_extglob_tail = requires_extglob_tail,
    };
    return alias;
}

bool ash_alias_define(
    struct ash_alias_table** table,
    const char* name,
    const char* value
) {
    if (table == NULL || !ash_alias_name_valid(name) ||
        value == NULL) {
        errno = EINVAL;
        return false;
    }

    struct ash_alias* existing =
        ash_alias_find_mutable(*table, name);
    if (existing != NULL) {
        size_t value_length;
        bool value_ends_blank;
        bool requires_tail;
        bool requires_extglob_tail;
        if (!ash_alias_value_metadata(
                value,
                &value_length,
                &value_ends_blank,
                &requires_tail,
                &requires_extglob_tail
            )) {
            return false;
        }
        char* value_copy = strdup(value);
        if (value_copy == NULL) {
            return false;
        }
        char* old_value = existing->value;
        existing->value = value_copy;
        existing->value_length = value_length;
        existing->value_ends_blank = value_ends_blank;
        existing->requires_tail = requires_tail;
        existing->requires_extglob_tail = requires_extglob_tail;
        free(old_value);
        return true;
    }

    struct ash_alias* candidate = ash_alias_create(name, value);
    if (candidate == NULL) {
        return false;
    }
    struct ash_alias_table* active = *table;
    bool new_table = active == NULL;
    if (new_table) {
        active = ash_alias_table_create();
        if (active == NULL) {
            free(candidate->name);
            free(candidate->value);
            free(candidate);
            return false;
        }
    }
    if (!ash_alias_table_reserve(active)) {
        if (new_table) {
            free(active->buckets);
            free(active);
        }
        free(candidate->name);
        free(candidate->value);
        free(candidate);
        return false;
    }

    size_t bucket = ash_alias_bucket(
        candidate->hash,
        active->bucket_count
    );
    candidate->next = active->buckets[bucket];
    active->buckets[bucket] = candidate;
    active->count++;
    if (new_table) {
        *table = active;
    }
    return true;
}

static void ash_alias_destroy(struct ash_alias* alias) {
    free(alias->name);
    free(alias->value);
    free(alias);
}

bool ash_alias_unset(
    struct ash_alias_table** table,
    const char* name
) {
    if (table == NULL || *table == NULL || name == NULL) {
        return false;
    }
    struct ash_alias_table* active = *table;
    size_t length = strlen(name);
    uint64_t hash = ash_alias_hash(name, length);
    size_t bucket = ash_alias_bucket(hash, active->bucket_count);
    struct ash_alias** link = &active->buckets[bucket];
    while (*link != NULL) {
        struct ash_alias* alias = *link;
        if (alias->hash == hash &&
            alias->name_length == length &&
            memcmp(alias->name, name, length) == 0) {
            *link = alias->next;
            active->count--;
            ash_alias_destroy(alias);
            if (active->count == 0u) {
                free(active->buckets);
                free(active);
                *table = NULL;
            }
            return true;
        }
        link = &alias->next;
    }
    return false;
}

void ash_aliases_destroy(struct ash_alias_table** table) {
    if (table == NULL || *table == NULL) {
        return;
    }
    struct ash_alias_table* active = *table;
    for (size_t i = 0u; i < active->bucket_count; i++) {
        struct ash_alias* alias = active->buckets[i];
        while (alias != NULL) {
            struct ash_alias* next = alias->next;
            ash_alias_destroy(alias);
            alias = next;
        }
    }
    free(active->buckets);
    free(active);
    *table = NULL;
}

const char* ash_alias_name(const struct ash_alias* alias) {
    return alias != NULL ? alias->name : NULL;
}

const char* ash_alias_value(const struct ash_alias* alias) {
    return alias != NULL ? alias->value : NULL;
}

size_t ash_alias_value_length(const struct ash_alias* alias) {
    return alias != NULL ? alias->value_length : 0u;
}

bool ash_alias_value_ends_blank(const struct ash_alias* alias) {
    return alias != NULL && alias->value_ends_blank;
}

bool ash_alias_requires_tail(
    const struct ash_alias* alias,
    const struct ash_lexer_options* options
) {
    if (alias == NULL || !ash_lexer_options_valid(options)) {
        return false;
    }
    return (options->flags & ASH_LEXER_EXTGLOB) != 0u ?
        alias->requires_extglob_tail :
        alias->requires_tail;
}

static int ash_alias_compare(const void* left, const void* right) {
    const struct ash_alias* const* a = left;
    const struct ash_alias* const* b = right;
    return strcmp((*a)->name, (*b)->name);
}

bool ash_alias_snapshot(
    const struct ash_alias_table* table,
    const struct ash_alias*** aliases,
    size_t* count
) {
    if (aliases == NULL || count == NULL) {
        errno = EINVAL;
        return false;
    }
    *aliases = NULL;
    *count = 0u;
    if (table == NULL) {
        return true;
    }
    if (!ash_alias_table_shape_valid(table) ||
        table->count > SIZE_MAX / sizeof(**aliases)) {
        errno = EINVAL;
        return false;
    }

    const struct ash_alias** result = malloc(
        table->count * sizeof(result[0])
    );
    if (result == NULL) {
        return false;
    }
    size_t offset = 0u;
    for (size_t i = 0u; i < table->bucket_count; i++) {
        for (const struct ash_alias* alias = table->buckets[i];
             alias != NULL;
             alias = alias->next) {
            result[offset++] = alias;
        }
    }
    if (offset != table->count) {
        free(result);
        errno = EINVAL;
        return false;
    }
    qsort(
        result,
        table->count,
        sizeof(result[0]),
        ash_alias_compare
    );
    *aliases = result;
    *count = table->count;
    return true;
}

bool ash_aliases_invariants(const struct ash_alias_table* table) {
    if (table == NULL) {
        return true;
    }
    if (!ash_alias_table_shape_valid(table)) {
        return false;
    }

    size_t count = 0u;
    for (size_t i = 0u; i < table->bucket_count; i++) {
        const struct ash_alias* slow = table->buckets[i];
        const struct ash_alias* fast = table->buckets[i];
        while (fast != NULL && fast->next != NULL) {
            slow = slow->next;
            fast = fast->next->next;
            if (slow == fast) {
                return false;
            }
        }
        for (const struct ash_alias* alias = table->buckets[i];
             alias != NULL;
             alias = alias->next) {
            size_t value_length =
                alias->value != NULL ?
                    strlen(alias->value) :
                    0u;
            count++;
            if (alias->name == NULL || alias->value == NULL ||
                !ash_alias_name_valid(alias->name) ||
                strlen(alias->name) != alias->name_length ||
                value_length != alias->value_length ||
                ash_alias_hash(alias->name, alias->name_length) !=
                    alias->hash ||
                ash_alias_bucket(alias->hash, table->bucket_count) !=
                    i ||
                alias->value_ends_blank !=
                    ash_alias_value_ends_blank_bytes(
                        alias->value,
                        value_length
                    ) ||
                ash_alias_find(table, alias->name) != alias) {
                return false;
            }
        }
    }
    return count == table->count;
}
