#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "context_roll.h"

#define BX_SEARCH_CONTEXT_INITIAL_BYTE_CAP 4096u
#define BX_SEARCH_CONTEXT_INITIAL_ENTRY_CAP 8u

static size_t bx_search_context_active_bytes(
    const struct bx_search_context_roll *roll
) {
    if (!roll || roll->bytes_len < roll->bytes_front)
        return 0u;
    return roll->bytes_len - roll->bytes_front;
}

static size_t bx_search_context_active_entries(
    const struct bx_search_context_roll *roll
) {
    if (!roll || roll->entries_len < roll->entries_front)
        return 0u;
    return roll->entries_len - roll->entries_front;
}

void bx_search_context_roll_clear(struct bx_search_context_roll *roll) {
    if (!roll)
        return;
    roll->bytes_front = 0u;
    roll->bytes_len = 0u;
    roll->entries_front = 0u;
    roll->entries_len = 0u;
}

void bx_search_context_roll_dispose(struct bx_search_context_roll *roll) {
    if (!roll)
        return;
    free(roll->bytes);
    free(roll->entries);
    memset(roll, 0, sizeof(*roll));
}

size_t bx_search_context_roll_count(const struct bx_search_context_roll *roll) {
    return bx_search_context_active_entries(roll);
}

static void bx_search_context_roll_pop_front(struct bx_search_context_roll *roll) {
    struct bx_search_context_entry *entry;

    if (!roll || bx_search_context_active_entries(roll) == 0u)
        return;
    entry = &roll->entries[roll->entries_front++];
    roll->bytes_front = entry->byte_index + entry->len;
    if (roll->entries_front == roll->entries_len)
        bx_search_context_roll_clear(roll);
}

static bool bx_search_context_double_target(size_t needed,
                                            size_t initial,
                                            size_t *target_out) {
    size_t target = initial;

    if (!target_out) {
        errno = EINVAL;
        return false;
    }
    while (target < needed) {
        if (target > SIZE_MAX / 2u) {
            target = needed;
            break;
        }
        target *= 2u;
    }
    *target_out = target;
    return true;
}

static void bx_search_context_compact_bytes(struct bx_search_context_roll *roll) {
    size_t active;

    if (!roll || roll->bytes_front == 0u)
        return;
    active = bx_search_context_active_bytes(roll);
    if (active > 0u)
        memmove(roll->bytes, roll->bytes + roll->bytes_front, active);
    for (size_t i = roll->entries_front; i < roll->entries_len; i++)
        roll->entries[i].byte_index -= roll->bytes_front;
    roll->bytes_front = 0u;
    roll->bytes_len = active;
}

static bool bx_search_context_reserve_bytes(struct bx_search_context_roll *roll,
                                            size_t append_len) {
    size_t active;
    size_t needed;
    size_t target;
    unsigned char *grown;

    if (!roll) {
        errno = EINVAL;
        return false;
    }
    if (append_len <= roll->bytes_cap - roll->bytes_len)
        return true;

    active = bx_search_context_active_bytes(roll);
    if (append_len > SIZE_MAX - active) {
        errno = ENOMEM;
        return false;
    }
    needed = active + append_len;
    if (roll->bytes_front > 0u && needed <= roll->bytes_cap / 2u) {
        bx_search_context_compact_bytes(roll);
        return true;
    }
    bx_search_context_compact_bytes(roll);

    if (!bx_search_context_double_target(
            needed, BX_SEARCH_CONTEXT_INITIAL_BYTE_CAP, &target)) {
        return false;
    }
    if (needed <= SIZE_MAX / 2u && target < needed * 2u)
        target = needed * 2u;
    grown = realloc(roll->bytes, target);
    if (!grown) {
        errno = ENOMEM;
        return false;
    }
    roll->bytes = grown;
    roll->bytes_cap = target;
    return true;
}

static void bx_search_context_compact_entries(struct bx_search_context_roll *roll) {
    size_t active;

    if (!roll || roll->entries_front == 0u)
        return;
    active = bx_search_context_active_entries(roll);
    if (active > 0u) {
        memmove(roll->entries,
                roll->entries + roll->entries_front,
                active * sizeof(*roll->entries));
    }
    roll->entries_front = 0u;
    roll->entries_len = active;
}

static bool bx_search_context_reserve_entry(struct bx_search_context_roll *roll) {
    size_t active;
    size_t needed;
    size_t target;
    struct bx_search_context_entry *grown;

    if (!roll) {
        errno = EINVAL;
        return false;
    }
    if (roll->entries_len < roll->entries_cap)
        return true;

    active = bx_search_context_active_entries(roll);
    needed = active + 1u;
    if (roll->entries_front > 0u && needed <= roll->entries_cap / 2u) {
        bx_search_context_compact_entries(roll);
        return true;
    }
    bx_search_context_compact_entries(roll);
    if (!bx_search_context_double_target(
            needed, BX_SEARCH_CONTEXT_INITIAL_ENTRY_CAP, &target)) {
        return false;
    }
    if (target > SIZE_MAX / sizeof(*roll->entries)) {
        errno = ENOMEM;
        return false;
    }
    grown = realloc(roll->entries, target * sizeof(*roll->entries));
    if (!grown) {
        errno = ENOMEM;
        return false;
    }
    roll->entries = grown;
    roll->entries_cap = target;
    return true;
}

bool bx_search_context_roll_push(struct bx_search_context_roll *roll,
                                 const unsigned char *text,
                                 size_t len,
                                 size_t byte_offset,
                                 size_t line_number,
                                 size_t max_records) {
    struct bx_search_context_entry *entry;

    if (!roll || (!text && len > 0u)) {
        errno = EINVAL;
        return false;
    }
    if (max_records == 0u) {
        bx_search_context_roll_clear(roll);
        return true;
    }
    while (bx_search_context_active_entries(roll) >= max_records)
        bx_search_context_roll_pop_front(roll);
    if (!bx_search_context_reserve_entry(roll) ||
        !bx_search_context_reserve_bytes(roll, len)) {
        return false;
    }

    entry = &roll->entries[roll->entries_len++];
    entry->byte_index = roll->bytes_len;
    entry->len = len;
    entry->byte_offset = byte_offset;
    entry->line_number = line_number;
    if (len > 0u)
        memcpy(roll->bytes + roll->bytes_len, text, len);
    roll->bytes_len += len;
    return true;
}

bool bx_search_context_roll_get(const struct bx_search_context_roll *roll,
                                size_t index,
                                struct bx_search_context_record *record) {
    size_t count;
    const struct bx_search_context_entry *entry;

    if (!roll || !record)
        return false;
    count = bx_search_context_active_entries(roll);
    if (index >= count)
        return false;
    entry = &roll->entries[roll->entries_front + index];
    record->text = entry->len > 0u
        ? roll->bytes + entry->byte_index
        : (const unsigned char *)"";
    record->len = entry->len;
    record->byte_offset = entry->byte_offset;
    record->line_number = entry->line_number;
    return true;
}
