#ifndef BX_FETCH_HTTP_HEADER_H
#define BX_FETCH_HTTP_HEADER_H

/* BX_FETCH_HEADER_OWNER: runtime */
/* BX_FETCH_HEADER_CONSUMERS: runtime, cli, core, net */

/*
 * Custom request-header policy:
 * - Field names use the RFC HTTP token grammar.
 * - Values may contain HTAB, SP, visible ASCII, and obs-text, but no other
 *   control characters.
 * - Content-Length, Transfer-Encoding, and Trailer remain owned by
 *   Mira/libcurl so caller input cannot contradict request framing.
 * - Normalization trims leading/trailing optional whitespace from values.
 *
 * All successful string-producing functions return heap-owned output.
 */

typedef enum {
    BX_FETCH_HTTP_HEADER_OK = 0,
    BX_FETCH_HTTP_HEADER_INVALID_ARGUMENT,
    BX_FETCH_HTTP_HEADER_MALFORMED_LINE,
    BX_FETCH_HTTP_HEADER_INVALID_NAME,
    BX_FETCH_HTTP_HEADER_INVALID_VALUE,
    BX_FETCH_HTTP_HEADER_FORBIDDEN_FRAMING,
    BX_FETCH_HTTP_HEADER_OUT_OF_MEMORY,
} BxFetchHttpHeaderError;

typedef enum {
    BX_FETCH_CONTENT_DISPOSITION_NONE = 0,
    BX_FETCH_CONTENT_DISPOSITION_FILENAME,
    BX_FETCH_CONTENT_DISPOSITION_INVALID,
    BX_FETCH_CONTENT_DISPOSITION_OUT_OF_MEMORY,
} BxFetchContentDispositionResult;

const char* bx_fetch_http_header_error_string(BxFetchHttpHeaderError error);

BxFetchHttpHeaderError bx_fetch_http_header_normalize_line(const char* line, char** normalized_out);
BxFetchHttpHeaderError bx_fetch_http_header_normalize_pair(const char* name, const char* value, char** normalized_name_out, char** normalized_value_out);

/*
 * Produce one CURLOPT_HTTPHEADER list entry. Empty values use libcurl's
 * explicit `name;` form so they are sent rather than interpreted as a request
 * to remove an internally generated header.
 */
BxFetchHttpHeaderError bx_fetch_http_header_format_line_for_curl(const char* line, char** formatted_out);
BxFetchHttpHeaderError bx_fetch_http_header_format_pair_for_curl(const char* name, const char* value, char** formatted_out);

/*
 * Parses one Content-Disposition field using strict token/quoted-string and
 * RFC 5987 UTF-8 filename* rules. Duplicate, malformed, control-bearing, or
 * unsupported extended filename parameters invalidate the field.
 */
BxFetchContentDispositionResult bx_fetch_http_content_disposition_filename(const char* value, char** filename_out);

#endif  // BX_FETCH_HTTP_HEADER_H
