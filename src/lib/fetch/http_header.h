#ifndef MIRA_HTTP_HEADER_H
#define MIRA_HTTP_HEADER_H

/* MIRA_HEADER_OWNER: runtime */
/* MIRA_HEADER_CONSUMERS: runtime, cli, core, net */

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
    MIRA_HTTP_HEADER_OK = 0,
    MIRA_HTTP_HEADER_INVALID_ARGUMENT,
    MIRA_HTTP_HEADER_MALFORMED_LINE,
    MIRA_HTTP_HEADER_INVALID_NAME,
    MIRA_HTTP_HEADER_INVALID_VALUE,
    MIRA_HTTP_HEADER_FORBIDDEN_FRAMING,
    MIRA_HTTP_HEADER_OUT_OF_MEMORY,
} MiraHttpHeaderError;

typedef enum {
    MIRA_CONTENT_DISPOSITION_NONE = 0,
    MIRA_CONTENT_DISPOSITION_FILENAME,
    MIRA_CONTENT_DISPOSITION_INVALID,
    MIRA_CONTENT_DISPOSITION_OUT_OF_MEMORY,
} MiraContentDispositionResult;

const char *mira_http_header_error_string(MiraHttpHeaderError error);

MiraHttpHeaderError mira_http_header_normalize_line(
    const char *line, char **normalized_out);
MiraHttpHeaderError mira_http_header_normalize_pair(
    const char *name, const char *value,
    char **normalized_name_out, char **normalized_value_out);

/*
 * Produce one CURLOPT_HTTPHEADER list entry. Empty values use libcurl's
 * explicit `name;` form so they are sent rather than interpreted as a request
 * to remove an internally generated header.
 */
MiraHttpHeaderError mira_http_header_format_line_for_curl(
    const char *line, char **formatted_out);
MiraHttpHeaderError mira_http_header_format_pair_for_curl(
    const char *name, const char *value, char **formatted_out);

/*
 * Parses one Content-Disposition field using strict token/quoted-string and
 * RFC 5987 UTF-8 filename* rules. Duplicate, malformed, control-bearing, or
 * unsupported extended filename parameters invalidate the field.
 */
MiraContentDispositionResult mira_http_content_disposition_filename(
    const char *value, char **filename_out);

#endif // MIRA_HTTP_HEADER_H
