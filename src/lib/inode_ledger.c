#include "lib/inode_ledger.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "bx/libbx.h"

static size_t bx_inode_hash(dev_t dev, ino_t ino) {
    uint64_t value = (uint64_t)ino ^ ((uint64_t)dev * UINT64_C(0x9e3779b97f4a7c15));
    value ^= value >> 30;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64_C(0x94d049bb133111eb);
    return (size_t)(value ^ (value >> 31));
}

static size_t bx_inode_slot(const struct bx_inode_ledger* set, const struct stat* status) {
    size_t index = bx_inode_hash(status->st_dev, status->st_ino) & (set->cap - 1u);
    while (set->slots[index].type && (set->slots[index].dev != status->st_dev || set->slots[index].ino != status->st_ino || set->slots[index].type != (status->st_mode & S_IFMT)))
        index = (index + 1u) & (set->cap - 1u);
    return index;
}

bool bx_inode_ledger_lookup(const struct bx_inode_ledger* set, const struct stat* status, uint64_t* sequence) {
    if (!set->cap)
        return false;
    const struct bx_inode_ledger_slot* slot = &set->slots[bx_inode_slot(set, status)];
    if (!slot->type)
        return false;
    if (sequence)
        *sequence = slot->sequence;
    return true;
}

bool bx_inode_ledger_record(struct bx_inode_ledger* set, const struct stat* status, uint64_t sequence, size_t limit) {
    if (!(status->st_mode & S_IFMT)) {
        errno = EINVAL;
        return false;
    }
    if (bx_inode_ledger_lookup(set, status, NULL)) {
        set->slots[bx_inode_slot(set, status)].sequence = sequence;
        return true;
    }
    if (set->len >= limit) {
        errno = E2BIG;
        return false;
    }
    if (!set->cap || set->len >= set->cap / 2u) {
        if (set->cap > SIZE_MAX / 2u / sizeof(*set->slots)) {
            errno = E2BIG;
            return false;
        }
        struct bx_inode_ledger old = *set;
        set->cap = set->cap ? set->cap * 2u : 16u;
        set->slots = xmalloc(set->cap * sizeof(*set->slots));
        memset(set->slots, 0, set->cap * sizeof(*set->slots));
        for (size_t i = 0; i < old.cap; i++) {
            const struct bx_inode_ledger_slot* slot = &old.slots[i];
            if (slot->type) {
                struct stat item = {.st_dev = slot->dev, .st_ino = slot->ino, .st_mode = slot->type};
                set->slots[bx_inode_slot(set, &item)] = *slot;
            }
        }
        free(old.slots);
    }
    set->slots[bx_inode_slot(set, status)] = (struct bx_inode_ledger_slot){
        .dev = status->st_dev,
        .ino = status->st_ino,
        .type = status->st_mode & S_IFMT,
        .sequence = sequence,
    };
    set->len++;
    return true;
}

void bx_inode_ledger_free(struct bx_inode_ledger* set) {
    free(set->slots);
    *set = (struct bx_inode_ledger){0};
}
