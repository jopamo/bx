#ifndef MIRA_METADATA_H
#define MIRA_METADATA_H

/* MIRA_HEADER_OWNER: fs */
/* MIRA_HEADER_CONSUMERS: fs, core, net */

/*
 * Layering contract:
 * - Metadata sidecar read/write behavior is centralized in fs.
 * - Core/net pass metadata values but do not manage sidecar file formats.
 * - Serialized URL fields are canonicalized without authority userinfo.
 *
 * Ownership and lifetime:
 * - MiraMetadata string fields are heap-owned by the struct instance.
 * - metadata_load() clears and repopulates the struct; caller releases fields
 *   with metadata_clear().
 * - metadata_save()/metadata_write_stream() borrow input pointers only.
 */

#include <stdbool.h>
#include <stdio.h>

typedef struct {
    char* etag;
    char* last_modified;
    char* origin_url;
    char* redirect_target;
    char* local_path;
} MiraMetadata;

bool metadata_is_empty(const MiraMetadata* meta);
int metadata_write_stream(FILE* f, const MiraMetadata* meta);
int metadata_load(const char* output_path, MiraMetadata* meta);
int metadata_save(const char* output_path, const MiraMetadata* meta);
void metadata_clear(MiraMetadata* meta);

#endif  // MIRA_METADATA_H
