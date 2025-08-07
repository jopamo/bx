#ifndef BX_LIB_OUTPUT_H
#define BX_LIB_OUTPUT_H

#include <stdbool.h>
#include <stddef.h>

void bx_json_begin(void);
void bx_json_end(void);
void bx_json_object_begin(void);
void bx_json_object_end(void);
void bx_json_array_begin(const char *key);
void bx_json_array_end(void);
void bx_json_string(const char *key, const char *value);
void bx_json_number(const char *key, long long value);
void bx_json_bool(const char *key, bool value);
void bx_json_null(const char *key);

/* helpers for search output */
void bx_json_match_begin(void);
void bx_json_match_path(const char *path);
void bx_json_match_line_number(size_t n);
void bx_json_match_byte_offset(size_t n);
void bx_json_match_text(const char *text, size_t len);
void bx_json_match_end(void);

#endif
