#ifndef BX_SYSTEM_MODULE_SYMBOLS_H
#define BX_SYSTEM_MODULE_SYMBOLS_H

#ifdef __linux__
#include <stdbool.h>
#include <stddef.h>

struct bx_module_symbol;

/* Start zeroed. Add exports, seal once, then query without mutation. */
struct bx_module_symbols {
    struct bx_module_symbol* entries;
    size_t count;
    size_t capacity;
    size_t name_bytes;
    bool sealed;
};

enum bx_module_symbol_result {
    BX_MODULE_SYMBOL_MISSING,
    BX_MODULE_SYMBOL_FOUND,
    BX_MODULE_SYMBOL_AMBIGUOUS,
    BX_MODULE_SYMBOL_INVALID,
};

/* Provider 0 is the kernel; nonzero providers are caller-owned module IDs. */
bool bx_module_symbols_add(struct bx_module_symbols* table, const char* name,
                           size_t length, size_t provider);
void bx_module_symbols_seal(struct bx_module_symbols* table);
enum bx_module_symbol_result bx_module_symbols_find(const struct bx_module_symbols* table,
                                                    const char* name, size_t length,
                                                    size_t* provider);
void bx_module_symbols_destroy(struct bx_module_symbols* table);
#endif

#endif
