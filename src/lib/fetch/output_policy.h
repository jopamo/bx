#ifndef MIRA_OUTPUT_POLICY_H
#define MIRA_OUTPUT_POLICY_H

/* MIRA_HEADER_OWNER: runtime */
/* MIRA_HEADER_CONSUMERS: entry, core, net */

/*
 * Output policy is resolved once in EffectiveConfig. Runtime callers use these
 * predicates instead of reconstructing precedence from independent flags.
 * Debug traces and structured errors are diagnostic channels and remain
 * independent of human-output verbosity.
 */

#include "config.h"

static inline bool mira_output_is_quiet(const EffectiveConfig* cfg) {
    return cfg && cfg->logging.verbosity == MIRA_VERBOSITY_QUIET;
}

static inline bool mira_output_is_verbose(const EffectiveConfig* cfg) {
    return cfg && cfg->logging.verbosity == MIRA_VERBOSITY_VERBOSE;
}

static inline bool mira_output_progress_enabled(const EffectiveConfig* cfg) {
    return cfg && !mira_output_is_quiet(cfg) && cfg->download.show_progress;
}

static inline bool mira_output_server_response_enabled(const EffectiveConfig* cfg) {
    return cfg && !mira_output_is_quiet(cfg) && cfg->download.server_response;
}

static inline bool mira_output_status_enabled(const EffectiveConfig* cfg) {
    return cfg && !mira_output_is_quiet(cfg) && (mira_output_is_verbose(cfg) || cfg->download.server_response);
}

static inline bool mira_output_use_wget_basic(const EffectiveConfig* cfg) {
    if (!mira_output_is_verbose(cfg))
        return false;
    if (cfg->recursive.recursive || cfg->recursive.page_requisites)
        return false;
    if (cfg->input.input_file)
        return false;
    return cfg->input.url_count == 1;
}

static inline bool mira_output_debug_trace_enabled(const EffectiveConfig* cfg) {
    return cfg && cfg->logging.debug_trace;
}

static inline bool mira_output_structured_errors_enabled(const EffectiveConfig* cfg) {
    return !cfg || cfg->logging.structured_errors;
}

#endif  // MIRA_OUTPUT_POLICY_H
