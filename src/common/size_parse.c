#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>

#include "common/size_parse.h"

static bool bx_dd_scale_binary(uintmax_t value, unsigned int power, uintmax_t* value_out) {
    uintmax_t scaled = value;

    for (unsigned int i = 0; i < power; i++) {
        if (scaled > UINTMAX_MAX / 1024u) {
            return false;
        }
        scaled *= 1024u;
    }

    *value_out = scaled;
    return true;
}

bool bx_dd_parse_u64(const char* text, uintmax_t* value_out) {
    if (text == NULL || text[0] == '\0' || value_out == NULL) {
        return false;
    }

    if (text[0] == '-') {
        return false;
    }

    errno = 0;
    char* end = NULL;
    uintmax_t parsed = strtoumax(text, &end, 10);
    if (errno == ERANGE || end == text || end == NULL || end[0] != '\0') {
        return false;
    }

    *value_out = parsed;
    return true;
}

bool bx_dd_parse_size(const char* text, uintmax_t* value_out) {
    if (text == NULL || text[0] == '\0' || value_out == NULL) {
        return false;
    }

    if (text[0] == '-') {
        return false;
    }

    errno = 0;
    char* end = NULL;
    uintmax_t parsed = strtoumax(text, &end, 10);
    if (errno == ERANGE || end == text || end == NULL) {
        return false;
    }

    if (end[0] == '\0') {
        *value_out = parsed;
        return true;
    }

    if (end[1] != '\0') {
        return false;
    }

    switch (end[0]) {
        case 'c':
        case 'C':
            *value_out = parsed;
            return true;
        case 'w':
        case 'W':
            if (parsed > UINTMAX_MAX / 2u) {
                return false;
            }
            *value_out = parsed * 2u;
            return true;
        case 'b':
            if (parsed > UINTMAX_MAX / 512u) {
                return false;
            }
            *value_out = parsed * 512u;
            return true;
        case 'k':
        case 'K':
            return bx_dd_scale_binary(parsed, 1u, value_out);
        case 'm':
        case 'M':
            return bx_dd_scale_binary(parsed, 2u, value_out);
        case 'g':
        case 'G':
            return bx_dd_scale_binary(parsed, 3u, value_out);
        case 't':
        case 'T':
            return bx_dd_scale_binary(parsed, 4u, value_out);
        default:
            return false;
    }
}
