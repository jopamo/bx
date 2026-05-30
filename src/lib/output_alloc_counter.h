#ifndef BX_LIB_OUTPUT_ALLOC_COUNTER_H
#define BX_LIB_OUTPUT_ALLOC_COUNTER_H

#include <stdbool.h>
#include <stddef.h>

bool bx_output_alloc_counter_enabled(void);
void bx_output_alloc_counter_begin_from_env(const char *applet_name);
void bx_output_alloc_counter_reset(void);
void bx_output_alloc_counter_note_alloc(size_t bytes);
void bx_output_alloc_counter_note_cstring_alloc(const char *text);
void bx_output_alloc_counter_note_realloc(size_t bytes);
void bx_output_alloc_counter_report_stderr(void);

#endif /* BX_LIB_OUTPUT_ALLOC_COUNTER_H */
