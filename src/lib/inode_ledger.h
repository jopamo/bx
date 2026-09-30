#ifndef BX_LIB_INODE_LEDGER_H
#define BX_LIB_INODE_LEDGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>

struct bx_inode_ledger_slot {
    dev_t dev;
    ino_t ino;
    mode_t type;
    uint64_t sequence;
};

/* Zero-initialize. Stores identities, never descriptors or paths. */
struct bx_inode_ledger {
    struct bx_inode_ledger_slot* slots;
    size_t len;
    size_t cap;
};

bool bx_inode_ledger_lookup(const struct bx_inode_ledger* set, const struct stat* status, uint64_t* sequence);
/* Existing identities succeed without consuming the caller's count limit. */
bool bx_inode_ledger_record(struct bx_inode_ledger* set, const struct stat* status, uint64_t sequence, size_t limit);
void bx_inode_ledger_free(struct bx_inode_ledger* set);

#endif
