#ifndef MIRA_PATHMAP_H
#define MIRA_PATHMAP_H

/* MIRA_HEADER_OWNER: fs */
/* MIRA_HEADER_CONSUMERS: fs, core */

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
 * - pathmap_url_to_local() canonicalizes an untrusted URL and fails on errors.
 * - pathmap_canonical_url_to_local() is the internal fast path for a URL
 *   already canonicalized at a trust boundary.
 */

#include "config.h"

char *pathmap_sanitize_component(const char *component, const EffectiveConfig *cfg);
char *pathmap_url_to_local(const char *url, const EffectiveConfig *cfg);
char *pathmap_canonical_url_to_local(const char *canonical_url,
                                     const EffectiveConfig *cfg);

#endif // MIRA_PATHMAP_H
