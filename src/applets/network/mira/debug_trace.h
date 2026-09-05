#ifndef BX_APPLETS_NETWORK_MIRA_DEBUG_TRACE_H
#define BX_APPLETS_NETWORK_MIRA_DEBUG_TRACE_H

#include "lib/fetch/config.h"
#include "lib/fetch/run.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    FILE* stream;
    uint64_t sequence;
    bool enabled;
} MiraDebugTrace;

void bx_mira_json_write_string(FILE* stream, const char* value);
void bx_mira_debug_trace_init(MiraDebugTrace* trace, FILE* stream, const struct bx_fetch_config* config);
void bx_mira_debug_trace_parse_complete(MiraDebugTrace* trace, const struct bx_fetch_config* config);
void bx_mira_debug_trace_enqueued(MiraDebugTrace* trace, const char* display_url, const char* output_path, int depth);
void bx_mira_debug_trace_dry_run_decision(MiraDebugTrace* trace, const char* display_url, const char* output_path);
void bx_mira_debug_trace_transfer_observation(MiraDebugTrace* trace, const BxFetchRunTransferObservation* observation);
void bx_mira_debug_trace_completion(MiraDebugTrace* trace, const struct bx_fetch_config* config, const BxFetchRunCompletion* completion);

#endif
