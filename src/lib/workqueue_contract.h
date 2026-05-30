#ifndef BX_LIB_WORKQUEUE_CONTRACT_H
#define BX_LIB_WORKQUEUE_CONTRACT_H

#include <stdbool.h>
#include <stddef.h>

#include "backpressure_limit.h"

enum bx_workqueue_contract_kind {
    BX_WORKQUEUE_CONTRACT_UNSPECIFIED = 0,
    BX_WORKQUEUE_CONTRACT_WALKER_JOBS,
    BX_WORKQUEUE_CONTRACT_SCANNER_JOBS,
    BX_WORKQUEUE_CONTRACT_ARCHIVE_MEMBERS,
    BX_WORKQUEUE_CONTRACT_CHILD_ACTIONS,
    BX_WORKQUEUE_CONTRACT_OUTPUT_CHUNKS,
};

struct bx_workqueue_contract {
    enum bx_workqueue_contract_kind kind;
    const char *name;
    const char *producer;
    const char *consumer;
    const char *payload;
    const char *bounded_by;
    const char *submit_rule;
    const char *failure_rule;
    const char *cancel_rule;
    const char *teardown_rule;
    unsigned int backpressure_limits;
    bool requires_explicit_bound;
    bool successful_submit_transfers_ownership;
    bool failed_submit_preserves_producer_ownership;
    bool producer_blocks_when_full;
    bool close_before_join;
};

size_t bx_workqueue_contract_count(void);
const struct bx_workqueue_contract *bx_workqueue_contract_at(size_t index);
const struct bx_workqueue_contract *bx_workqueue_contract_for_kind(
    enum bx_workqueue_contract_kind kind);
bool bx_workqueue_contract_is_valid(const struct bx_workqueue_contract *contract);

#endif /* BX_LIB_WORKQUEUE_CONTRACT_H */
