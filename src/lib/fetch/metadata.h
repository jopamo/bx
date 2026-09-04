#ifndef BX_FETCH_METADATA_H
#define BX_FETCH_METADATA_H

/* BX_FETCH_HEADER_OWNER: fs */
/* BX_FETCH_HEADER_CONSUMERS: fs, core, net */

/*
 * Layering contract:
 * - Metadata sidecar read/write behavior is centralized in fs.
 * - Core/net pass metadata values but do not manage sidecar file formats.
 * - Serialized URL fields are canonicalized without authority userinfo.
 *
 * Ownership and lifetime:
 * - BxFetchMetadata string fields are heap-owned by the struct instance.
 * - bx_fetch_metadata_load() clears and repopulates the struct; caller releases fields
 *   with bx_fetch_metadata_clear().
 * - bx_fetch_metadata_save()/bx_fetch_metadata_write_stream() borrow input pointers only.
 */

#include <stdbool.h>
#include <stdio.h>

typedef struct {
    char* etag;
    char* last_modified;
    char* origin_url;
    char* redirect_target;
    char* local_path;
} BxFetchMetadata;

bool bx_fetch_metadata_is_empty(const BxFetchMetadata* meta);
int bx_fetch_metadata_write_stream(FILE* f, const BxFetchMetadata* meta);
int bx_fetch_metadata_load(const char* output_path, BxFetchMetadata* meta);
int bx_fetch_metadata_save(const char* output_path, const BxFetchMetadata* meta);
void bx_fetch_metadata_clear(BxFetchMetadata* meta);

#endif  // BX_FETCH_METADATA_H
