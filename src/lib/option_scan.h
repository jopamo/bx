#ifndef BX_LIB_OPTION_SCAN_H
#define BX_LIB_OPTION_SCAN_H

#include <stdbool.h>
#include <stddef.h>

enum bx_option_value {
    BX_OPTION_NO_VALUE,
    BX_OPTION_REQUIRED_VALUE,
    BX_OPTION_OPTIONAL_VALUE,
};

struct bx_option_spec {
    int id;
    unsigned char short_name; /* 0 if absent */
    const char* long_name;    /* NULL if absent, without leading "--" */
    enum bx_option_value value;
};

enum bx_option_mode {
    BX_OPTION_ORDERED = 0, /* Return operands as events, including after "--". */
    BX_OPTION_PREFIX = 1 << 0, /* Stop at the first operand or just after "--". */
    BX_OPTION_ABBREVIATE = 1 << 1, /* Accept unique prefixes of long names. */
};

enum bx_option_event {
    BX_OPTION_END,
    BX_OPTION_MATCH,
    BX_OPTION_OPERAND,
    BX_OPTION_UNKNOWN,
    BX_OPTION_AMBIGUOUS,
    BX_OPTION_MISSING_VALUE,
    BX_OPTION_UNEXPECTED_VALUE,
};

struct bx_option_result {
    enum bx_option_event event;
    size_t index;
    const char* token; /* Original token; includes --NAME and any "=VALUE". */
    const struct bx_option_spec* spec; /* Matched descriptor, if any. */
    char* value; /* Borrowed argv bytes; NULL if absent, "" if supplied empty. */
    unsigned char short_name; /* Member of a short group, even on error. */
};

struct bx_option_cursor {
    char* const* argv;
    const struct bx_option_spec* specs;
    size_t spec_count;
    size_t index, end, short_offset;
    unsigned mode;
    bool literal, stopped;
};

/*
 * Scan a caller-owned [first, end) with non-NULL, NUL-terminated argument
 * strings. No sentinel or argv[0] access is required. The caller also owns
 * the immutable descriptors (unique spellings; NULL table permitted at
 * count zero). Keep the inputs alive and unchanged while using the cursor
 * or its results. Neither slots nor strings are modified. Reinitialize to
 * restart; abandoning a partially read group needs no cleanup.
 *
 * Long names match exactly unless mode includes BX_OPTION_ABBREVIATE.
 * Even then, prefer exact matches; multiple prefix matches are ambiguous.
 * Required values use the attached suffix or the next token (even "--");
 * optional values are attached only. Long values attach with '='. A short
 * value takes the rest of its group.
 */
void bx_option_cursor_init(struct bx_option_cursor* cursor, char* const argv[],
                           size_t first, size_t end, unsigned mode,
                           const struct bx_option_spec* specs, size_t spec_count);

/*
 * Each call emits one event. Help/version and option IDs are applet policy,
 * reported through the matched descriptor. A syntax error stops scanning;
 * callers must reject the invocation, not use the error index as a tail.
 * The next call returns END at the error index.
 * END is stable and returns the first unconsumed index (end if exhausted).
 * After a successful prefix scan, argv[result.index..end) is the untouched
 * tail; an ordered scan should collect OPERAND events if needed.
 */
struct bx_option_result bx_option_next(struct bx_option_cursor* cursor);

#endif
