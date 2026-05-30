#ifndef BX_LIB_BACKPRESSURE_LIMIT_H
#define BX_LIB_BACKPRESSURE_LIMIT_H

#include <stdbool.h>
#include <stddef.h>

enum bx_backpressure_limit_kind {
    BX_BACKPRESSURE_LIMIT_NONE = 0,
    BX_BACKPRESSURE_LIMIT_PENDING_DIRS = 1u << 0,
    BX_BACKPRESSURE_LIMIT_PENDING_FILES = 1u << 1,
    BX_BACKPRESSURE_LIMIT_PENDING_ARCHIVE_MEMBERS = 1u << 2,
    BX_BACKPRESSURE_LIMIT_PENDING_OUTPUT_BYTES = 1u << 3,
    BX_BACKPRESSURE_LIMIT_OPEN_FDS = 1u << 4,
    BX_BACKPRESSURE_LIMIT_MMAP_BYTES = 1u << 5,
    BX_BACKPRESSURE_LIMIT_CHILD_PROCESSES = 1u << 6,
    BX_BACKPRESSURE_LIMIT_ALL = BX_BACKPRESSURE_LIMIT_PENDING_DIRS |
                                BX_BACKPRESSURE_LIMIT_PENDING_FILES |
                                BX_BACKPRESSURE_LIMIT_PENDING_ARCHIVE_MEMBERS |
                                BX_BACKPRESSURE_LIMIT_PENDING_OUTPUT_BYTES |
                                BX_BACKPRESSURE_LIMIT_OPEN_FDS |
                                BX_BACKPRESSURE_LIMIT_MMAP_BYTES |
                                BX_BACKPRESSURE_LIMIT_CHILD_PROCESSES,
};

#define BX_BACKPRESSURE_DEFAULT_PENDING_DIRS ((size_t)4096u)
#define BX_BACKPRESSURE_DEFAULT_PENDING_FILES ((size_t)16384u)
#define BX_BACKPRESSURE_DEFAULT_PENDING_ARCHIVE_MEMBERS ((size_t)1024u)
#define BX_BACKPRESSURE_DEFAULT_PENDING_OUTPUT_BYTES ((size_t)16u * 1024u * 1024u)
#define BX_BACKPRESSURE_DEFAULT_OPEN_FDS ((size_t)256u)
#define BX_BACKPRESSURE_DEFAULT_MMAP_BYTES ((size_t)256u * 1024u * 1024u)
#define BX_BACKPRESSURE_DEFAULT_CHILD_PROCESSES ((size_t)256u)

struct bx_backpressure_limit {
    enum bx_backpressure_limit_kind kind;
    const char *name;
    const char *unit;
    const char *applies_to;
    const char *rule;
    size_t default_limit;
};

size_t bx_backpressure_limit_count(void);
const struct bx_backpressure_limit *bx_backpressure_limit_at(size_t index);
const struct bx_backpressure_limit *bx_backpressure_limit_for_kind(
    enum bx_backpressure_limit_kind kind);
bool bx_backpressure_limit_kind_valid(enum bx_backpressure_limit_kind kind);
bool bx_backpressure_limit_mask_valid(unsigned int mask);
size_t bx_backpressure_limit_default(enum bx_backpressure_limit_kind kind);
bool bx_backpressure_limit_can_add(size_t current, size_t amount, size_t limit);

#endif /* BX_LIB_BACKPRESSURE_LIMIT_H */
