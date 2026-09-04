#ifndef MIRA_RESOURCE_LIMITS_H
#define MIRA_RESOURCE_LIMITS_H

/* MIRA_HEADER_OWNER: runtime */
/* MIRA_HEADER_CONSUMERS: cli, core, crawl, store */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * Remote documents, redirects, input files, and persisted state are hostile
 * resource sources. These limits are one fail-closed contract shared by every
 * layer that retains URL-derived state.
 */
#ifndef MIRA_URL_MAX_BYTES
#define MIRA_URL_MAX_BYTES ((size_t)64 * 1024u)
#endif

#ifndef MIRA_URL_STATE_MAX_ENTRIES
#define MIRA_URL_STATE_MAX_ENTRIES ((size_t)64 * 1024u)
#endif

#ifndef MIRA_URL_STATE_MAX_BYTES
#define MIRA_URL_STATE_MAX_BYTES ((size_t)64 * 1024u * 1024u)
#endif

#ifndef MIRA_REDIRECT_CLAIM_MAX_ENTRIES
#define MIRA_REDIRECT_CLAIM_MAX_ENTRIES MIRA_URL_STATE_MAX_ENTRIES
#endif

#ifndef MIRA_REDIRECT_WAITER_MAX_ENTRIES
#define MIRA_REDIRECT_WAITER_MAX_ENTRIES MIRA_URL_STATE_MAX_ENTRIES
#endif

#ifndef MIRA_REDIRECT_STATE_MAX_BYTES
#define MIRA_REDIRECT_STATE_MAX_BYTES MIRA_URL_STATE_MAX_BYTES
#endif

#ifndef MIRA_URL_MAP_MAX_ENTRIES
#define MIRA_URL_MAP_MAX_ENTRIES MIRA_URL_STATE_MAX_ENTRIES
#endif

#ifndef MIRA_URL_MAP_MAX_FIELD_BYTES
#define MIRA_URL_MAP_MAX_FIELD_BYTES MIRA_URL_MAX_BYTES
#endif

#ifndef MIRA_URL_MAP_MAX_DECODED_BYTES
#define MIRA_URL_MAP_MAX_DECODED_BYTES MIRA_URL_STATE_MAX_BYTES
#endif

#ifndef MIRA_URL_MAP_STORE_MAX_BYTES
#define MIRA_URL_MAP_STORE_MAX_BYTES (MIRA_URL_MAP_MAX_DECODED_BYTES * 2u + MIRA_URL_MAP_MAX_ENTRIES * 3u)
#endif

#ifndef MIRA_URL_MAP_ENCODED_LINE_MAX_BYTES
#define MIRA_URL_MAP_ENCODED_LINE_MAX_BYTES (MIRA_URL_MAP_MAX_FIELD_BYTES * 4u + 3u)
#endif

#define MIRA_URL_STATE_ENTRY_LIMIT_TEXT "65536"
#define MIRA_URL_STATE_BYTE_LIMIT_TEXT "64 MiB"
#define MIRA_URL_LIMIT_TEXT "64 KiB"
#define MIRA_URL_MAP_ENTRY_LIMIT_TEXT "65536"
#define MIRA_URL_MAP_BYTE_LIMIT_TEXT "64 MiB decoded"

/*
 * Check a reservation without performing overflow-prone addition. Callers
 * mutate their counters only after all allocations needed by the reservation
 * have succeeded.
 */
static inline bool mira_resource_can_reserve(size_t current_entries, size_t current_bytes, size_t add_entries, size_t add_bytes, size_t max_entries, size_t max_bytes) {
    return current_entries <= max_entries && current_bytes <= max_bytes && add_entries <= max_entries - current_entries && add_bytes <= max_bytes - current_bytes;
}

static inline bool mira_resource_bounded_strlen(const char* value, size_t max_bytes, size_t* length_out) {
    if (!value || !length_out || max_bytes == SIZE_MAX)
        return false;

    size_t length = strnlen(value, max_bytes + 1u);
    if (length > max_bytes)
        return false;
    *length_out = length;
    return true;
}

#endif  // MIRA_RESOURCE_LIMITS_H
