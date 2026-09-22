#ifndef BX_FETCH_PATHMAP_H
#define BX_FETCH_PATHMAP_H

/* BX_FETCH_HEADER_OWNER: fs */
/* BX_FETCH_HEADER_CONSUMERS: fs, core */

/*
 * Layering contract:
 * - URL-to-path mapping and filename sanitization remain in fs mapping code.
 * - Core chooses policy inputs but does not reimplement sanitization rules.
 *
 * Ownership and lifetime:
 * - Returned strings are heap-allocated and owned by the caller.
 * - Input pointers are borrowed and never retained.
 * - Distinct components never collide solely because sanitization replaces
 *   forbidden bytes; transformed inputs use a reserved whole-input encoding.
 * - bx_fetch_pathmap_url_to_local() canonicalizes an untrusted URL and fails on errors.
 * - bx_fetch_pathmap_canonical_url_to_local() is the internal fast path for a URL
 *   already canonicalized at a trust boundary.
 */

#include "config.h"
#include "url.h"

char* bx_fetch_pathmap_sanitize_component(const char* component, const struct bx_fetch_config* cfg);
/* Last nonempty path segment, or index.html for the root; always one safe leaf. */
char* bx_fetch_pathmap_prepared_basename(const BxFetchPreparedUrl* target, const struct bx_fetch_config* cfg);
char* bx_fetch_pathmap_url_to_local(const char* url, const struct bx_fetch_config* cfg);
char* bx_fetch_pathmap_canonical_url_to_local(const char* canonical_url, const struct bx_fetch_config* cfg);
/* Returns an owned path whose final component has a Markdown extension. */
char* bx_fetch_pathmap_markdown_path(const char* path);

#endif  // BX_FETCH_PATHMAP_H
