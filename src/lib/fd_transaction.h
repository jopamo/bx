#ifndef BX_LIB_FD_TRANSACTION_H
#define BX_LIB_FD_TRANSACTION_H

#include <stdbool.h>
#include <stddef.h>

struct bx_fd_transaction;
struct bx_fd_transaction_entry;
struct bx_fd_transaction_ref;

/*
 * A stack owns every backup descriptor created by its active transactions.
 * Transactions must finish in strict last-in, first-out order.
 */
struct bx_fd_transaction_stack {
    struct bx_fd_transaction_entry* entries;
    size_t entry_count;
    size_t entry_capacity;
    struct bx_fd_transaction* active;
};

/*
 * Callers keep a transaction object alive from begin through rollback or
 * commit. Descriptor references are copied by begin so private backups can be
 * kept outside the descriptor namespace visible to that transaction.
 */
struct bx_fd_transaction {
    struct bx_fd_transaction_stack* stack;
    struct bx_fd_transaction* parent;
    struct bx_fd_transaction_ref* references;
    size_t reference_count;
    size_t entry_start;
    int next_backup_fd;
};

void bx_fd_transaction_stack_init(struct bx_fd_transaction_stack* stack);
bool bx_fd_transaction_stack_invariants(
    const struct bx_fd_transaction_stack* stack
);
/*
 * Discard closes all private backups without restoring target descriptors.
 * It also invalidates every active transaction. This is intended for process
 * teardown and fork children that must not inherit a parent's save stack.
 */
void bx_fd_transaction_stack_discard(struct bx_fd_transaction_stack* stack);

void bx_fd_transaction_init(struct bx_fd_transaction* transaction);
bool bx_fd_transaction_active(
    const struct bx_fd_transaction* transaction
);
int bx_fd_transaction_begin(
    struct bx_fd_transaction_stack* stack,
    struct bx_fd_transaction* transaction,
    const int* descriptor_references,
    size_t reference_count
);
/*
 * Save records a target's state once per transaction. It does not change the
 * target. Every target must have been declared as a descriptor reference.
 */
int bx_fd_transaction_save(
    struct bx_fd_transaction* transaction,
    int target_fd
);
/*
 * Rollback restores targets and their descriptor flags. It attempts every
 * restore before returning the first error.
 */
int bx_fd_transaction_rollback(struct bx_fd_transaction* transaction);
/* Commit keeps target changes and releases only transaction-owned backups. */
int bx_fd_transaction_commit(struct bx_fd_transaction* transaction);

#endif /* BX_LIB_FD_TRANSACTION_H */
