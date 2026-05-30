#include "lib/workqueue_contract.h"

static const struct bx_workqueue_contract bx_workqueue_contracts[] = {
    {
        .kind = BX_WORKQUEUE_CONTRACT_WALKER_JOBS,
        .name = "walker jobs",
        .producer = "recursive traversal coordinator",
        .consumer = "directory/file discovery workers",
        .payload = "owned path or directory work item plus borrowed immutable walk policy",
        .bounded_by = "explicit pending directory/file job capacity and file-descriptor budget",
        .submit_rule = "successful submit transfers ownership to the queue",
        .failure_rule = "failed submit preserves producer ownership for disposal",
        .cancel_rule = "cancellation closes or wakes the queue before join",
        .teardown_rule = "close before join; reclaim queued leftovers only after workers are quiescent",
        .backpressure_limits = BX_BACKPRESSURE_LIMIT_PENDING_DIRS |
                               BX_BACKPRESSURE_LIMIT_PENDING_FILES |
                               BX_BACKPRESSURE_LIMIT_OPEN_FDS,
        .requires_explicit_bound = true,
        .successful_submit_transfers_ownership = true,
        .failed_submit_preserves_producer_ownership = true,
        .producer_blocks_when_full = true,
        .close_before_join = true,
    },
    {
        .kind = BX_WORKQUEUE_CONTRACT_SCANNER_JOBS,
        .name = "scanner jobs",
        .producer = "walker or search batch builder",
        .consumer = "search scanner workers",
        .payload = "owned file batch or path job plus borrowed immutable search plan",
        .bounded_by = "explicit pending file batch capacity and pending path-byte budget",
        .submit_rule = "successful submit transfers ownership to the queue",
        .failure_rule = "failed submit preserves producer ownership for disposal",
        .cancel_rule = "cancellation wakes producers and consumers so scanner workers drain or stop",
        .teardown_rule = "close before join; reclaim queued batches only after workers are quiescent",
        .backpressure_limits = BX_BACKPRESSURE_LIMIT_PENDING_FILES |
                               BX_BACKPRESSURE_LIMIT_MMAP_BYTES,
        .requires_explicit_bound = true,
        .successful_submit_transfers_ownership = true,
        .failed_submit_preserves_producer_ownership = true,
        .producer_blocks_when_full = true,
        .close_before_join = true,
    },
    {
        .kind = BX_WORKQUEUE_CONTRACT_ARCHIVE_MEMBERS,
        .name = "archive members",
        .producer = "archive reader or compressor frontend",
        .consumer = "archive member/chunk workers",
        .payload = "owned archive member or chunk packet plus borrowed archive policy",
        .bounded_by = "explicit pending member/chunk count and pending byte budget",
        .submit_rule = "successful submit transfers ownership to the queue",
        .failure_rule = "failed submit preserves producer ownership for disposal",
        .cancel_rule = "cancellation wakes producers and workers before archive output is finalized",
        .teardown_rule = "close before join; publish or discard packets before reclaiming buffers",
        .backpressure_limits = BX_BACKPRESSURE_LIMIT_PENDING_ARCHIVE_MEMBERS |
                               BX_BACKPRESSURE_LIMIT_PENDING_OUTPUT_BYTES,
        .requires_explicit_bound = true,
        .successful_submit_transfers_ownership = true,
        .failed_submit_preserves_producer_ownership = true,
        .producer_blocks_when_full = true,
        .close_before_join = true,
    },
    {
        .kind = BX_WORKQUEUE_CONTRACT_CHILD_ACTIONS,
        .name = "child actions",
        .producer = "applet command planner",
        .consumer = "child_runner spawn and reaper loop",
        .payload = "owned argv/env/cwd/fd action plus borrowed execution policy",
        .bounded_by = "explicit process-slot limit, argv-byte budget, and fd budget",
        .submit_rule = "successful slot claim transfers ownership of the action to child_runner",
        .failure_rule = "failed slot claim preserves producer ownership for disposal",
        .cancel_rule = "cancellation stops new spawns, signals running children, then drains reaping",
        .teardown_rule = "stop spawning before join/reap; reclaim argv/env storage after child completion",
        .backpressure_limits = BX_BACKPRESSURE_LIMIT_CHILD_PROCESSES |
                               BX_BACKPRESSURE_LIMIT_OPEN_FDS,
        .requires_explicit_bound = true,
        .successful_submit_transfers_ownership = true,
        .failed_submit_preserves_producer_ownership = true,
        .producer_blocks_when_full = true,
        .close_before_join = true,
    },
    {
        .kind = BX_WORKQUEUE_CONTRACT_OUTPUT_CHUNKS,
        .name = "output chunks",
        .producer = "worker-local formatter or ordered publisher frontend",
        .consumer = "output publisher/emitter",
        .payload = "owned output record/chunk plus borrowed output policy",
        .bounded_by = "explicit pending record count and pending output-byte budget",
        .submit_rule = "successful submit transfers ownership to the publisher",
        .failure_rule = "failed submit preserves producer ownership for disposal",
        .cancel_rule = "cancellation wakes publisher and producers so stdout/stderr ownership is resolved",
        .teardown_rule = "close before join; emit, skip, or dispose chunks before reclaiming buffers",
        .backpressure_limits = BX_BACKPRESSURE_LIMIT_PENDING_OUTPUT_BYTES,
        .requires_explicit_bound = true,
        .successful_submit_transfers_ownership = true,
        .failed_submit_preserves_producer_ownership = true,
        .producer_blocks_when_full = true,
        .close_before_join = true,
    },
};

size_t bx_workqueue_contract_count(void) {
    return sizeof(bx_workqueue_contracts) / sizeof(bx_workqueue_contracts[0]);
}

const struct bx_workqueue_contract *bx_workqueue_contract_at(size_t index) {
    if (index >= bx_workqueue_contract_count()) {
        return NULL;
    }
    return &bx_workqueue_contracts[index];
}

const struct bx_workqueue_contract *bx_workqueue_contract_for_kind(
    enum bx_workqueue_contract_kind kind) {
    for (size_t i = 0; i < bx_workqueue_contract_count(); i++) {
        if (bx_workqueue_contracts[i].kind == kind) {
            return &bx_workqueue_contracts[i];
        }
    }
    return NULL;
}

bool bx_workqueue_contract_is_valid(const struct bx_workqueue_contract *contract) {
    return contract != NULL &&
           contract->kind != BX_WORKQUEUE_CONTRACT_UNSPECIFIED &&
           contract->name != NULL &&
           contract->producer != NULL &&
           contract->consumer != NULL &&
           contract->payload != NULL &&
           contract->bounded_by != NULL &&
           contract->submit_rule != NULL &&
           contract->failure_rule != NULL &&
           contract->cancel_rule != NULL &&
           contract->teardown_rule != NULL &&
           bx_backpressure_limit_mask_valid(contract->backpressure_limits) &&
           contract->requires_explicit_bound &&
           contract->successful_submit_transfers_ownership &&
           contract->failed_submit_preserves_producer_ownership &&
           contract->producer_blocks_when_full &&
           contract->close_before_join;
}
