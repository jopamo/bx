#ifndef BX_LIB_JQ_FILTER_H
#define BX_LIB_JQ_FILTER_H

#include "vendor/jq/src/jq.h"
#include <stdbool.h>

/*
 * Values passed to BxJqOutputFn are borrowed for the duration of the call.
 * Copy a value with jv_copy() if it must outlive the callback.
 */
typedef int (*BxJqOutputFn)(void* userdata, jv value);

typedef enum {
    BX_JQ_FILTER_RUNTIME_ERROR = -4,
    BX_JQ_FILTER_OUTPUT_ERROR = -3,
    BX_JQ_FILTER_COMPILE_ERROR = -2,
    BX_JQ_FILTER_INIT_ERROR = -1,
    BX_JQ_FILTER_OK = 0,
    BX_JQ_FILTER_STOPPED = 1,
    BX_JQ_FILTER_HALTED = 2,
} BxJqFilterStatus;

typedef struct {
    bool produced_output;
    bool last_output_false_or_null;
    int halt_code;
    jv error_message;
    jv halt_message;
} BxJqFilterResult;

void bx_jq_filter_result_init(BxJqFilterResult* result);
void bx_jq_filter_result_clear(BxJqFilterResult* result);

/*
 * Runs an already compiled jq state. This consumes input. Returning a positive
 * value from output stops iteration successfully; a negative value reports an
 * output failure.
 */
BxJqFilterStatus bx_jq_filter_run(jq_state* jq,
                                  jv input,
                                  int flags,
                                  BxJqOutputFn output,
                                  void* userdata,
                                  BxJqFilterResult* result);

/*
 * Initializes jq, compiles program, runs one input value, and tears jq down.
 * This consumes input on every path.
 */
BxJqFilterStatus bx_jq_filter(jv input,
                              const char* program,
                              BxJqOutputFn output,
                              void* userdata,
                              BxJqFilterResult* result);

#endif
