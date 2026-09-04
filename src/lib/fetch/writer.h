#ifndef BX_FETCH_WRITER_H
#define BX_FETCH_WRITER_H

/* BX_FETCH_HEADER_OWNER: fs */
/* BX_FETCH_HEADER_CONSUMERS: fs, core, net */

/*
 * Layering contract:
 * - All payload/sidecar commit and rollback semantics are centralized in fs
 *   writer state-machine code.
 * - Core/net must use BxFetchWriter APIs instead of open/rename/fsync directly for
 *   download commit paths.
 *
 * Ownership and lifetime:
 * - bx_fetch_writer_open*() returns an owned BxFetchWriter handle.
 * - bx_fetch_writer_close() and bx_fetch_writer_abort() are terminal: both consume and free the
 *   BxFetchWriter handle in all paths.
 * - Caller must not reuse a BxFetchWriter pointer after close/abort.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "metadata.h"
#include "types.h"

typedef struct BxFetchWriter BxFetchWriter;

typedef enum {
    WRITER_CREATE = 0,
    WRITER_RESUME,
} BxFetchWriterMode;

/*
 * Verifies the Linux openat2(2) path-resolution primitive required by every
 * filesystem-backed BxFetchWriter. Returns 0 when available, or -1 with errno set.
 * There is deliberately no weaker openat(2) fallback.
 */
int bx_fetch_writer_check_secure_path_resolution(void);
/* Opens an existing regular file without following any path-component symlink. */
int bx_fetch_writer_open_existing_file(const char* path);
/* Removes a non-directory leaf through a no-symlink parent traversal. */
int bx_fetch_writer_unlink_file(const char* path);
BxFetchWriter* bx_fetch_writer_open(const char* path, BxFetchWriterMode mode);
BxFetchWriter* bx_fetch_writer_open_with_options(const char* path, BxFetchWriterMode mode, int backups, bool unlink_existing);
/* Must stay within the same parent directory; updates final commit target name. */
int bx_fetch_writer_set_final_path(BxFetchWriter* w, const char* path);
/*
 * As above, but requires the final payload and sidecar names to remain absent
 * and publishes without replacement.
 */
int bx_fetch_writer_set_final_path_exclusive(BxFetchWriter* w, const char* path);
/* Copies metadata into writer staging state for commit-time sidecar handling. */
int bx_fetch_writer_stage_metadata(BxFetchWriter* w, const BxFetchMetadata* meta);
/*
 * Copies regular-destination mode and user xattrs to the private candidate.
 * A changed destination identity fails rather than copying attacker-selected
 * metadata.
 */
int bx_fetch_writer_preserve_destination_metadata(BxFetchWriter* w);
/* Resets write stream for full replacement semantics before additional writes. */
int bx_fetch_writer_begin_replace(BxFetchWriter* w);
int bx_fetch_writer_write(BxFetchWriter* w, const void* data, size_t len);
/* Applies mtime to the private candidate before publication. */
int bx_fetch_writer_set_mtime(BxFetchWriter* w, time_t mtime);
int bx_fetch_writer_stage_xattrs(BxFetchWriter* w, const char* url, const char* content_type, const char* etag, const char* last_modified);
/* Terminal success path: commits payload/metadata and frees `w`. */
int bx_fetch_writer_close(BxFetchWriter* w);
/* Terminal failure path: drops staged artifacts and frees `w`. */
void bx_fetch_writer_abort(BxFetchWriter* w);
/* Size already staged in this private candidate (nonzero for true resume). */
uint64_t bx_fetch_writer_candidate_size(const BxFetchWriter* w);
/*
 * Returns the mtime captured from the original regular destination through
 * the writer's retained parent authority. False means no regular destination.
 */
bool bx_fetch_writer_original_mtime(const BxFetchWriter* w, time_t* mtime_out);
/*
 * Loads the sidecar adjacent to the original destination through the retained
 * parent descriptor. Missing metadata succeeds with an empty result.
 */
int bx_fetch_writer_load_original_metadata(const BxFetchWriter* w, BxFetchMetadata* metadata);
/* Borrowed pointer owned by BxFetchWriter; invalid after bx_fetch_writer_close()/bx_fetch_writer_abort(). */
const char* bx_fetch_writer_get_path(const BxFetchWriter* w);

#endif  // BX_FETCH_WRITER_H
