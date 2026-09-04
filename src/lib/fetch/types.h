#ifndef BX_FETCH_TYPES_H
#define BX_FETCH_TYPES_H

/* BX_FETCH_HEADER_OWNER: util */
/* BX_FETCH_HEADER_CONSUMERS: util, runtime, fs, net */

/*
 * Layering contract:
 * - Shared primitive typedefs and small value structs used by low-level runtime
 *   and util/fs/net components.
 *
 * Ownership and lifetime:
 * - BxFetchHeader name/value fields are heap strings owned by the containing
 *   request/response object.
 * - BxFetchResult is a value type with no dynamic ownership.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "error.h"

typedef int32_t BxFetchI32;
typedef uint32_t BxFetchU32;
typedef int64_t BxFetchI64;
typedef uint64_t BxFetchU64;
typedef size_t BxFetchUsize;

typedef struct {
    char* name;
    char* value;
} BxFetchHeader;

typedef struct {
    bool ok;
    BxFetchError error;
} BxFetchResult;

#endif  // BX_FETCH_TYPES_H
