#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bx/diag.h"
#include "bx/libbx.h"
#include "lib/output_alloc_counter.h"
#include "lib/output_quote.h"

bool bx_output_quote_terminal_should_hide_control(int fd) {
    return isatty(fd) == 1;
}

static bool bx_output_quote_capacity_checked(size_t len, size_t multiplier, size_t extra, size_t* capacity_out) {
    if (multiplier != 0u && len > (SIZE_MAX - extra) / multiplier) {
        return false;
    }
    *capacity_out = len * multiplier + extra;
    return true;
}

static size_t bx_output_quote_capacity(size_t len, size_t multiplier, size_t extra) {
    size_t capacity = 0u;
    if (!bx_output_quote_capacity_checked(len, multiplier, extra, &capacity)) {
        bx_fatal(3, "path quote allocation overflow");
    }
    return capacity;
}

static char* bx_output_quote_alloc(size_t size) {
    char* out = xmalloc(size);
    bx_output_alloc_counter_note_alloc(size);
    return out;
}

static char* bx_output_quote_try_alloc(size_t size) {
    char* out = malloc(size);
    if (out != NULL) {
        bx_output_alloc_counter_note_alloc(size);
    }
    return out;
}

static char* bx_output_quote_strdup(const char* text) {
    char* out = xstrdup(text);
    bx_output_alloc_counter_note_cstring_alloc(text);
    return out;
}

static char* bx_output_quote_try_strdup(const char* text) {
    size_t len;
    char* out;

    if (text == NULL) {
        text = "";
    }

    len = strlen(text);
    if (len == SIZE_MAX) {
        return NULL;
    }

    out = malloc(len + 1u);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, text, len + 1u);
    bx_output_alloc_counter_note_cstring_alloc(text);
    return out;
}

static size_t bx_output_quote_append_octal(char* out, size_t out_pos, unsigned char ch) {
    out[out_pos++] = '\\';
    out[out_pos++] = (char)('0' + ((ch >> 6) & 7u));
    out[out_pos++] = (char)('0' + ((ch >> 3) & 7u));
    out[out_pos++] = (char)('0' + (ch & 7u));
    return out_pos;
}

static bool bx_output_quote_is_nongraphic(unsigned char ch) {
    return isprint((int)ch) == 0;
}

static size_t bx_output_quote_append_escape_char(
    char* out,
    size_t out_pos,
    unsigned char ch,
    bool escape_space,
    bool escape_double_quote,
    bool escape_single_quote) {
    switch (ch) {
        case ' ':
            if (escape_space) {
                out[out_pos++] = '\\';
            }
            out[out_pos++] = ' ';
            break;
        case '\\':
            out[out_pos++] = '\\';
            out[out_pos++] = '\\';
            break;
        case '"':
            if (escape_double_quote) {
                out[out_pos++] = '\\';
            }
            out[out_pos++] = '"';
            break;
        case '\'':
            if (escape_single_quote) {
                out[out_pos++] = '\\';
            }
            out[out_pos++] = '\'';
            break;
        case '\a':
            out[out_pos++] = '\\';
            out[out_pos++] = 'a';
            break;
        case '\b':
            out[out_pos++] = '\\';
            out[out_pos++] = 'b';
            break;
        case '\f':
            out[out_pos++] = '\\';
            out[out_pos++] = 'f';
            break;
        case '\n':
            out[out_pos++] = '\\';
            out[out_pos++] = 'n';
            break;
        case '\r':
            out[out_pos++] = '\\';
            out[out_pos++] = 'r';
            break;
        case '\t':
            out[out_pos++] = '\\';
            out[out_pos++] = 't';
            break;
        case '\v':
            out[out_pos++] = '\\';
            out[out_pos++] = 'v';
            break;
        default:
            if (!bx_output_quote_is_nongraphic(ch)) {
                out[out_pos++] = (char)ch;
            }
            else {
                out_pos = bx_output_quote_append_octal(out, out_pos, ch);
            }
            break;
    }

    return out_pos;
}

static char* bx_output_quote_literal_dup(const char* text, bool hide_nongraphic) {
    size_t len = strlen(text);
    char* out = bx_output_quote_alloc(len + 1u);
    size_t out_pos = 0u;

    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)text[i];
        out[out_pos++] = (hide_nongraphic && bx_output_quote_is_nongraphic(ch)) ? '?' : (char)ch;
    }

    out[out_pos] = '\0';
    return out;
}

static char* bx_output_quote_escape_dup(const char* text) {
    size_t len = strlen(text);
    char* out = bx_output_quote_alloc(bx_output_quote_capacity(len, 4u, 1u));
    size_t out_pos = 0u;

    for (size_t i = 0; i < len; i++) {
        out_pos = bx_output_quote_append_escape_char(out, out_pos, (unsigned char)text[i], true, false, false);
    }

    out[out_pos] = '\0';
    return out;
}

static char* bx_output_quote_c_dup(const char* text) {
    size_t len = strlen(text);
    char* out = bx_output_quote_alloc(bx_output_quote_capacity(len, 4u, 3u));
    size_t out_pos = 0u;

    out[out_pos++] = '"';
    for (size_t i = 0; i < len; i++) {
        out_pos = bx_output_quote_append_escape_char(out, out_pos, (unsigned char)text[i], false, true, false);
    }
    out[out_pos++] = '"';
    out[out_pos] = '\0';
    return out;
}

static char* bx_output_quote_locale_dup(const char* text) {
    size_t len = strlen(text);
    char* out = bx_output_quote_alloc(bx_output_quote_capacity(len, 4u, 3u));
    size_t out_pos = 0u;

    out[out_pos++] = '\'';
    for (size_t i = 0; i < len; i++) {
        out_pos = bx_output_quote_append_escape_char(out, out_pos, (unsigned char)text[i], false, false, true);
    }
    out[out_pos++] = '\'';
    out[out_pos] = '\0';
    return out;
}

static char* bx_output_quote_single_backslash_dup(const char* text) {
    size_t len = strlen(text);
    char* out = bx_output_quote_alloc(bx_output_quote_capacity(len, 2u, 3u));
    size_t out_pos = 0u;

    out[out_pos++] = '\'';
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)text[i];
        if (ch == '\'' || ch == '\\') {
            out[out_pos++] = '\\';
        }
        out[out_pos++] = (char)ch;
    }
    out[out_pos++] = '\'';
    out[out_pos] = '\0';
    return out;
}

static bool bx_output_quote_shell_char_is_safe(unsigned char ch) {
    return isalnum((int)ch) != 0
        || ch == '-'
        || ch == '_'
        || ch == '.'
        || ch == '/'
        || ch == '~';
}

static bool bx_output_quote_has_nongraphic(const char* text) {
    for (size_t i = 0; text[i] != '\0'; i++) {
        if (bx_output_quote_is_nongraphic((unsigned char)text[i])) {
            return true;
        }
    }

    return false;
}

static size_t bx_output_quote_append_shell_double_quoted_segment(
    char* out,
    size_t out_pos,
    const char* text,
    size_t len) {
    out[out_pos++] = '"';
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)text[i];
        if (ch == '"' || ch == '\\' || ch == '$' || ch == '`') {
            out[out_pos++] = '\\';
        }
        out[out_pos++] = (char)ch;
    }
    out[out_pos++] = '"';
    return out_pos;
}

static size_t bx_output_quote_append_shell_single_quoted_segment(
    char* out,
    size_t out_pos,
    const char* text,
    size_t len) {
    out[out_pos++] = '\'';
    memcpy(out + out_pos, text, len);
    out_pos += len;
    out[out_pos++] = '\'';
    return out_pos;
}

static size_t bx_output_quote_append_shell_quoted_segment(
    char* out,
    size_t out_pos,
    const char* text,
    size_t len,
    bool always_quote) {
    bool needs_quotes = always_quote;

    if (!needs_quotes) {
        for (size_t i = 0; i < len; i++) {
            if (!bx_output_quote_shell_char_is_safe((unsigned char)text[i])) {
                needs_quotes = true;
                break;
            }
        }
    }

    if (!needs_quotes) {
        memcpy(out + out_pos, text, len);
        out_pos += len;
        return out_pos;
    }

    if (memchr(text, '\'', len) == NULL) {
        return bx_output_quote_append_shell_single_quoted_segment(out, out_pos, text, len);
    }

    return bx_output_quote_append_shell_double_quoted_segment(out, out_pos, text, len);
}

static char* bx_output_quote_shell_dup(const char* text, bool always_quote) {
    size_t len = strlen(text);
    char* out = bx_output_quote_alloc(bx_output_quote_capacity(len, 4u, 3u));
    size_t out_pos = 0u;

    out_pos = bx_output_quote_append_shell_quoted_segment(out, out_pos, text, len, always_quote);
    out[out_pos] = '\0';
    return out;
}

static size_t bx_output_quote_append_shell_escape_fragment(char* out, size_t out_pos, unsigned char ch) {
    memcpy(out + out_pos, "$'", 2u);
    out_pos += 2u;
    out_pos = bx_output_quote_append_escape_char(out, out_pos, ch, false, false, false);
    out[out_pos++] = '\'';
    return out_pos;
}

static char* bx_output_quote_shell_escape_dup(const char* text, bool always_quote) {
    if (!bx_output_quote_has_nongraphic(text)) {
        return bx_output_quote_shell_dup(text, always_quote);
    }

    size_t len = strlen(text);
    char* out = bx_output_quote_alloc(bx_output_quote_capacity(len, 8u, 8u));
    size_t out_pos = 0u;
    size_t segment_start = 0u;

    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)text[i];
        if (!bx_output_quote_is_nongraphic(ch)) {
            continue;
        }

        if (i > segment_start) {
            out_pos = bx_output_quote_append_shell_quoted_segment(out, out_pos, text + segment_start, i - segment_start, true);
        }
        out_pos = bx_output_quote_append_shell_escape_fragment(out, out_pos, ch);
        segment_start = i + 1u;
    }

    if (segment_start < len) {
        out_pos = bx_output_quote_append_shell_quoted_segment(out, out_pos, text + segment_start, len - segment_start, true);
    }

    out[out_pos] = '\0';
    return out;
}

static bool bx_output_quote_reusable_shell_safe_char(unsigned char ch) {
    if (isalnum((int)ch)) {
        return true;
    }

    switch (ch) {
        case '%':
        case '+':
        case ',':
        case '-':
        case '.':
        case '/':
        case ':':
        case '@':
        case ']':
        case '_':
            return true;
        default:
            return false;
    }
}

static bool bx_output_quote_reusable_shell_safe_string(const char* text) {
    if (text == NULL || text[0] == '\0') {
        return false;
    }

    size_t text_len = strlen(text);
    const unsigned char* p = (const unsigned char*)text;
    size_t index = 0u;
    while (*p != '\0') {
        unsigned char ch = *p;
        if ((ch == '~' || ch == '#') && index > 0u) {
            p++;
            index++;
            continue;
        }
        if ((ch == '{' || ch == '}') && text_len > 1u) {
            p++;
            index++;
            continue;
        }
        if (!bx_output_quote_reusable_shell_safe_char(ch)) {
            return false;
        }
        p++;
        index++;
    }

    return true;
}

static bool bx_output_quote_reusable_shell_double_quote_char(unsigned char ch) {
    return ch == ' ' || ch == '\'' || bx_output_quote_reusable_shell_safe_char(ch);
}

static bool bx_output_quote_reusable_shell_can_use_double_quotes(const char* text) {
    bool has_single_quote = false;
    const unsigned char* p = (const unsigned char*)text;
    size_t index = 0u;

    while (*p != '\0') {
        unsigned char ch = *p++;
        if (isprint((int)ch) == 0) {
            return false;
        }
        if (ch == '\'') {
            has_single_quote = true;
        }
        if ((ch == '#' || ch == '~') && index == 0u) {
            index++;
            continue;
        }
        if (!bx_output_quote_reusable_shell_double_quote_char(ch)) {
            return false;
        }
        index++;
    }

    return has_single_quote;
}

static bool bx_output_quote_append_text(char* out, size_t out_cap, size_t* out_pos, const char* text) {
    size_t len = strlen(text);
    if (*out_pos > out_cap || len >= out_cap - *out_pos) {
        return false;
    }
    memcpy(out + *out_pos, text, len);
    *out_pos += len;
    out[*out_pos] = '\0';
    return true;
}

static bool bx_output_quote_append_char(char* out, size_t out_cap, size_t* out_pos, char ch) {
    if (*out_pos + 1u >= out_cap) {
        return false;
    }
    out[(*out_pos)++] = ch;
    out[*out_pos] = '\0';
    return true;
}

static bool bx_output_quote_append_reusable_shell_dollar_escape(char* out, size_t out_cap, size_t* out_pos, unsigned char ch) {
    switch (ch) {
        case '\a':
            return bx_output_quote_append_text(out, out_cap, out_pos, "\\a");
        case '\b':
            return bx_output_quote_append_text(out, out_cap, out_pos, "\\b");
        case '\t':
            return bx_output_quote_append_text(out, out_cap, out_pos, "\\t");
        case '\n':
            return bx_output_quote_append_text(out, out_cap, out_pos, "\\n");
        case '\v':
            return bx_output_quote_append_text(out, out_cap, out_pos, "\\v");
        case '\f':
            return bx_output_quote_append_text(out, out_cap, out_pos, "\\f");
        case '\r':
            return bx_output_quote_append_text(out, out_cap, out_pos, "\\r");
        default: {
            char octal_escape[5];
            int written = snprintf(octal_escape, sizeof(octal_escape), "\\%03o", (unsigned int)ch);
            if (written < 0 || (size_t)written >= sizeof(octal_escape)) {
                return false;
            }
            return bx_output_quote_append_text(out, out_cap, out_pos, octal_escape);
        }
    }
}

static char* bx_output_quote_shell_reusable_alloc(size_t size, bool fatal_alloc) {
    return fatal_alloc ? bx_output_quote_alloc(size) : bx_output_quote_try_alloc(size);
}

static char* bx_output_quote_shell_reusable_strdup(const char* text, bool fatal_alloc) {
    return fatal_alloc ? bx_output_quote_strdup(text) : bx_output_quote_try_strdup(text);
}

static bool bx_output_quote_shell_reusable_capacity(
    size_t len,
    size_t multiplier,
    size_t extra,
    bool fatal_alloc,
    size_t* capacity_out) {
    if (bx_output_quote_capacity_checked(len, multiplier, extra, capacity_out)) {
        return true;
    }
    if (fatal_alloc) {
        bx_fatal(3, "path quote allocation overflow");
    }
    return false;
}

static bool bx_output_quote_shell_reusable_append_or_fail(char* out, bool fatal_alloc, bool ok) {
    if (ok) {
        return true;
    }
    if (fatal_alloc) {
        bx_fatal(3, "output quote allocation overflow");
    }
    free(out);
    return false;
}

static char* bx_output_quote_shell_reusable_dup_impl(const char* text, bool fatal_alloc) {
    enum bx_output_quote_reusable_shell_mode {
        BX_OUTPUT_QUOTE_REUSABLE_SHELL_NONE = 0,
        BX_OUTPUT_QUOTE_REUSABLE_SHELL_SINGLE,
        BX_OUTPUT_QUOTE_REUSABLE_SHELL_DOLLAR,
    };

    if (text == NULL) {
        text = "";
    }

    if (text[0] == '\0') {
        return bx_output_quote_shell_reusable_strdup("''", fatal_alloc);
    }

    if (bx_output_quote_reusable_shell_safe_string(text)) {
        return bx_output_quote_shell_reusable_strdup(text, fatal_alloc);
    }

    size_t text_len = strlen(text);
    if (bx_output_quote_reusable_shell_can_use_double_quotes(text)) {
        size_t out_cap = 0u;
        if (!bx_output_quote_shell_reusable_capacity(text_len, 1u, 3u, fatal_alloc, &out_cap)) {
            return NULL;
        }
        char* out = bx_output_quote_shell_reusable_alloc(out_cap, fatal_alloc);
        if (out == NULL) {
            return NULL;
        }
        size_t out_pos = 0u;
        out[0] = '\0';

        if (!bx_output_quote_shell_reusable_append_or_fail(out, fatal_alloc, bx_output_quote_append_char(out, out_cap, &out_pos, '"')) ||
            !bx_output_quote_shell_reusable_append_or_fail(out, fatal_alloc, bx_output_quote_append_text(out, out_cap, &out_pos, text)) ||
            !bx_output_quote_shell_reusable_append_or_fail(out, fatal_alloc, bx_output_quote_append_char(out, out_cap, &out_pos, '"'))) {
            return NULL;
        }
        return out;
    }

    size_t out_cap = 0u;
    if (!bx_output_quote_shell_reusable_capacity(text_len, 12u, 8u, fatal_alloc, &out_cap)) {
        return NULL;
    }
    char* out = bx_output_quote_shell_reusable_alloc(out_cap, fatal_alloc);
    if (out == NULL) {
        return NULL;
    }
    size_t out_pos = 0u;
    enum bx_output_quote_reusable_shell_mode mode = BX_OUTPUT_QUOTE_REUSABLE_SHELL_NONE;
    bool emitted = false;

    out[0] = '\0';
    for (size_t i = 0; i < text_len; i++) {
        unsigned char ch = (unsigned char)text[i];
        if (isprint((int)ch) == 0) {
            if (mode == BX_OUTPUT_QUOTE_REUSABLE_SHELL_SINGLE) {
                if (!bx_output_quote_shell_reusable_append_or_fail(out, fatal_alloc, bx_output_quote_append_char(out, out_cap, &out_pos, '\''))) {
                    return NULL;
                }
                mode = BX_OUTPUT_QUOTE_REUSABLE_SHELL_NONE;
            }
            if (mode != BX_OUTPUT_QUOTE_REUSABLE_SHELL_DOLLAR) {
                if (!emitted &&
                    !bx_output_quote_shell_reusable_append_or_fail(out, fatal_alloc, bx_output_quote_append_text(out, out_cap, &out_pos, "''"))) {
                    return NULL;
                }
                emitted = true;
                if (!bx_output_quote_shell_reusable_append_or_fail(out, fatal_alloc, bx_output_quote_append_text(out, out_cap, &out_pos, "$'"))) {
                    return NULL;
                }
                mode = BX_OUTPUT_QUOTE_REUSABLE_SHELL_DOLLAR;
            }
            if (!bx_output_quote_shell_reusable_append_or_fail(out, fatal_alloc, bx_output_quote_append_reusable_shell_dollar_escape(out, out_cap, &out_pos, ch))) {
                return NULL;
            }
            emitted = true;
            continue;
        }

        if (mode == BX_OUTPUT_QUOTE_REUSABLE_SHELL_DOLLAR) {
            if (!bx_output_quote_shell_reusable_append_or_fail(out, fatal_alloc, bx_output_quote_append_char(out, out_cap, &out_pos, '\''))) {
                return NULL;
            }
            mode = BX_OUTPUT_QUOTE_REUSABLE_SHELL_NONE;
        }

        if (ch == '\'') {
            if (mode == BX_OUTPUT_QUOTE_REUSABLE_SHELL_SINGLE) {
                if (!bx_output_quote_shell_reusable_append_or_fail(out, fatal_alloc, bx_output_quote_append_char(out, out_cap, &out_pos, '\''))) {
                    return NULL;
                }
                mode = BX_OUTPUT_QUOTE_REUSABLE_SHELL_NONE;
            }
            if (!emitted &&
                !bx_output_quote_shell_reusable_append_or_fail(out, fatal_alloc, bx_output_quote_append_text(out, out_cap, &out_pos, "''"))) {
                return NULL;
            }
            emitted = true;
            if (!bx_output_quote_shell_reusable_append_or_fail(out, fatal_alloc, bx_output_quote_append_text(out, out_cap, &out_pos, "\\'"))) {
                return NULL;
            }
            bool next_is_printable_non_quote = false;
            if (i + 1u < text_len) {
                unsigned char next = (unsigned char)text[i + 1u];
                next_is_printable_non_quote = (next != '\'') && (isprint((int)next) != 0);
            }
            if (!next_is_printable_non_quote &&
                !bx_output_quote_shell_reusable_append_or_fail(out, fatal_alloc, bx_output_quote_append_text(out, out_cap, &out_pos, "''"))) {
                return NULL;
            }
            emitted = true;
            continue;
        }

        if (mode != BX_OUTPUT_QUOTE_REUSABLE_SHELL_SINGLE) {
            if (!bx_output_quote_shell_reusable_append_or_fail(out, fatal_alloc, bx_output_quote_append_char(out, out_cap, &out_pos, '\''))) {
                return NULL;
            }
            mode = BX_OUTPUT_QUOTE_REUSABLE_SHELL_SINGLE;
            emitted = true;
        }

        if (!bx_output_quote_shell_reusable_append_or_fail(out, fatal_alloc, bx_output_quote_append_char(out, out_cap, &out_pos, (char)ch))) {
            return NULL;
        }
        emitted = true;
    }

    if (mode == BX_OUTPUT_QUOTE_REUSABLE_SHELL_SINGLE || mode == BX_OUTPUT_QUOTE_REUSABLE_SHELL_DOLLAR) {
        if (!bx_output_quote_shell_reusable_append_or_fail(out, fatal_alloc, bx_output_quote_append_char(out, out_cap, &out_pos, '\''))) {
            return NULL;
        }
    }

    if (!emitted &&
        !bx_output_quote_shell_reusable_append_or_fail(out, fatal_alloc, bx_output_quote_append_text(out, out_cap, &out_pos, "''"))) {
        return NULL;
    }

    return out;
}

char* bx_output_quote_shell_reusable_dup(const char* text) {
    return bx_output_quote_shell_reusable_dup_impl(text, true);
}

char* bx_output_quote_shell_reusable_try_dup(const char* text) {
    return bx_output_quote_shell_reusable_dup_impl(text, false);
}

char* bx_output_quote_dup(const char* text, enum bx_output_quote_style style) {
    switch (style) {
        case BX_OUTPUT_QUOTE_LITERAL:
            return bx_output_quote_literal_dup(text, false);
        case BX_OUTPUT_QUOTE_LITERAL_HIDE_NONGRAPHIC:
            return bx_output_quote_literal_dup(text, true);
        case BX_OUTPUT_QUOTE_ESCAPE:
            return bx_output_quote_escape_dup(text);
        case BX_OUTPUT_QUOTE_C:
            return bx_output_quote_c_dup(text);
        case BX_OUTPUT_QUOTE_LOCALE:
            return bx_output_quote_locale_dup(text);
        case BX_OUTPUT_QUOTE_SHELL:
            return bx_output_quote_shell_dup(text, false);
        case BX_OUTPUT_QUOTE_SHELL_ALWAYS:
            return bx_output_quote_shell_dup(text, true);
        case BX_OUTPUT_QUOTE_SHELL_ESCAPE:
            return bx_output_quote_shell_escape_dup(text, false);
        case BX_OUTPUT_QUOTE_SHELL_ESCAPE_ALWAYS:
            return bx_output_quote_shell_escape_dup(text, true);
        case BX_OUTPUT_QUOTE_SHELL_REUSABLE:
            return bx_output_quote_shell_reusable_dup(text);
        case BX_OUTPUT_QUOTE_SINGLE_BACKSLASH:
            return bx_output_quote_single_backslash_dup(text);
        default:
            return bx_output_quote_literal_dup(text, false);
    }
}

char* bx_output_quote_control_dup(const char* text, const struct bx_output_control_quote_options* options) {
    enum bx_output_control_quote_style style = BX_OUTPUT_CONTROL_QUOTE_LITERAL;
    bool high_bit_printable = false;

    if (options != NULL) {
        style = options->style;
        high_bit_printable = options->high_bit_printable;
    }

    size_t len = strlen(text);
    char* out = bx_output_quote_alloc(bx_output_quote_capacity(len, 2u, 1u));
    size_t out_pos = 0u;

    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)text[i];
        bool printable = isprint((int)ch) != 0 || (high_bit_printable && ch >= 0x80u);

        if (style == BX_OUTPUT_CONTROL_QUOTE_LITERAL || printable) {
            out[out_pos++] = (char)ch;
            continue;
        }
        if (style == BX_OUTPUT_CONTROL_QUOTE_QUESTION) {
            out[out_pos++] = '?';
            continue;
        }

        out[out_pos++] = '^';
        out[out_pos++] = (ch == 127u) ? '?' : (char)(ch ^ 0x40u);
    }

    out[out_pos] = '\0';
    return out;
}
