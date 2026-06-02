#ifndef BX_COMMON_CHECKED_MATH_H
#define BX_COMMON_CHECKED_MATH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <time.h>

static inline bool bx_checked_size_add(size_t left, size_t right, size_t* out) {
    if (out == NULL || left > SIZE_MAX - right) {
        return false;
    }

    *out = left + right;
    return true;
}

static inline bool bx_checked_size_mul(size_t left, size_t right, size_t* out) {
    if (out == NULL || (left != 0u && right > SIZE_MAX / left)) {
        return false;
    }

    *out = left * right;
    return true;
}

static inline bool bx_checked_size_round_up(size_t value, size_t align, size_t* out) {
    if (out == NULL || align == 0u) {
        return false;
    }

    size_t remainder = value % align;
    if (remainder == 0u) {
        *out = value;
        return true;
    }

    return bx_checked_size_add(value, align - remainder, out);
}

static inline bool bx_checked_size_offset_add(size_t base, size_t offset, size_t* out) {
    return bx_checked_size_add(base, offset, out);
}

static inline bool bx_checked_size_range_end(size_t offset, size_t length, size_t* end_out) {
    return bx_checked_size_offset_add(offset, length, end_out);
}

static inline bool bx_checked_size_range_within(size_t total, size_t offset, size_t length, size_t* end_out) {
    size_t end = 0u;
    if (end_out == NULL || !bx_checked_size_range_end(offset, length, &end) || end > total) {
        return false;
    }

    *end_out = end;
    return true;
}

static inline bool bx_checked_uintmax_add(uintmax_t left, uintmax_t right, uintmax_t* out) {
    if (out == NULL || left > UINTMAX_MAX - right) {
        return false;
    }

    *out = left + right;
    return true;
}

static inline bool bx_checked_uintmax_mul(uintmax_t left, uintmax_t right, uintmax_t* out) {
    if (out == NULL || (left != 0u && right > UINTMAX_MAX / left)) {
        return false;
    }

    *out = left * right;
    return true;
}

static inline bool bx_checked_uintmax_round_up(uintmax_t value, uintmax_t align, uintmax_t* out) {
    if (out == NULL || align == 0u) {
        return false;
    }

    uintmax_t remainder = value % align;
    if (remainder == 0u) {
        *out = value;
        return true;
    }

    return bx_checked_uintmax_add(value, align - remainder, out);
}

static inline bool bx_checked_uintmax_blocks_to_bytes(uintmax_t blocks, uintmax_t block_size, uintmax_t* bytes_out) {
    if (block_size == 0u) {
        return false;
    }

    return bx_checked_uintmax_mul(blocks, block_size, bytes_out);
}

static inline bool bx_checked_uintmax_bytes_to_blocks_ceil(uintmax_t bytes, uintmax_t block_size, uintmax_t* blocks_out) {
    if (blocks_out == NULL || block_size == 0u) {
        return false;
    }

    uintmax_t blocks = bytes / block_size;
    if (bytes % block_size == 0u) {
        *blocks_out = blocks;
        return true;
    }

    return bx_checked_uintmax_add(blocks, 1u, blocks_out);
}

static inline bool bx_checked_time_t_is_signed(void) {
    return (time_t)-1 < (time_t)0;
}

static inline unsigned int bx_checked_time_t_bits(void) {
    return (unsigned int)(sizeof(time_t) * CHAR_BIT);
}

static inline unsigned int bx_checked_uintmax_bits(void) {
    return (unsigned int)(sizeof(uintmax_t) * CHAR_BIT);
}

static inline uintmax_t bx_checked_time_t_unsigned_max(void) {
    unsigned int bits = bx_checked_time_t_bits();
    if (bits >= bx_checked_uintmax_bits()) {
        return UINTMAX_MAX;
    }
    if (bits == 0u) {
        return 0u;
    }

    return ((uintmax_t)1u << bits) - 1u;
}

static inline uintmax_t bx_checked_time_t_signed_positive_max(void) {
    unsigned int bits = bx_checked_time_t_bits();
    if (bits <= 1u) {
        return 0u;
    }

    unsigned int value_bits = bits - 1u;
    if (value_bits >= bx_checked_uintmax_bits()) {
        return UINTMAX_MAX;
    }

    return ((uintmax_t)1u << value_bits) - 1u;
}

static inline uintmax_t bx_checked_time_t_signed_negative_magnitude_max(void) {
    unsigned int bits = bx_checked_time_t_bits();
    if (bits <= 1u) {
        return 0u;
    }

    unsigned int value_bits = bits - 1u;
    if (value_bits >= bx_checked_uintmax_bits()) {
        return UINTMAX_MAX;
    }

    return (uintmax_t)1u << value_bits;
}

static inline bool bx_checked_uintmax_to_time_t(uintmax_t value, time_t* out) {
    if (out == NULL) {
        return false;
    }

    uintmax_t max = bx_checked_time_t_is_signed()
                        ? bx_checked_time_t_signed_positive_max()
                        : bx_checked_time_t_unsigned_max();
    if (value > max) {
        return false;
    }

    time_t converted = (time_t)value;
    if ((uintmax_t)converted != value) {
        return false;
    }

    *out = converted;
    return true;
}

static inline bool bx_checked_intmax_to_time_t(intmax_t value, time_t* out) {
    if (out == NULL) {
        return false;
    }

    if (value >= 0) {
        return bx_checked_uintmax_to_time_t((uintmax_t)value, out);
    }

    if (!bx_checked_time_t_is_signed()) {
        return false;
    }

    uintmax_t magnitude = (uintmax_t)(-(value + 1)) + 1u;
    if (magnitude > bx_checked_time_t_signed_negative_magnitude_max()) {
        return false;
    }

    time_t converted = (time_t)value;
    if ((intmax_t)converted != value) {
        return false;
    }

    *out = converted;
    return true;
}

#endif /* BX_COMMON_CHECKED_MATH_H */
