#ifndef BX_FETCH_INPUT_H
#define BX_FETCH_INPUT_H

/* BX_FETCH_HEADER_OWNER: core */
/* BX_FETCH_HEADER_CONSUMERS: core */

/*
 * Bounded URL-input mechanics. The reader owns untrusted file parsing only;
 * URL normalization, protocol/filter policy, and diagnostics remain with the
 * run coordinator and frontend.
 */

#include <stddef.h>

typedef enum {
    BX_FETCH_INPUT_FAILURE_NONE = 0,
    BX_FETCH_INPUT_FAILURE_OPEN,
    BX_FETCH_INPUT_FAILURE_READ,
    BX_FETCH_INPUT_FAILURE_LINE_TOO_LONG,
    BX_FETCH_INPUT_FAILURE_INVALID_CONTROL,
    BX_FETCH_INPUT_FAILURE_FILE_TOO_LARGE,
    BX_FETCH_INPUT_FAILURE_HTML_TOO_LARGE,
    BX_FETCH_INPUT_FAILURE_HTML_PARSE,
    BX_FETCH_INPUT_FAILURE_BASE_URL,
    BX_FETCH_INPUT_FAILURE_URL_STATE_LIMIT,
    BX_FETCH_INPUT_FAILURE_MEMORY,
} BxFetchInputFailureKind;

typedef struct {
    BxFetchInputFailureKind kind;
    size_t line_number;
    int error_number;
} BxFetchInputOutcome;

typedef struct {
    char** urls;
    size_t count;
    size_t retained_bytes;
    size_t capacity;
} BxFetchInputUrls;

/*
 * Reads one user-selected file through a single descriptor. Empty lines and
 * lines beginning with '#' are ignored; all other bytes are preserved except
 * the line ending. reserved_* accounts for direct operands sharing the same
 * global URL-state budget.
 */
int bx_fetch_input_urls_load_plain(const char* path, size_t reserved_entries, size_t reserved_bytes, BxFetchInputUrls* urls_out, BxFetchInputOutcome* outcome_out);
int bx_fetch_input_urls_load_html(const char* path, const char* base_url, size_t reserved_entries, size_t reserved_bytes, BxFetchInputUrls* urls_out, BxFetchInputOutcome* outcome_out);
void bx_fetch_input_urls_free(BxFetchInputUrls* urls);

#endif
