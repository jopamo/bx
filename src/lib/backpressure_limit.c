#include "backpressure_limit.h"

static const struct bx_backpressure_limit bx_backpressure_limits[] = {
    {
        .kind = BX_BACKPRESSURE_LIMIT_PENDING_DIRS,
        .name = "pending dirs",
        .unit = "directory work items",
        .applies_to = "recursive walkers and subtree schedulers",
        .rule = "block producer or keep traversal local before directory work is unbounded",
        .default_limit = BX_BACKPRESSURE_DEFAULT_PENDING_DIRS,
    },
    {
        .kind = BX_BACKPRESSURE_LIMIT_PENDING_FILES,
        .name = "pending files",
        .unit = "file path work items",
        .applies_to = "walker-to-scanner queues and search batch builders",
        .rule = "bound queued file paths before scanner workers own them",
        .default_limit = BX_BACKPRESSURE_DEFAULT_PENDING_FILES,
    },
    {
        .kind = BX_BACKPRESSURE_LIMIT_PENDING_ARCHIVE_MEMBERS,
        .name = "pending archive members",
        .unit = "archive member or chunk packets",
        .applies_to = "archive reader-to-worker queues and ordered archive publication",
        .rule = "bound compressed/decompressed chunks before archive output finalization",
        .default_limit = BX_BACKPRESSURE_DEFAULT_PENDING_ARCHIVE_MEMBERS,
    },
    {
        .kind = BX_BACKPRESSURE_LIMIT_PENDING_OUTPUT_BYTES,
        .name = "pending output bytes",
        .unit = "owned stdout/stderr bytes",
        .applies_to = "ordered output sinks and search publishers",
        .rule = "block producers while queued output bytes exceed the budget",
        .default_limit = BX_BACKPRESSURE_DEFAULT_PENDING_OUTPUT_BYTES,
    },
    {
        .kind = BX_BACKPRESSURE_LIMIT_OPEN_FDS,
        .name = "open fds",
        .unit = "file descriptors",
        .applies_to = "walkers, archive streams, and child fd actions",
        .rule = "claim fd budget before opening or donating descriptors",
        .default_limit = BX_BACKPRESSURE_DEFAULT_OPEN_FDS,
    },
    {
        .kind = BX_BACKPRESSURE_LIMIT_MMAP_BYTES,
        .name = "mmap bytes",
        .unit = "mapped bytes",
        .applies_to = "future mmap-backed scanners or file readers",
        .rule = "claim mapped-byte budget before mapping file contents",
        .default_limit = BX_BACKPRESSURE_DEFAULT_MMAP_BYTES,
    },
    {
        .kind = BX_BACKPRESSURE_LIMIT_CHILD_PROCESSES,
        .name = "child processes",
        .unit = "running child slots",
        .applies_to = "child_runner process-slot queues",
        .rule = "claim a process slot before spawning and release it after reap",
        .default_limit = BX_BACKPRESSURE_DEFAULT_CHILD_PROCESSES,
    },
};

size_t bx_backpressure_limit_count(void) {
    return sizeof(bx_backpressure_limits) / sizeof(bx_backpressure_limits[0]);
}

const struct bx_backpressure_limit *bx_backpressure_limit_at(size_t index) {
    if (index >= bx_backpressure_limit_count())
        return NULL;
    return &bx_backpressure_limits[index];
}

const struct bx_backpressure_limit *bx_backpressure_limit_for_kind(
    enum bx_backpressure_limit_kind kind) {
    if (!bx_backpressure_limit_kind_valid(kind))
        return NULL;
    for (size_t i = 0; i < bx_backpressure_limit_count(); i++) {
        if (bx_backpressure_limits[i].kind == kind)
            return &bx_backpressure_limits[i];
    }
    return NULL;
}

bool bx_backpressure_limit_kind_valid(enum bx_backpressure_limit_kind kind) {
    unsigned int bit = (unsigned int)kind;

    return bit != 0u &&
           (bit & ~((unsigned int)BX_BACKPRESSURE_LIMIT_ALL)) == 0u &&
           (bit & (bit - 1u)) == 0u;
}

bool bx_backpressure_limit_mask_valid(unsigned int mask) {
    return mask != 0u && (mask & ~((unsigned int)BX_BACKPRESSURE_LIMIT_ALL)) == 0u;
}

size_t bx_backpressure_limit_default(enum bx_backpressure_limit_kind kind) {
    const struct bx_backpressure_limit *limit = bx_backpressure_limit_for_kind(kind);

    return limit ? limit->default_limit : 0u;
}

bool bx_backpressure_limit_can_add(size_t current, size_t amount, size_t limit) {
    if (limit == 0u || amount == 0u)
        return true;
    if (amount > limit)
        return current == 0u;
    return current <= limit - amount;
}
