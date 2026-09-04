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

#include "config.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#define BX_FETCH_METADATA_LINE_MAX_BYTES ((size_t)8 * 1024u)
#define BX_FETCH_METADATA_FILE_MAX_BYTES ((size_t)256 * 1024u)
#define BX_FETCH_METADATA_MAX_FIELDS ((size_t)1024u)

typedef struct {
    char* etag;
    char* last_modified;
    char* origin_url;
    char* redirect_target;
    char* local_path;
} BxFetchMetadata;

typedef int (*BxFetchMetadataMappingVisitor)(void* userdata, const char* url, const char* local_path);

bool bx_fetch_metadata_is_empty(const BxFetchMetadata* meta);
int bx_fetch_metadata_write_stream(FILE* f, const BxFetchMetadata* meta);
/* Reads bounded metadata from an already-authorized borrowed stream. */
int bx_fetch_metadata_read_stream(FILE* f, BxFetchMetadata* meta);
int bx_fetch_metadata_load(const char* output_path, BxFetchMetadata* meta);
int bx_fetch_metadata_save(const char* output_path, const BxFetchMetadata* meta);
/*
 * Securely walks the configured output root without following symlinks and
 * emits URL mappings recovered from sidecars whose payload still exists.
 */
int bx_fetch_metadata_visit_recovery_mappings(const struct bx_fetch_config* cfg, BxFetchMetadataMappingVisitor visitor, void* userdata);
void bx_fetch_metadata_clear(BxFetchMetadata* meta);

#endif  // BX_FETCH_METADATA_H
