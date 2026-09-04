#ifndef BX_FETCH_WRITER_H
#define BX_FETCH_WRITER_H

/* BX_FETCH_HEADER_OWNER: fs */
/* BX_FETCH_HEADER_CONSUMERS: fs, core, net */

/*
 * Layering contract:
 * - All payload/sidecar commit and rollback semantics are centralized in fs
 *   writer state-machine code.
 * - Core/net must use Writer APIs instead of open/rename/fsync directly for
 *   download commit paths.
 *
 * Ownership and lifetime:
 * - bx_fetch_writer_open*() returns an owned Writer handle.
 * - bx_fetch_writer_close() and bx_fetch_writer_abort() are terminal: both consume and free the
 *   Writer handle in all paths.
 * - Caller must not reuse a Writer pointer after close/abort.
 */

#include <stdbool.h>
#include <stddef.h>
#include <time.h>
#include "metadata.h"
#include "types.h"

typedef struct Writer Writer;

typedef enum {
    WRITER_CREATE = 0,
    WRITER_RESUME,
} WriterMode;

/*
 * Verifies the Linux openat2(2) path-resolution primitive required by every
 * filesystem-backed Writer. Returns 0 when available, or -1 with errno set.
 * There is deliberately no weaker openat(2) fallback.
 */
int bx_fetch_writer_check_secure_path_resolution(void);
/* Opens an existing regular file without following any path-component symlink. */
int bx_fetch_writer_open_existing_file(const char* path);
/* Removes a non-directory leaf through a no-symlink parent traversal. */
int bx_fetch_writer_unlink_file(const char* path);
Writer* bx_fetch_writer_open(const char* path, WriterMode mode);
Writer* bx_fetch_writer_open_with_options(const char* path, WriterMode mode, int backups, bool unlink_existing);
/* Must stay within the same parent directory; updates final commit target name. */
int bx_fetch_writer_set_final_path(Writer* w, const char* path);
/*
 * As above, but requires the final payload and sidecar names to remain absent
 * and publishes without replacement.
 */
int bx_fetch_writer_set_final_path_exclusive(Writer* w, const char* path);
/* Copies metadata into writer staging state for commit-time sidecar handling. */
int bx_fetch_writer_stage_metadata(Writer* w, const MiraMetadata* meta);
/*
 * Copies regular-destination mode and user xattrs to the private candidate.
 * A changed destination identity fails rather than copying attacker-selected
 * metadata.
 */
int bx_fetch_writer_preserve_destination_metadata(Writer* w);
/* Resets write stream for full replacement semantics before additional writes. */
int bx_fetch_writer_begin_replace(Writer* w);
int bx_fetch_writer_write(Writer* w, const void* data, size_t len);
/* Applies mtime to the private candidate before publication. */
int bx_fetch_writer_set_mtime(Writer* w, time_t mtime);
int bx_fetch_writer_stage_xattrs(Writer* w, const char* url, const char* content_type, const char* etag, const char* last_modified);
/* Terminal success path: commits payload/metadata and frees `w`. */
int bx_fetch_writer_close(Writer* w);
/* Terminal failure path: drops staged artifacts and frees `w`. */
void bx_fetch_writer_abort(Writer* w);
i64 bx_fetch_writer_get_size(const char* path);
/* Borrowed pointer owned by Writer; invalid after bx_fetch_writer_close()/bx_fetch_writer_abort(). */
const char* bx_fetch_writer_get_path(const Writer* w);

#endif  // BX_FETCH_WRITER_H
