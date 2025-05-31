#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "common/size_parse.h"

static bool bx_size_safe_mul(uintmax_t a, uintmax_t b, uintmax_t* out) {
    if (out == NULL) {
        return false;
    }

    if (a != 0 && b > UINTMAX_MAX / a) {
        return false;
    }

    *out = a * b;
    return true;
}

static bool bx_size_pow(uintmax_t base, unsigned int power, uintmax_t* out) {
    uintmax_t value = 1;

    for (unsigned int i = 0; i < power; i++) {
        if (!bx_size_safe_mul(value, base, &value)) {
            return false;
        }
    }

    *out = value;
    return true;
}

static bool bx_size_suffix_multiplier(const char* suffix, uintmax_t* multiplier_out) {
    if (suffix == NULL || multiplier_out == NULL) {
        return false;
    }

    if (suffix[0] == '\0' || strcmp(suffix, "c") == 0 || strcmp(suffix, "C") == 0 || strcmp(suffix, "B") == 0) {
        *multiplier_out = 1;
        return true;
    }

    if (strcmp(suffix, "w") == 0 || strcmp(suffix, "W") == 0) {
        *multiplier_out = 2;
        return true;
    }

    if (strcmp(suffix, "b") == 0) {
        *multiplier_out = 512;
        return true;
    }

    if (strcmp(suffix, "k") == 0 || strcmp(suffix, "K") == 0 || strcmp(suffix, "KiB") == 0 || strcmp(suffix, "kiB") == 0) {
        return bx_size_pow(1024, 1, multiplier_out);
    }

    if (strcmp(suffix, "kB") == 0 || strcmp(suffix, "KB") == 0) {
        return bx_size_pow(1000, 1, multiplier_out);
    }

    static const char prefixes[] = "MGTPEZYRQ";
    const char* prefix = strchr(prefixes, suffix[0]);
    if (prefix == NULL) {
        return false;
    }

    unsigned int power = (unsigned int)(prefix - prefixes) + 2;

    if (suffix[1] == '\0') {
        return bx_size_pow(1024, power, multiplier_out);
    }

    if (suffix[1] == 'B' && suffix[2] == '\0') {
        return bx_size_pow(1000, power, multiplier_out);
    }

    if (suffix[1] == 'i' && suffix[2] == 'B' && suffix[3] == '\0') {
        return bx_size_pow(1024, power, multiplier_out);
    }

    return false;
}

static bool bx_size_parse_factor(const char* text, size_t len, uintmax_t* value_out) {
    if (text == NULL || len == 0 || value_out == NULL) {
        return false;
    }

    size_t pos = 0;
    uintmax_t value = 0;
    while (pos < len && text[pos] >= '0' && text[pos] <= '9') {
        unsigned int digit = (unsigned int)(text[pos] - '0');
        if (value > (UINTMAX_MAX - digit) / 10) {
            return false;
        }
        value = value * 10 + digit;
        pos++;
    }

    if (pos == 0) {
        return false;
    }

    char suffix[4];
    size_t suffix_len = len - pos;
    if (suffix_len >= sizeof(suffix)) {
        return false;
    }

    memcpy(suffix, text + pos, suffix_len);
    suffix[suffix_len] = '\0';

    uintmax_t multiplier = 0;
    if (!bx_size_suffix_multiplier(suffix, &multiplier)) {
        return false;
    }

    return bx_size_safe_mul(value, multiplier, value_out);
}

bool bx_dd_parse_u64(const char* text, uintmax_t* value_out) {
    if (text == NULL || text[0] == '\0' || value_out == NULL) {
        return false;
    }

    if (text[0] == '-') {
        return false;
    }

    return bx_size_parse_factor(text, strlen(text), value_out);
}

bool bx_dd_parse_size(const char* text, uintmax_t* value_out) {
    if (text == NULL || text[0] == '\0' || value_out == NULL) {
        return false;
    }

    if (text[0] == '-') {
        return false;
    }

    uintmax_t total = 1;
    const char* factor_start = text;
    const char* p = text;

    while (true) {
        if (*p == 'x' || *p == '\0') {
            uintmax_t factor = 0;
            if (!bx_size_parse_factor(factor_start, (size_t)(p - factor_start), &factor)) {
                return false;
            }
            if (!bx_size_safe_mul(total, factor, &total)) {
                return false;
            }
            if (*p == '\0') {
                break;
            }
            factor_start = p + 1;
        }
        p++;
    }

    *value_out = total;
    return true;
}
