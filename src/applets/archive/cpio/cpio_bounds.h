#ifndef BX_CPIO_BOUNDS_H
#define BX_CPIO_BOUNDS_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "lib/checked_math.h"

struct bx_cpio_member_bounds {
    size_t data_offset;
    size_t next_offset;
};

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
