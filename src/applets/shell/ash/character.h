#ifndef BX_APPLETS_SHELL_ASH_CHARACTER_H
#define BX_APPLETS_SHELL_ASH_CHARACTER_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <wchar.h>

static inline bool ash_character_is_scalar(uint32_t value) {
    return value <= 0x10ffffu && !(value >= 0xd800u && value <= 0xdfffu);
}

static inline size_t ash_character_decode(const char* text, size_t length, mbstate_t* state, wint_t* character) {
    wchar_t decoded;
    size_t width = mbrtowc(&decoded, text, length, state);
    if (width == (size_t)-1 || width == (size_t)-2 || !ash_character_is_scalar((uint32_t)decoded)) {
        /* Malformed input consumes one byte and resets conversion. */
        *state = (mbstate_t){0};
        *character = WEOF;
        return 1u;
    }
    *character = (wint_t)decoded;
    return width;
}

static inline size_t ash_character_width(const char* value, size_t remaining, mbstate_t* state) {
    if (MB_CUR_MAX == 1u)
        return 1u;
    wint_t character;
    size_t width = ash_character_decode(value, remaining, state, &character);
    return width == 0u ? 1u : width;
}

#endif /* BX_APPLETS_SHELL_ASH_CHARACTER_H */
