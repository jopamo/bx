/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "module_symbols.h"

#ifdef __linux__
#include <errno.h>
#include <stdlib.h>
#include <string.h>

enum { EXPORTS_MAX = 262144, EXPORT_BYTES_MAX = 32 * 1024 * 1024, NAME_MAX_BYTES = 4096 };

struct bx_module_symbol {
    char* name;
    size_t length;
    size_t provider;
};

static int name_order(const char* a, size_t alen, const char* b, size_t blen) {
    int order = memcmp(a, b, alen < blen ? alen : blen);
    return order ? order : (alen > blen) - (alen < blen);
}

static int compare_entries(const void* a, const void* b) {
    const struct bx_module_symbol* left = a;
    const struct bx_module_symbol* right = b;
    int order = name_order(left->name, left->length, right->name, right->length);
    return order ? order : (left->provider > right->provider) - (left->provider < right->provider);
}

bool bx_module_symbols_add(struct bx_module_symbols* table, const char* name,
                           size_t length, size_t provider) {
    if (table->sealed || !name || !length || length > NAME_MAX_BYTES) {
        errno = EINVAL;
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        if (name[i] < '!' || name[i] > '~') {
            errno = EINVAL;
            return false;
        }
    }
    if (table->count == EXPORTS_MAX || length + 1 > EXPORT_BYTES_MAX - table->name_bytes) {
        errno = EFBIG;
        return false;
    }
    if (table->count == table->capacity) {
        size_t capacity = table->capacity ? table->capacity * 2 : 64;
        struct bx_module_symbol* grown = realloc(table->entries, capacity * sizeof(*grown));
        if (!grown)
            return false;
        table->entries = grown;
        table->capacity = capacity;
    }
    char* owned = malloc(length + 1);
    if (!owned)
        return false;
    memcpy(owned, name, length);
    owned[length] = '\0';
    table->entries[table->count++] = (struct bx_module_symbol){owned, length, provider};
    table->name_bytes += length + 1;
    return true;
}

void bx_module_symbols_seal(struct bx_module_symbols* table) {
    if (table->sealed)
        return;
    table->sealed = true;
    if (table->count < 2)
        return;
    qsort(table->entries, table->count, sizeof(*table->entries), compare_entries);
    size_t count = 1;
    for (size_t i = 1; i < table->count; i++) {
        if (compare_entries(&table->entries[count - 1], &table->entries[i]) == 0)
            free(table->entries[i].name);
        else
            table->entries[count++] = table->entries[i];
    }
    table->count = count;
}

enum bx_module_symbol_result bx_module_symbols_find(const struct bx_module_symbols* table,
                                                    const char* name, size_t length,
                                                    size_t* provider) {
    if (!table->sealed || !name || !length || length > NAME_MAX_BYTES || !provider)
        return BX_MODULE_SYMBOL_INVALID;
    size_t low = 0, high = table->count;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        const struct bx_module_symbol* item = &table->entries[mid];
        if (name_order(item->name, item->length, name, length) < 0)
            low = mid + 1;
        else
            high = mid;
    }
    if (low == table->count ||
        name_order(table->entries[low].name, table->entries[low].length, name, length) != 0)
        return BX_MODULE_SYMBOL_MISSING;
    const struct bx_module_symbol* first = &table->entries[low];
    if (first->provider != 0 && low + 1 < table->count &&
        name_order(table->entries[low + 1].name, table->entries[low + 1].length, name, length) == 0)
        return BX_MODULE_SYMBOL_AMBIGUOUS;
    *provider = first->provider;
    return BX_MODULE_SYMBOL_FOUND;
}

void bx_module_symbols_destroy(struct bx_module_symbols* table) {
    for (size_t i = 0; i < table->count; i++)
        free(table->entries[i].name);
    free(table->entries);
    *table = (struct bx_module_symbols){0};
}
#endif
