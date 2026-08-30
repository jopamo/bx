#ifndef ZU_BUFFER_UTILS_H
#define ZU_BUFFER_UTILS_H

#include <stddef.h>
#include <stdint.h>

int zu_buffer_prepare(const void* input, uint8_t** out_buf, size_t* out_len);
int zu_buffer_alloc(uint8_t** out_buf, size_t* out_len, size_t hint);
void zu_buffer_discard(uint8_t** out_buf, size_t* out_len);

#endif
