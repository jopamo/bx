#include "buffer_utils.h"

#include <stdlib.h>

#include "ziputils.h"

int zu_buffer_prepare(const void* input, uint8_t** out_buf, size_t* out_len) {
    if (!input || !out_buf || !out_len)
        return ZU_STATUS_USAGE;

    *out_buf = NULL;
    *out_len = 0;
    return ZU_STATUS_OK;
}

int zu_buffer_alloc(uint8_t** out_buf, size_t* out_len, size_t hint) {
    if (!out_buf || !out_len)
        return ZU_STATUS_USAGE;

    *out_buf = malloc(hint);
    if (!*out_buf)
        return ZU_STATUS_OOM;

    *out_len = hint;
    return ZU_STATUS_OK;
}

void zu_buffer_discard(uint8_t** out_buf, size_t* out_len) {
    if (out_buf && *out_buf) {
        free(*out_buf);
        *out_buf = NULL;
    }

    if (out_len)
        *out_len = 0;
}
