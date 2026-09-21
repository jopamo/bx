#include "lib/jq/filter.h"
#include <limits.h>

void bx_jq_filter_result_init(BxJqFilterResult* result) {
    if (!result)
        return;
    *result = (BxJqFilterResult){
        .error_message = jv_invalid(),
        .halt_message = jv_invalid(),
    };
}

void bx_jq_filter_result_clear(BxJqFilterResult* result) {
    if (!result)
        return;
    jv_free(result->error_message);
    jv_free(result->halt_message);
    bx_jq_filter_result_init(result);
}

static int jq_halt_code(jv value) {
    if (!jv_is_valid(value))
        return 0;
    if (jv_get_kind(value) != JV_KIND_NUMBER) {
        jv_free(value);
        return 5;
    }

    double number = jv_number_value(value);
    jv_free(value);
    if (number > INT_MAX || number < INT_MIN)
        return 5;
    return (int)number;
}

BxJqFilterStatus bx_jq_filter_run(jq_state* jq,
                                  jv input,
                                  int flags,
                                  BxJqOutputFn output,
                                  void* userdata,
                                  BxJqFilterResult* result) {
    if (!jq || !output || !result) {
        jv_free(input);
        return BX_JQ_FILTER_INIT_ERROR;
    }

    bx_jq_filter_result_clear(result);
    jq_start(jq, input, flags);

    jv value;
    while (jv_is_valid(value = jq_next(jq))) {
        result->produced_output = true;
        jv_kind kind = jv_get_kind(value);
        result->last_output_false_or_null =
            kind == JV_KIND_FALSE || kind == JV_KIND_NULL;

        int output_result = output(userdata, value);
        jv_free(value);
        if (output_result > 0)
            return BX_JQ_FILTER_STOPPED;
        if (output_result < 0)
            return BX_JQ_FILTER_OUTPUT_ERROR;
    }

    if (jq_halted(jq)) {
        jv_free(value);
        result->halt_code = jq_halt_code(jq_get_exit_code(jq));
        result->halt_message = jq_get_error_message(jq);
        return BX_JQ_FILTER_HALTED;
    }

    if (jv_invalid_has_msg(jv_copy(value))) {
        result->error_message = jv_invalid_get_msg(value);
        return BX_JQ_FILTER_RUNTIME_ERROR;
    }

    jv_free(value);
    return BX_JQ_FILTER_OK;
}

BxJqFilterStatus bx_jq_filter(jv input,
                              const char* program,
                              BxJqOutputFn output,
                              void* userdata,
                              BxJqFilterResult* result) {
    if (!program || !output || !result) {
        jv_free(input);
        return BX_JQ_FILTER_INIT_ERROR;
    }

    jq_state* jq = jq_init();
    if (!jq) {
        jv_free(input);
        return BX_JQ_FILTER_INIT_ERROR;
    }
    if (!jq_compile(jq, program)) {
        jv_free(input);
        jq_teardown(&jq);
        return BX_JQ_FILTER_COMPILE_ERROR;
    }

    BxJqFilterStatus status =
        bx_jq_filter_run(jq, input, 0, output, userdata, result);
    jq_teardown(&jq);
    return status;
}
