#ifndef MIRA_TYPES_H
#define MIRA_TYPES_H

/* MIRA_HEADER_OWNER: util */
/* MIRA_HEADER_CONSUMERS: util, runtime, fs, net */

/*
 * Layering contract:
 * - Shared primitive typedefs and small value structs used by low-level runtime
 *   and util/fs/net components.
 *
 * Ownership and lifetime:
 * - MiraHeader name/value fields are heap strings owned by the containing
 *   request/response object.
 * - MiraResult is a value type with no dynamic ownership.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "error.h"

typedef int32_t i32;
typedef uint32_t u32;
typedef int64_t i64;
typedef uint64_t u64;
typedef size_t usize;

typedef struct {
    char* name;
    char* value;
} MiraHeader;

typedef struct {
    bool ok;
    MiraError error;
} MiraResult;

#endif  // MIRA_TYPES_H
