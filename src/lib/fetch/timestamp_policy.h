#ifndef MIRA_TIMESTAMP_POLICY_H
#define MIRA_TIMESTAMP_POLICY_H

/* MIRA_HEADER_OWNER: util */
/* MIRA_HEADER_CONSUMERS: util, core */

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

bool mira_timestamp_should_use_server_time(bool no_use_server_timestamps,
                                           int status,
                                           const char *output_path,
                                           const char *last_modified_header,
                                           time_t *server_mtime_out);

#endif // MIRA_TIMESTAMP_POLICY_H
