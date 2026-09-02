#ifndef BX_APPLETS_SHELL_ASH_ALIASES_H
#define BX_APPLETS_SHELL_ASH_ALIASES_H

#include <stdbool.h>
#include <stddef.h>

struct ash_alias;
struct ash_alias_table;
struct ash_lexer_options;
struct ash_word;

bool ash_alias_name_valid(const char* name);
bool ash_alias_define(
    struct ash_alias_table** table,
    const char* name,
    const char* value
);
const struct ash_alias* ash_alias_find(
    const struct ash_alias_table* table,
    const char* name
);
const struct ash_alias* ash_alias_find_word(
    const struct ash_alias_table* table,
    const struct ash_word* word
);
bool ash_alias_table_contains(
    const struct ash_alias_table* table,
    const struct ash_alias* alias
);
bool ash_alias_unset(
    struct ash_alias_table** table,
    const char* name
);
void ash_aliases_destroy(struct ash_alias_table** table);
bool ash_aliases_invariants(const struct ash_alias_table* table);

const char* ash_alias_name(const struct ash_alias* alias);
const char* ash_alias_value(const struct ash_alias* alias);
size_t ash_alias_value_length(const struct ash_alias* alias);
bool ash_alias_value_ends_blank(const struct ash_alias* alias);
bool ash_alias_requires_tail(
    const struct ash_alias* alias,
    const struct ash_lexer_options* options
);

/*
 * Returns a caller-owned, name-sorted array borrowing immutable entries from
 * table. A successful empty snapshot is represented by NULL and zero.
 */
bool ash_alias_snapshot(
    const struct ash_alias_table* table,
    const struct ash_alias*** aliases,
    size_t* count
);

#endif /* BX_APPLETS_SHELL_ASH_ALIASES_H */
