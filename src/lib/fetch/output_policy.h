#ifndef BX_FETCH_OUTPUT_POLICY_H
#define BX_FETCH_OUTPUT_POLICY_H

/* BX_FETCH_HEADER_OWNER: runtime */
/* BX_FETCH_HEADER_CONSUMERS: entry, core, net */

/*
 * Output policy is resolved once in EffectiveConfig. Runtime callers use these
 * predicates instead of reconstructing precedence from independent flags.
 * Debug traces and structured errors are diagnostic channels and remain
 * independent of human-output verbosity.
 */

#include "config.h"

static inline bool bx_fetch_output_is_quiet(const EffectiveConfig* cfg) {
    return cfg && cfg->logging.verbosity == BX_FETCH_VERBOSITY_QUIET;
}

static inline bool bx_fetch_output_is_verbose(const EffectiveConfig* cfg) {
    return cfg && cfg->logging.verbosity == BX_FETCH_VERBOSITY_VERBOSE;
}

static inline bool bx_fetch_output_progress_enabled(const EffectiveConfig* cfg) {
    return cfg && !bx_fetch_output_is_quiet(cfg) && cfg->download.show_progress;
}

static inline bool bx_fetch_output_server_response_enabled(const EffectiveConfig* cfg) {
    return cfg && !bx_fetch_output_is_quiet(cfg) && cfg->download.server_response;
}

static inline bool bx_fetch_output_status_enabled(const EffectiveConfig* cfg) {
    return cfg && !bx_fetch_output_is_quiet(cfg) && (bx_fetch_output_is_verbose(cfg) || cfg->download.server_response);
}

static inline bool bx_fetch_output_use_wget_basic(const EffectiveConfig* cfg) {
    if (!bx_fetch_output_is_verbose(cfg))
        return false;
    if (cfg->recursive.recursive || cfg->recursive.page_requisites)
        return false;
    if (cfg->input.input_file)
        return false;
    return cfg->input.url_count == 1;
}

static inline bool bx_fetch_output_debug_trace_enabled(const EffectiveConfig* cfg) {
    return cfg && cfg->logging.debug_trace;
}

static inline bool bx_fetch_output_structured_errors_enabled(const EffectiveConfig* cfg) {
    return !cfg || cfg->logging.structured_errors;
}

#endif  // BX_FETCH_OUTPUT_POLICY_H
