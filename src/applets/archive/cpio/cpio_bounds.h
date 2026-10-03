#ifndef BX_CPIO_BOUNDS_H
#define BX_CPIO_BOUNDS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "lib/checked_math.h"

struct bx_cpio_member_bounds {
    size_t data_offset;
    size_t next_offset;
};

/* A rejected field remains private to the caller's uncommitted header. */
static inline bool bx_cpio_format_number(unsigned char* field, size_t width, uintmax_t value, unsigned int bits) {
    static const char digits[] = "0123456789ABCDEF";
    if (!field || !width || (bits != 3 && bits != 4))
        return false;
    unsigned int mask = (1u << bits) - 1u;
    for (size_t i = width; i > 0; i--) {
        field[i - 1] = (unsigned char)digits[value & mask];
        value >>= bits;
    }
    return value == 0;
}

static inline bool bx_cpio_encode_newc_header(unsigned char* header, const uintmax_t fields[13]) {
    for (size_t i = 0; i < 13; i++) {
        if (!bx_cpio_format_number(header + 6 + 8 * i, 8, fields[i], 4))
            return false;
    }
    memcpy(header, "070701", 6);
    return true;
}

static inline bool bx_cpio_encode_odc_header(unsigned char* header, const uintmax_t fields[10]) {
    size_t offset = 6;
    for (size_t i = 0; i < 10; i++) {
        size_t width = i == 7 || i == 9 ? 11 : 6;
        if (!bx_cpio_format_number(header + offset, width, fields[i], 3))
            return false;
        offset += width;
    }
    memcpy(header, "070707", 6);
    return true;
}

/* The payload span has already been checked; symlink text is not NUL-padded. */
static inline bool bx_cpio_symlink_target_valid(const unsigned char* data, size_t size) {
    return data != NULL && size > 0 && memchr(data, '\0', size) == NULL;
}

/* Validate the complete member before allocating or copying any field. */
static inline bool bx_cpio_member_bounds(const unsigned char* data, size_t length,
                                         size_t name_offset, size_t name_size,
                                         size_t payload_size, size_t name_alignment,
                                         size_t payload_alignment,
                                         struct bx_cpio_member_bounds* bounds) {
    size_t name_end, data_offset, data_end, next_offset;
    if (!data || !bounds || name_size < 2 ||
        !bx_checked_size_range_within(length, name_offset, name_size, &name_end) ||
        !bx_checked_size_round_up(name_end, name_alignment, &data_offset) ||
        !bx_checked_size_range_within(length, data_offset, payload_size, &data_end) ||
        !bx_checked_size_round_up(data_end, payload_alignment, &next_offset) ||
        next_offset > length)
        return false;
    if (data[name_end - 1] != '\0' ||
        memchr(data + name_offset, '\0', name_size - 1) != NULL)
        return false;
    *bounds = (struct bx_cpio_member_bounds){data_offset, next_offset};
    return true;
}

#endif
