#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "lib/size_parse.h"

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

bool bx_size_parse_uint(const char* text, uintmax_t* value_out) {
    if (text == NULL || text[0] == '\0' || value_out == NULL) {
        return false;
    }

    uintmax_t value = 0;
    for (size_t i = 0; text[i] != '\0'; i++) {
        unsigned char ch = (unsigned char)text[i];
        if (ch < '0' || ch > '9') {
            return false;
        }

        unsigned int digit = (unsigned int)(ch - '0');
        if (value > (UINTMAX_MAX - digit) / 10u) {
            return false;
        }

        value = (value * 10u) + (uintmax_t)digit;
    }

    *value_out = value;
    return true;
}

bool bx_size_parse_signed_count(const char* text, intmax_t* value_out) {
    if (text == NULL || text[0] == '\0' || value_out == NULL) {
        return false;
    }

    bool negative = false;
    const char* magnitude = text;
    if (magnitude[0] == '-') {
        negative = true;
        magnitude++;
    }
    else if (magnitude[0] == '+') {
        magnitude++;
    }

    if (magnitude[0] == '\0') {
        return false;
    }

    uintmax_t parsed = 0;
    if (!bx_size_parse_uint(magnitude, &parsed)) {
        return false;
    }

    if (negative) {
        uintmax_t negative_limit = (uintmax_t)INTMAX_MAX + 1u;
        if (parsed > negative_limit) {
            return false;
        }
        if (parsed == negative_limit) {
            *value_out = INTMAX_MIN;
            return true;
        }
        *value_out = -(intmax_t)parsed;
        return true;
    }

    if (parsed > (uintmax_t)INTMAX_MAX) {
        return false;
    }

    *value_out = (intmax_t)parsed;
    return true;
}

static bool bx_size_parse_scaled_uint_len(const char* text, size_t len, uintmax_t* value_out) {
    if (text == NULL || len == 0 || value_out == NULL) {
        return false;
    }

    size_t pos = 0;
    while (pos < len && text[pos] >= '0' && text[pos] <= '9') {
        pos++;
    }
    if (pos == 0) {
        return false;
    }

    char digits[64];
    if (pos >= sizeof(digits)) {
        return false;
    }
    memcpy(digits, text, pos);
    digits[pos] = '\0';

    char suffix[5];
    size_t suffix_len = len - pos;
    if (suffix_len >= sizeof(suffix)) {
        return false;
    }
    memcpy(suffix, text + pos, suffix_len);
    suffix[suffix_len] = '\0';

    uintmax_t value = 0;
    if (!bx_size_parse_uint(digits, &value)) {
        return false;
    }

    uintmax_t multiplier = 0;
    if (!bx_size_suffix_multiplier(suffix, &multiplier)) {
        return false;
    }

    return bx_size_safe_mul(value, multiplier, value_out);
}

bool bx_size_parse_scaled_uint(const char* text, uintmax_t* value_out) {
    if (text == NULL) {
        return false;
    }

    return bx_size_parse_scaled_uint_len(text, strlen(text), value_out);
}

static uintmax_t bx_size_saturating_mul(uintmax_t lhs, uintmax_t rhs) {
    if (lhs != 0u && rhs > UINTMAX_MAX / lhs) {
        return UINTMAX_MAX;
    }

    return lhs * rhs;
}

static uintmax_t bx_size_ceil_div(uintmax_t value, uintmax_t divisor) {
    if (divisor == 0u) {
        return value;
    }

    return (value / divisor) + ((value % divisor) != 0u ? 1u : 0u);
}

void bx_size_format_human_ceil(uintmax_t value, uintmax_t base, const char* suffixes, char* buffer, size_t buffer_size) {
    if (buffer == NULL || buffer_size == 0u) {
        return;
    }

    if (base < 2u || suffixes == NULL || suffixes[0] == '\0' || value < base) {
        (void)snprintf(buffer, buffer_size, "%" PRIuMAX, value);
        return;
    }

    size_t suffix_index = 0u;
    size_t suffix_count = strlen(suffixes);
    uintmax_t unit = 1u;

    while (suffix_index < suffix_count) {
        if (unit > UINTMAX_MAX / base) {
            break;
        }
        uintmax_t next_unit = unit * base;
        if (value < next_unit) {
            break;
        }
        unit = next_unit;
        suffix_index++;
    }

    if (suffix_index == 0u) {
        (void)snprintf(buffer, buffer_size, "%" PRIuMAX, value);
        return;
    }

    uintmax_t tenths = bx_size_ceil_div(bx_size_saturating_mul(value, 10u), unit);
    if (tenths < 100u) {
        uintmax_t whole = tenths / 10u;
        uintmax_t fractional = tenths % 10u;
        (void)snprintf(buffer, buffer_size, "%" PRIuMAX ".%" PRIuMAX "%c", whole, fractional, suffixes[suffix_index - 1u]);
    }
    else {
        uintmax_t whole = bx_size_ceil_div(value, unit);
        (void)snprintf(buffer, buffer_size, "%" PRIuMAX "%c", whole, suffixes[suffix_index - 1u]);
    }
}

void bx_size_format_human_round(uintmax_t value, uintmax_t base, const char* suffixes, bool include_base_suffix, char* buffer, size_t buffer_size) {
    if (buffer == NULL || buffer_size == 0u) {
        return;
    }

    if (base < 2u || suffixes == NULL || suffixes[0] == '\0') {
        (void)snprintf(buffer, buffer_size, "%" PRIuMAX, value);
        return;
    }

    if (value < base) {
        if (include_base_suffix) {
            (void)snprintf(buffer, buffer_size, "%" PRIuMAX "%c", value, suffixes[0]);
        }
        else {
            (void)snprintf(buffer, buffer_size, "%" PRIuMAX, value);
        }
        return;
    }

    double scaled = (double)value;
    size_t suffix_index = 0u;
    size_t suffix_count = strlen(suffixes);
    while (scaled >= (double)base && suffix_index + 1u < suffix_count) {
        scaled /= (double)base;
        suffix_index++;
    }

    if (scaled >= 10.0) {
        (void)snprintf(buffer, buffer_size, "%.0f%c", scaled, suffixes[suffix_index]);
    }
    else {
        (void)snprintf(buffer, buffer_size, "%.1f%c", scaled, suffixes[suffix_index]);
    }
}

void bx_size_format_decimal_rate(double bytes_per_sec, char* buffer, size_t buffer_size) {
    if (buffer == NULL || buffer_size == 0u) {
        return;
    }

    double value = bytes_per_sec;
    unsigned int power = 0u;
    while (value >= 999.5 && power < 6u) {
        value /= 1000.0;
        power++;
    }

    unsigned int label_power = (value == 0.0 && power == 0u) ? 1u : power;
    const char* label = bx_size_unit_label(BX_SIZE_UNIT_LABEL_SI_LOWER_K, label_power);
    if (label == NULL) {
        label = "";
    }

    if (value == 0.0) {
        (void)snprintf(buffer, buffer_size, "0.0 %sB/s", label);
        return;
    }

    char number[64];
    (void)snprintf(number, sizeof(number), "%.3g", value);
    (void)snprintf(buffer, buffer_size, "%s %sB/s", number, label);
}

bool bx_size_parse_scaled_count(const char* text, intmax_t* value_out) {
    if (text == NULL || text[0] == '\0' || value_out == NULL) {
        return false;
    }

    bool negative = false;
    const char* magnitude = text;
    if (magnitude[0] == '-') {
        negative = true;
        magnitude++;
    }
    else if (magnitude[0] == '+') {
        magnitude++;
    }

    if (magnitude[0] == '\0') {
        return false;
    }

    uintmax_t scaled = 0;
    if (!bx_size_parse_scaled_uint(magnitude, &scaled)) {
        return false;
    }

    if (negative) {
        uintmax_t negative_limit = (uintmax_t)INTMAX_MAX + 1u;
        if (scaled > negative_limit) {
            return false;
        }
        if (scaled == negative_limit) {
            *value_out = INTMAX_MIN;
            return true;
        }
        *value_out = -(intmax_t)scaled;
        return true;
    }

    if (scaled > (uintmax_t)INTMAX_MAX) {
        return false;
    }

    *value_out = (intmax_t)scaled;
    return true;
}

static enum bx_size_suffix_parse_result bx_size_pow_result(uintmax_t base, unsigned int power, uintmax_t* out) {
    uintmax_t value = 1;

    if (out == NULL) {
        return BX_SIZE_SUFFIX_PARSE_INVALID;
    }

    for (unsigned int i = 0; i < power; i++) {
        if (!bx_size_safe_mul(value, base, &value)) {
            return BX_SIZE_SUFFIX_PARSE_TOO_LARGE;
        }
    }

    *out = value;
    return BX_SIZE_SUFFIX_PARSE_OK;
}

bool bx_size_suffix_prefix_power(char suffix, unsigned int* power_out) {
    if (power_out == NULL) {
        return false;
    }

    if (suffix == 'k' || suffix == 'K') {
        *power_out = 1u;
        return true;
    }

    if (suffix == '\0') {
        return false;
    }

    static const char prefixes[] = "MGTPEZYRQ";
    const char* prefix = strchr(prefixes, suffix);
    if (prefix == NULL) {
        return false;
    }

    *power_out = (unsigned int)(prefix - prefixes) + 2u;
    return true;
}

const char* bx_size_unit_label(enum bx_size_unit_label_style style, unsigned int power) {
    static const char* const si_units[] = {"", "k", "M", "G", "T", "P", "E", "Z", "Y", "R", "Q"};
    static const char* const iec_units[] = {"", "K", "M", "G", "T", "P", "E", "Z", "Y", "R", "Q"};
    static const char* const iec_i_units[] = {"", "Ki", "Mi", "Gi", "Ti", "Pi", "Ei", "Zi", "Yi", "Ri", "Qi"};

    if (power >= sizeof(si_units) / sizeof(si_units[0])) {
        return NULL;
    }

    switch (style) {
        case BX_SIZE_UNIT_LABEL_SI_LOWER_K:
            return si_units[power];
        case BX_SIZE_UNIT_LABEL_IEC_PREFIX:
            return iec_units[power];
        case BX_SIZE_UNIT_LABEL_IEC_I_SUFFIX:
            return iec_i_units[power];
    }

    return NULL;
}

enum bx_size_suffix_parse_result bx_size_suffix_multiplier_result(const char* suffix, uintmax_t* multiplier_out) {
    if (suffix == NULL || multiplier_out == NULL) {
        return BX_SIZE_SUFFIX_PARSE_INVALID;
    }

    if (suffix[0] == 'x') {
        suffix++;
    }

    if (suffix[0] == '\0' || strcmp(suffix, "c") == 0 || strcmp(suffix, "B") == 0) {
        *multiplier_out = 1;
        return BX_SIZE_SUFFIX_PARSE_OK;
    }

    if (strcmp(suffix, "w") == 0) {
        *multiplier_out = 2;
        return BX_SIZE_SUFFIX_PARSE_OK;
    }

    if (strcmp(suffix, "b") == 0) {
        *multiplier_out = 512;
        return BX_SIZE_SUFFIX_PARSE_OK;
    }

    unsigned int power = 0;
    if (!bx_size_suffix_prefix_power(suffix[0], &power)) {
        return BX_SIZE_SUFFIX_PARSE_INVALID;
    }

    if (suffix[1] == '\0') {
        return bx_size_pow_result(1024, power, multiplier_out);
    }

    if (suffix[1] == 'B' && suffix[2] == '\0') {
        return bx_size_pow_result(1000, power, multiplier_out);
    }

    if (suffix[1] == 'i' && suffix[2] == 'B' && suffix[3] == '\0') {
        return bx_size_pow_result(1024, power, multiplier_out);
    }

    return BX_SIZE_SUFFIX_PARSE_INVALID;
}

bool bx_size_suffix_multiplier(const char* suffix, uintmax_t* multiplier_out) {
    return bx_size_suffix_multiplier_result(suffix, multiplier_out) == BX_SIZE_SUFFIX_PARSE_OK;
}

bool bx_size_parse_block_size(const char* text, uintmax_t* value_out) {
    if (text == NULL || text[0] == '\0' || value_out == NULL) {
        return false;
    }

    uintmax_t value = 0;
    if (!bx_size_parse_scaled_uint(text, &value) || value == 0) {
        return false;
    }

    *value_out = value;
    return true;
}

static bool bx_size_parse_factor(const char* text, size_t len, uintmax_t* value_out) {
    return bx_size_parse_scaled_uint_len(text, len, value_out);
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
        if ((*p == 'x' && p[1] >= '0' && p[1] <= '9') || *p == '\0') {
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
