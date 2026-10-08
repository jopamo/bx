#include <stdint.h>

#include "applets/shell/ash/quote.h"
#include "lib/text_buffer.h"

static int ash_hex_value(unsigned char character) {
    if (character >= '0' && character <= '9') {
        return (int)(character - '0');
    }
    if (character >= 'a' && character <= 'f') {
        return (int)(character - 'a') + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return (int)(character - 'A') + 10;
    }
    return -1;
}

static bool ash_append_ansi_codepoint(struct bx_text_buffer* output, uint32_t value) {
    size_t length;
    unsigned char lead;
    if (value <= 0x7fu) {
        return bx_text_buffer_append_char(output, (char)value);
    }
    if (value <= 0x7ffu) {
        length = 2u;
        lead = 0xc0u;
    }
    else if (value <= 0xffffu) {
        length = 3u;
        lead = 0xe0u;
    }
    else if (value <= 0x1fffffu) {
        length = 4u;
        lead = 0xf0u;
    }
    else if (value <= 0x3ffffffu) {
        length = 5u;
        lead = 0xf8u;
    }
    else if (value <= 0x7fffffffu) {
        length = 6u;
        lead = 0xfcu;
    }
    else {
        return bx_text_buffer_append_char(output, '\0');
    }

    char encoded[6];
    uint32_t remaining = value;
    for (size_t i = length - 1u; i != 0u; i--) {
        encoded[i] = (char)(0x80u | (remaining & 0x3fu));
        remaining >>= 6u;
    }
    encoded[0] = (char)(lead | remaining);
    return bx_text_buffer_append_span(output, encoded, length);
}

bool ash_quote_append_dollar_single(struct bx_text_buffer* output, const char* text, size_t length) {
    for (size_t i = 0u; i < length; i++) {
        unsigned char character = (unsigned char)text[i];
        if (character != '\\' || i + 1u == length) {
            if (!bx_text_buffer_append_char(output, (char)character)) {
                return false;
            }
            continue;
        }

        unsigned char escaped = (unsigned char)text[++i];
        char decoded;
        switch (escaped) {
            case 'a':
                decoded = '\a';
                break;
            case 'b':
                decoded = '\b';
                break;
            case 'e':
            case 'E':
                decoded = 0x1b;
                break;
            case 'f':
                decoded = '\f';
                break;
            case 'n':
                decoded = '\n';
                break;
            case 'r':
                decoded = '\r';
                break;
            case 't':
                decoded = '\t';
                break;
            case 'v':
                decoded = '\v';
                break;
            case '\\':
                decoded = '\\';
                break;
            case '\'':
                decoded = '\'';
                break;
            case '"':
                decoded = '"';
                break;
            case '?':
                decoded = '?';
                break;
            case '\n':
                if (!bx_text_buffer_append_char(output, '\\')) {
                    return false;
                }
                decoded = '\n';
                break;
            case 'c':
                if (i + 1u == length) {
                    if (!bx_text_buffer_append_char(output, '\\')) {
                        return false;
                    }
                    decoded = 'c';
                }
                else {
                    unsigned char controlled = (unsigned char)text[++i];
                    decoded = controlled == '?' ? 0x7f : (char)(controlled & 0x1fu);
                }
                break;
            case 'x': {
                int value = 0;
                size_t digits = 0u;
                while (digits < 2u && i + 1u < length) {
                    int digit = ash_hex_value((unsigned char)text[i + 1u]);
                    if (digit < 0) {
                        break;
                    }
                    value = value * 16 + digit;
                    i++;
                    digits++;
                }
                if (digits == 0u) {
                    if (!bx_text_buffer_append_char(output, '\\')) {
                        return false;
                    }
                    decoded = 'x';
                }
                else {
                    decoded = (char)(unsigned char)value;
                }
                break;
            }
            case 'u':
            case 'U': {
                size_t maximum = escaped == 'u' ? 4u : 8u;
                size_t digits = 0u;
                uint32_t value = 0u;
                while (digits < maximum && i + 1u < length) {
                    int digit = ash_hex_value((unsigned char)text[i + 1u]);
                    if (digit < 0) {
                        break;
                    }
                    value = value * 16u + (uint32_t)digit;
                    i++;
                    digits++;
                }
                if (digits == 0u) {
                    if (!bx_text_buffer_append_char(output, '\\')) {
                        return false;
                    }
                    decoded = (char)escaped;
                    break;
                }
                if (!ash_append_ansi_codepoint(output, value)) {
                    return false;
                }
                continue;
            }
            default:
                if (escaped >= '0' && escaped <= '7') {
                    unsigned int value = (unsigned int)(escaped - '0');
                    size_t digits = 1u;
                    while (digits < 3u && i + 1u < length && text[i + 1u] >= '0' && text[i + 1u] <= '7') {
                        value = value * 8u + (unsigned int)(text[++i] - '0');
                        digits++;
                    }
                    decoded = (char)(unsigned char)value;
                }
                else {
                    if (!bx_text_buffer_append_char(output, '\\')) {
                        return false;
                    }
                    decoded = (char)escaped;
                }
                break;
        }
        if (!bx_text_buffer_append_char(output, decoded)) {
            return false;
        }
    }
    return true;
}
