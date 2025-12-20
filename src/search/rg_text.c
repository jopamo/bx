#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <iconv.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

static bool bx_rg_encoding_is_utf8(const char *name) {
    return bx_rg_encoding_is_alias(name, "utf8") ||
           bx_rg_encoding_is_alias(name, "utf-8");
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

static bool bx_rg_decode_with_iconv(const char *encoding_name,
                                    const unsigned char *input,
                                    size_t input_len,
                                    unsigned char **output,
                                    size_t *output_len) {
    iconv_t cd;
    size_t cap;
    unsigned char *buf;
    unsigned char *outptr;
    size_t outleft;
    char *inptr;
    size_t inleft;

    if (!encoding_name || !output || !output_len)
        return false;

    cd = iconv_open("UTF-8", encoding_name);
    if (cd == (iconv_t)-1)
        return false;

    cap = input_len * 4u + 16u;
    buf = malloc(cap);
    if (!buf) {
        iconv_close(cd);
        return false;
    }

    outptr = buf;
    outleft = cap;
    inptr = (char *)(uintptr_t)input;
    inleft = input_len;

    while (inleft > 0u) {
        size_t rc = iconv(cd, &inptr, &inleft, (char **)&outptr, &outleft);
        if (rc != (size_t)-1)
            continue;
        if (errno == E2BIG) {
            size_t used = (size_t)(outptr - buf);
            unsigned char *grown;
            cap *= 2u;
            grown = realloc(buf, cap);
            if (!grown) {
                free(buf);
                iconv_close(cd);
                return false;
            }
            buf = grown;
            outptr = buf + used;
            outleft = cap - used;
            continue;
        }
        if (errno == EILSEQ || errno == EINVAL) {
            static const unsigned char replacement[] = {0xEFu, 0xBFu, 0xBDu};
            if (outleft < sizeof(replacement)) {
                size_t used = (size_t)(outptr - buf);
                unsigned char *grown;
                cap *= 2u;
                grown = realloc(buf, cap);
                if (!grown) {
                    free(buf);
                    iconv_close(cd);
                    return false;
                }
                buf = grown;
                outptr = buf + used;
                outleft = cap - used;
            }
            memcpy(outptr, replacement, sizeof(replacement));
            outptr += sizeof(replacement);
            outleft -= sizeof(replacement);
            inptr++;
            inleft--;
            continue;
        }
        free(buf);
        iconv_close(cd);
        return false;
    }

    *output_len = (size_t)(outptr - buf);
    unsigned char *grown = realloc(buf, *output_len + 1u);
    *output = grown ? grown : buf;
    (*output)[*output_len] = '\0';
    iconv_close(cd);
    return true;
}

bool bx_rg_decode_buffer(enum bx_rg_encoding_mode mode,
                         const char *encoding_name,
                         const unsigned char *input,
                         size_t input_len,
                         unsigned char **output,
                         size_t *output_len) {
    const unsigned char *body = input;
    size_t body_len = input_len;
    const char *effective = encoding_name;

    if (!output || !output_len)
        return false;
    *output = NULL;
    *output_len = 0u;

    if (!input) {
        *output = malloc(1u);
        if (!*output)
            return false;
        (*output)[0] = '\0';
        return true;
    }

    if (mode == BX_RG_ENCODING_NONE) {
        *output = malloc(input_len + 1u);
        if (!*output)
            return false;
        memcpy(*output, input, input_len);
        (*output)[input_len] = '\0';
        *output_len = input_len;
        return true;
    }

    if (body_len >= 3u && body[0] == 0xEFu && body[1] == 0xBBu && body[2] == 0xBFu) {
        body += 3u;
        body_len -= 3u;
        if (mode == BX_RG_ENCODING_AUTO || bx_rg_encoding_is_utf8(effective))
            effective = "UTF-8";
    } else if (body_len >= 2u && body[0] == 0xFFu && body[1] == 0xFEu) {
        body += 2u;
        body_len -= 2u;
        if (mode == BX_RG_ENCODING_AUTO)
            effective = "UTF-16LE";
    } else if (body_len >= 2u && body[0] == 0xFEu && body[1] == 0xFFu) {
        body += 2u;
        body_len -= 2u;
        if (mode == BX_RG_ENCODING_AUTO)
            effective = "UTF-16BE";
    } else if (mode == BX_RG_ENCODING_AUTO) {
        *output = malloc(body_len + 1u);
        if (!*output)
            return false;
        memcpy(*output, body, body_len);
        (*output)[body_len] = '\0';
        *output_len = body_len;
        return true;
    }

    if (!effective || bx_rg_encoding_is_utf8(effective)) {
        *output = malloc(body_len + 1u);
        if (!*output)
            return false;
        memcpy(*output, body, body_len);
        (*output)[body_len] = '\0';
        *output_len = body_len;
        return true;
    }

    return bx_rg_decode_with_iconv(effective, body, body_len, output, output_len);
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

static bool bx_rg_decode_utf8_codepoint(const unsigned char *buf, size_t len,
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

static bool bx_rg_decode_prev_utf8(const unsigned char *buf, size_t end, uint32_t *cp_out) {
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

static bool bx_rg_decode_next_utf8(const unsigned char *buf, size_t len, size_t start,
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
