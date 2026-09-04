#ifndef BX_FETCH_TIMESTAMP_POLICY_H
#define BX_FETCH_TIMESTAMP_POLICY_H

/* BX_FETCH_HEADER_OWNER: util */
/* BX_FETCH_HEADER_CONSUMERS: util, core */

/*
 * Layering contract:
 * - Timestamp selection stays independent of transport implementation details.
 * - Callers provide policy inputs; this helper never calls libcurl or touches
 *   filesystem state directly.
 *
 * Ownership and lifetime:
 * - Inputs are borrowed.
 * - `server_mtime_out`, when non-NULL, is written only on success.
 */

#include <stdbool.h>
#include <time.h>

bool bx_fetch_timestamp_should_use_server_time(bool no_use_server_timestamps, int status, const char* output_path, const char* last_modified_header, time_t* server_mtime_out);

#endif  // BX_FETCH_TIMESTAMP_POLICY_H
