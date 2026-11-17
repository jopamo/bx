#ifndef BX_APPLETS_SHELL_ASH_FIELD_SPLIT_H
#define BX_APPLETS_SHELL_ASH_FIELD_SPLIT_H

#include <string.h>

#include "applets/shell/ash/character.h"

static inline size_t ash_ifs_delimiter_length(const char* ifs, size_t ifs_length, const char* text, size_t remaining) {
    if (MB_CUR_MAX == 1u)
        return strchr(ifs, (unsigned char)text[0]) != NULL ? 1u : 0u;
    mbstate_t text_state = {0};
    size_t text_width = ash_character_width(text, remaining, &text_state);
    mbstate_t state = {0};
    for (size_t position = 0u; position < ifs_length;) {
        size_t width = ash_character_width(ifs + position, ifs_length - position, &state);
        if (width == text_width && memcmp(ifs + position, text, width) == 0)
            return width;
        position += width;
    }
    return 0u;
}

static inline bool ash_ifs_whitespace(const char* ifs, unsigned char character) {
    return (character == ' ' || character == '\t' || character == '\n') && strchr(ifs, (int)character) != NULL;
}

#endif
