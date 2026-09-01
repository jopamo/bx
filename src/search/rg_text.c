#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <iconv.h>
#include <langinfo.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include "rg_text.h"

static bool bx_rg_encoding_is_alias(const char *name, const char *alias) {
    if (!name || !alias)
        return false;
    while (*name && *alias) {
        if (*name == '-' || *name == '_') {
            name++;
            continue;
        }
        if (*alias == '-' || *alias == '_') {
            alias++;
            continue;
        }
        if (tolower((unsigned char)*name) != tolower((unsigned char)*alias))
            return false;
        name++;
        alias++;
    }
    while (*name == '-' || *name == '_')
        name++;
    while (*alias == '-' || *alias == '_')
        alias++;
    return *name == '\0' && *alias == '\0';
}

bool bx_rg_encoding_is_utf8(const char *name) {
    return bx_rg_encoding_is_alias(name, "utf8") ||
           bx_rg_encoding_is_alias(name, "utf-8");
}

bool bx_rg_locale_is_utf8(void) {
    return bx_rg_encoding_is_utf8(nl_langinfo(CODESET));
}

uint32_t bx_rg_locale_uppercase_codepoint(uint32_t cp) {
    if (cp > (uint32_t)WINT_MAX)
        return cp;
    wint_t upper = towupper((wint_t)cp);
    if (upper == WEOF)
        return cp;
    return (uint32_t)upper;
}

bool bx_rg_parse_encoding_name(const char *progname, const char *name,
                               enum bx_rg_encoding_mode *mode_out,
                               char **encoding_name_out) {
    iconv_t cd;
    char *copy = NULL;

    if (!name || !mode_out || !encoding_name_out)
        return false;

    if (strcmp(name, "auto") == 0) {
        *mode_out = BX_RG_ENCODING_AUTO;
        free(*encoding_name_out);
        *encoding_name_out = NULL;
        return true;
    }
    if (strcmp(name, "none") == 0) {
        *mode_out = BX_RG_ENCODING_NONE;
        free(*encoding_name_out);
        *encoding_name_out = NULL;
        return true;
    }

    cd = iconv_open("UTF-8", name);
    if (cd == (iconv_t)-1) {
        fprintf(stderr,
                "%s: error parsing flag --encoding: grep config error: unknown encoding: %s\n",
                progname, name);
        return false;
    }
    iconv_close(cd);

    copy = strdup(name);
    if (!copy)
        return false;
    free(*encoding_name_out);
    *encoding_name_out = copy;
    *mode_out = BX_RG_ENCODING_EXPLICIT;
    return true;
}

size_t bx_rg_record_match_len(const unsigned char *buf, size_t len,
                              char delimiter, bool crlf_enabled) {
    if (len > 0u && buf[len - 1u] == (unsigned char)delimiter)
        len--;
    if (crlf_enabled && delimiter == '\n' && len > 0u && buf[len - 1u] == '\r')
        len--;
    return len;
}

size_t bx_rg_trim_leading_ascii_space(const unsigned char *buf, size_t len) {
    size_t i = 0u;
    while (i < len) {
        unsigned char c = buf[i];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\f' && c != '\v')
            break;
        i++;
    }
    return i;
}

static bool bx_rg_is_word_char_unicode(uint32_t cp) {
    if (cp == (uint32_t)'_')
        return true;
    if (cp < 0x80u)
        return isalnum((unsigned char)cp) != 0;
    return true;
}

static bool bx_rg_is_word_char_locale(uint32_t cp) {
    if (cp == (uint32_t)'_')
        return true;
    if (cp > (uint32_t)WINT_MAX)
        return false;
    return iswalnum((wint_t)cp) != 0;
}

bool bx_rg_decode_utf8_codepoint(const unsigned char *buf, size_t len,
                                 size_t *consumed_out, uint32_t *cp_out) {
    uint32_t cp;
    size_t consumed;

    if (!buf || len == 0u || !consumed_out || !cp_out)
        return false;
    if (buf[0] < 0x80u) {
        *consumed_out = 1u;
        *cp_out = buf[0];
        return true;
    }
    if ((buf[0] & 0xE0u) == 0xC0u) {
        if (len < 2u || (buf[1] & 0xC0u) != 0x80u)
            return false;
        cp = ((uint32_t)(buf[0] & 0x1Fu) << 6) |
             (uint32_t)(buf[1] & 0x3Fu);
        consumed = 2u;
    } else if ((buf[0] & 0xF0u) == 0xE0u) {
        if (len < 3u || (buf[1] & 0xC0u) != 0x80u || (buf[2] & 0xC0u) != 0x80u)
            return false;
        cp = ((uint32_t)(buf[0] & 0x0Fu) << 12) |
             ((uint32_t)(buf[1] & 0x3Fu) << 6) |
             (uint32_t)(buf[2] & 0x3Fu);
        consumed = 3u;
    } else if ((buf[0] & 0xF8u) == 0xF0u) {
        if (len < 4u || (buf[1] & 0xC0u) != 0x80u ||
            (buf[2] & 0xC0u) != 0x80u || (buf[3] & 0xC0u) != 0x80u) {
            return false;
        }
        cp = ((uint32_t)(buf[0] & 0x07u) << 18) |
             ((uint32_t)(buf[1] & 0x3Fu) << 12) |
             ((uint32_t)(buf[2] & 0x3Fu) << 6) |
             (uint32_t)(buf[3] & 0x3Fu);
        consumed = 4u;
    } else {
        return false;
    }
    *consumed_out = consumed;
    *cp_out = cp;
    return true;
}

bool bx_rg_decode_prev_utf8(const unsigned char *buf, size_t end, uint32_t *cp_out) {
    size_t start;
    size_t consumed = 0u;

    if (end == 0u || !cp_out)
        return false;
    start = end - 1u;
    while (start > 0u && (buf[start] & 0xC0u) == 0x80u)
        start--;
    if (!bx_rg_decode_utf8_codepoint(buf + start, end - start, &consumed, cp_out))
        return false;
    return start + consumed == end;
}

bool bx_rg_decode_next_utf8(const unsigned char *buf, size_t len, size_t start,
                            uint32_t *cp_out) {
    size_t consumed = 0u;

    if (start >= len || !cp_out)
        return false;
    return bx_rg_decode_utf8_codepoint(buf + start, len - start, &consumed, cp_out);
}

bool bx_rg_match_has_word_boundaries(const unsigned char *buf, size_t len,
                                     size_t start, size_t end,
                                     bool unicode_mode) {
    if (!buf)
        return false;

    if (!unicode_mode) {
        if (start > 0u) {
            unsigned char c = buf[start - 1u];
            if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                (c >= 'a' && c <= 'z') || c == '_') {
                return false;
            }
        }
        if (end < len) {
            unsigned char c = buf[end];
            if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                (c >= 'a' && c <= 'z') || c == '_') {
                return false;
            }
        }
        return true;
    }

    if (start > 0u) {
        uint32_t cp;
        if (bx_rg_decode_prev_utf8(buf, start, &cp) && bx_rg_is_word_char_unicode(cp))
            return false;
    }
    if (end < len) {
        uint32_t cp;
        if (bx_rg_decode_next_utf8(buf, len, end, &cp) && bx_rg_is_word_char_unicode(cp))
            return false;
    }
    return true;
}

bool bx_rg_match_has_locale_word_boundaries_utf8(const unsigned char *buf, size_t len,
                                                 size_t start, size_t end) {
    if (!buf)
        return false;

    if (start > 0u) {
        uint32_t cp;
        if (bx_rg_decode_prev_utf8(buf, start, &cp) && bx_rg_is_word_char_locale(cp))
            return false;
    }
    if (end < len) {
        uint32_t cp;
        if (bx_rg_decode_next_utf8(buf, len, end, &cp) && bx_rg_is_word_char_locale(cp))
            return false;
    }
    return true;
}
