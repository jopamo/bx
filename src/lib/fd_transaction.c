#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include "lib/fd_ops.h"
#include "lib/fd_transaction.h"

struct bx_fd_transaction_entry {
    int target_fd;
    int saved_fd;
    int target_flags;
};

struct bx_fd_transaction_ref {
    int fd;
    bool saved;
};

static int bx_fd_transaction_ref_compare(
    const void* left_pointer,
    const void* right_pointer
) {
    const struct bx_fd_transaction_ref* left = left_pointer;
    const struct bx_fd_transaction_ref* right = right_pointer;
    return (left->fd > right->fd) - (left->fd < right->fd);
}

static struct bx_fd_transaction_ref* bx_fd_transaction_find_ref(
    const struct bx_fd_transaction* transaction,
    int fd
) {
    size_t first = 0u;
    size_t count = transaction->reference_count;
    while (count != 0u) {
        size_t step = count / 2u;
        size_t index = first + step;
        int candidate = transaction->references[index].fd;
        if (candidate < fd) {
            first = index + 1u;
            count -= step + 1u;
        }
        else {
            count = step;
        }
    }
    if (first == transaction->reference_count ||
        transaction->references[first].fd != fd) {
        return NULL;
    }
    return &transaction->references[first];
}

static bool bx_fd_transaction_chain_acyclic(
    const struct bx_fd_transaction* active
) {
    const struct bx_fd_transaction* slow = active;
    const struct bx_fd_transaction* fast = active;
    while (fast != NULL && fast->parent != NULL) {
        slow = slow->parent;
        fast = fast->parent->parent;
        if (slow == fast) {
            return false;
        }
    }
    return true;
}

static bool bx_fd_transaction_references_invariant(
    const struct bx_fd_transaction* transaction
) {
    if ((transaction->reference_count == 0u) !=
        (transaction->references == NULL)) {
        return false;
    }
    for (size_t i = 0u; i < transaction->reference_count; i++) {
        if (transaction->references[i].fd < 0 ||
            (i != 0u &&
             transaction->references[i - 1u].fd >=
                transaction->references[i].fd)) {
            return false;
        }
    }
    return transaction->next_backup_fd >= 10;
}

static bool bx_fd_transaction_backup_invariant(
    const struct bx_fd_transaction_stack* stack,
    size_t entry_index
) {
    const struct bx_fd_transaction_entry* entry =
        &stack->entries[entry_index];
    if (entry->target_fd < 0 || entry->saved_fd == entry->target_fd ||
        (entry->target_flags & ~FD_CLOEXEC) != 0) {
        return false;
    }
    if (entry->saved_fd >= 0) {
        int flags = fcntl(entry->saved_fd, F_GETFD);
        if (flags < 0 || (flags & FD_CLOEXEC) == 0) {
            return false;
        }
    }
    for (size_t i = 0u; i < entry_index; i++) {
        if (entry->saved_fd >= 0 &&
            stack->entries[i].saved_fd == entry->saved_fd) {
            return false;
        }
        if (stack->entries[i].target_fd == entry->saved_fd ||
            (stack->entries[i].saved_fd >= 0 &&
             stack->entries[i].saved_fd == entry->target_fd)) {
            return false;
        }
    }
    for (const struct bx_fd_transaction* transaction = stack->active;
         transaction != NULL;
         transaction = transaction->parent) {
        if (entry->saved_fd >= 0 &&
            bx_fd_transaction_find_ref(
                transaction,
                entry->saved_fd
            ) != NULL) {
            return false;
        }
    }
    return true;
}

bool bx_fd_transaction_stack_invariants(
    const struct bx_fd_transaction_stack* stack
) {
    if (stack == NULL || stack->entry_count > stack->entry_capacity ||
        (stack->entry_capacity == 0u) != (stack->entries == NULL) ||
        !bx_fd_transaction_chain_acyclic(stack->active)) {
        return false;
    }
    if (stack->active == NULL) {
        return stack->entry_count == 0u;
    }

    size_t frame_end = stack->entry_count;
    for (const struct bx_fd_transaction* transaction = stack->active;
         transaction != NULL;
         transaction = transaction->parent) {
        if (transaction->stack != stack ||
            transaction->entry_start > frame_end ||
            !bx_fd_transaction_references_invariant(transaction)) {
            return false;
        }

        for (size_t i = transaction->entry_start; i < frame_end; i++) {
            const struct bx_fd_transaction_entry* entry =
                &stack->entries[i];
            const struct bx_fd_transaction_ref* reference =
                bx_fd_transaction_find_ref(
                    transaction,
                    entry->target_fd
                );
            if (reference == NULL || !reference->saved ||
                !bx_fd_transaction_backup_invariant(stack, i)) {
                return false;
            }
            for (size_t j = transaction->entry_start; j < i; j++) {
                if (stack->entries[j].target_fd == entry->target_fd) {
                    return false;
                }
            }
        }
        for (size_t i = 0u; i < transaction->reference_count; i++) {
            bool saved = false;
            for (size_t j = transaction->entry_start; j < frame_end; j++) {
                saved |= stack->entries[j].target_fd ==
                    transaction->references[i].fd;
            }
            if (saved != transaction->references[i].saved) {
                return false;
            }
        }
        frame_end = transaction->entry_start;
    }
    return frame_end == 0u;
}

static int bx_fd_transaction_dup_private(
    int source_fd,
    struct bx_fd_transaction* transaction
) {
    for (;;) {
        int duplicate;
        do {
            duplicate = bx_fd_dup_cloexec_min(
                source_fd,
                transaction->next_backup_fd
            );
        } while (duplicate < 0 && errno == EINTR);
        if (duplicate < 0) {
            return -1;
        }

        struct bx_fd_transaction_ref* collision =
            bx_fd_transaction_find_ref(transaction, duplicate);
        if (duplicate == INT_MAX) {
            transaction->next_backup_fd = INT_MAX;
            if (collision == NULL) {
                return duplicate;
            }
            close(duplicate);
            errno = EMFILE;
            return -1;
        }
        transaction->next_backup_fd = duplicate + 1;
        if (collision == NULL) {
            return duplicate;
        }

        /*
         * Advancing the minimum before closing is enough to exclude this
         * user-visible number from every later backup allocation.
         */
        close(duplicate);
    }
}

static int bx_fd_transaction_entries_reserve(
    struct bx_fd_transaction_stack* stack
) {
    if (stack->entry_count != stack->entry_capacity) {
        return 0;
    }
    size_t capacity = stack->entry_capacity == 0u ?
        4u : stack->entry_capacity * 2u;
    if (capacity < stack->entry_capacity ||
        capacity > SIZE_MAX / sizeof(*stack->entries)) {
        errno = ENOMEM;
        return -1;
    }
    struct bx_fd_transaction_entry* entries = realloc(
        stack->entries,
        capacity * sizeof(*entries)
    );
    if (entries == NULL) {
        return -1;
    }
    stack->entries = entries;
    stack->entry_capacity = capacity;
    return 0;
}

static void bx_fd_transaction_finish(
    struct bx_fd_transaction* transaction
) {
    struct bx_fd_transaction_stack* stack = transaction->stack;
    stack->entry_count = transaction->entry_start;
    stack->active = transaction->parent;
    free(transaction->references);
    *transaction = (struct bx_fd_transaction){0};
}

void bx_fd_transaction_stack_init(struct bx_fd_transaction_stack* stack) {
    if (stack != NULL) {
        *stack = (struct bx_fd_transaction_stack){0};
        assert(bx_fd_transaction_stack_invariants(stack));
    }
}

void bx_fd_transaction_stack_discard(struct bx_fd_transaction_stack* stack) {
    if (stack == NULL) {
        return;
    }
    for (size_t i = 0u; i < stack->entry_count; i++) {
        if (stack->entries[i].saved_fd >= 0) {
            close(stack->entries[i].saved_fd);
        }
    }
    free(stack->entries);

    while (stack->active != NULL) {
        struct bx_fd_transaction* transaction = stack->active;
        stack->active = transaction->parent;
        free(transaction->references);
        *transaction = (struct bx_fd_transaction){0};
    }
    *stack = (struct bx_fd_transaction_stack){0};
    assert(bx_fd_transaction_stack_invariants(stack));
}

void bx_fd_transaction_init(struct bx_fd_transaction* transaction) {
    if (transaction != NULL) {
        *transaction = (struct bx_fd_transaction){0};
    }
}

bool bx_fd_transaction_active(
    const struct bx_fd_transaction* transaction
) {
    return transaction != NULL && transaction->stack != NULL;
}

int bx_fd_transaction_begin(
    struct bx_fd_transaction_stack* stack,
    struct bx_fd_transaction* transaction,
    const int* descriptor_references,
    size_t reference_count
) {
    if (stack == NULL || transaction == NULL ||
        transaction->stack != NULL ||
        (reference_count != 0u && descriptor_references == NULL) ||
        !bx_fd_transaction_stack_invariants(stack)) {
        errno = EINVAL;
        return -1;
    }
    if (reference_count >
        SIZE_MAX / sizeof(*transaction->references)) {
        errno = ENOMEM;
        return -1;
    }

    struct bx_fd_transaction_ref* references = NULL;
    if (reference_count != 0u) {
        references = malloc(reference_count * sizeof(*references));
        if (references == NULL) {
            return -1;
        }
        for (size_t i = 0u; i < reference_count; i++) {
            if (descriptor_references[i] < 0) {
                free(references);
                errno = EINVAL;
                return -1;
            }
            references[i] = (struct bx_fd_transaction_ref){
                .fd = descriptor_references[i],
            };
        }
        qsort(
            references,
            reference_count,
            sizeof(*references),
            bx_fd_transaction_ref_compare
        );

        size_t unique_count = 0u;
        for (size_t i = 0u; i < reference_count; i++) {
            if (unique_count == 0u ||
                references[i].fd != references[unique_count - 1u].fd) {
                references[unique_count++] = references[i];
            }
        }
        reference_count = unique_count;
    }

    struct bx_fd_transaction candidate = {
        .stack = stack,
        .parent = stack->active,
        .references = references,
        .reference_count = reference_count,
        .entry_start = stack->entry_count,
        .next_backup_fd = 10,
    };

    /*
     * An outer transaction's backup is shell-private. If this transaction
     * names that number, move the backup before user policy can observe or
     * replace it. Earlier relocations remain equivalent if a later one fails.
     */
    for (size_t i = 0u; i < stack->entry_count; i++) {
        struct bx_fd_transaction_entry* entry = &stack->entries[i];
        if (entry->saved_fd < 0 ||
            bx_fd_transaction_find_ref(&candidate, entry->saved_fd) == NULL) {
            continue;
        }
        int duplicate = bx_fd_transaction_dup_private(
            entry->saved_fd,
            &candidate
        );
        if (duplicate < 0) {
            int error = errno;
            free(references);
            errno = error;
            return -1;
        }
        close(entry->saved_fd);
        entry->saved_fd = duplicate;
    }

    *transaction = candidate;
    stack->active = transaction;
    assert(bx_fd_transaction_stack_invariants(stack));
    return 0;
}

int bx_fd_transaction_save(
    struct bx_fd_transaction* transaction,
    int target_fd
) {
    if (transaction == NULL || transaction->stack == NULL ||
        transaction->stack->active != transaction) {
        errno = EINVAL;
        return -1;
    }
    struct bx_fd_transaction_ref* reference =
        bx_fd_transaction_find_ref(transaction, target_fd);
    if (reference == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (reference->saved) {
        return 0;
    }
    if (bx_fd_transaction_entries_reserve(transaction->stack) != 0) {
        return -1;
    }

    int target_flags = fcntl(target_fd, F_GETFD);
    int saved_fd = -1;
    if (target_flags < 0) {
        if (errno != EBADF) {
            return -1;
        }
        target_flags = 0;
    }
    else {
        saved_fd = bx_fd_transaction_dup_private(target_fd, transaction);
        if (saved_fd < 0) {
            return -1;
        }
    }

    transaction->stack->entries[transaction->stack->entry_count++] =
        (struct bx_fd_transaction_entry){
            .target_fd = target_fd,
            .saved_fd = saved_fd,
            .target_flags = target_flags,
        };
    reference->saved = true;
    assert(bx_fd_transaction_stack_invariants(transaction->stack));
    return 0;
}

int bx_fd_transaction_rollback(struct bx_fd_transaction* transaction) {
    if (transaction == NULL || transaction->stack == NULL ||
        transaction->stack->active != transaction) {
        errno = EINVAL;
        return -1;
    }

    int first_error = 0;
    struct bx_fd_transaction_stack* stack = transaction->stack;
    for (size_t i = stack->entry_count; i > transaction->entry_start; i--) {
        struct bx_fd_transaction_entry* entry = &stack->entries[i - 1u];
        if (entry->saved_fd < 0) {
            if (close(entry->target_fd) != 0 && errno != EBADF &&
                first_error == 0) {
                first_error = errno;
            }
            continue;
        }

        if (bx_fd_dup2_exact(entry->saved_fd, entry->target_fd) < 0) {
            if (first_error == 0) {
                first_error = errno;
            }
        }
        else if (fcntl(
                entry->target_fd,
                F_SETFD,
                entry->target_flags
            ) != 0 && first_error == 0) {
            first_error = errno;
        }
        close(entry->saved_fd);
    }
    bx_fd_transaction_finish(transaction);
    assert(bx_fd_transaction_stack_invariants(stack));
    if (first_error != 0) {
        errno = first_error;
        return -1;
    }
    return 0;
}

int bx_fd_transaction_commit(struct bx_fd_transaction* transaction) {
    if (transaction == NULL || transaction->stack == NULL ||
        transaction->stack->active != transaction) {
        errno = EINVAL;
        return -1;
    }

    struct bx_fd_transaction_stack* stack = transaction->stack;
    for (size_t i = transaction->entry_start; i < stack->entry_count; i++) {
        if (stack->entries[i].saved_fd >= 0) {
            close(stack->entries[i].saved_fd);
        }
    }
    bx_fd_transaction_finish(transaction);
    assert(bx_fd_transaction_stack_invariants(stack));
    return 0;
}
