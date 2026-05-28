#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "dev_counters.h"
#include "rg_publish.h"

void bx_rg_publish_emit_record_default(void *user,
                                       struct bx_rg_publish_record *record) {
    struct bx_rg_publish_aggregate *state = user;

    if (!state || !record)
        return;

    if (record->used_heading && record->stdout_len > 0u &&
        state->heading_output_started && *state->heading_output_started) {
        fputc('\n', stdout);
    }
    if (record->stdout_len > 0u && record->stdout_buf)
        fwrite(record->stdout_buf, 1u, record->stdout_len, stdout);
    if (record->stderr_len > 0u && record->stderr_buf)
        fwrite(record->stderr_buf, 1u, record->stderr_len, stderr);
    if (record->used_heading && record->stdout_len > 0u && state->heading_output_started)
        *state->heading_output_started = true;

    if (state->stats) {
        state->stats->matches += record->stats.matches;
        state->stats->matched_lines += record->stats.matched_lines;
        state->stats->files_with_matches += record->stats.files_with_matches;
        state->stats->files_searched += record->stats.files_searched;
        state->stats->bytes_printed += record->stats.bytes_printed;
        state->stats->bytes_searched += record->stats.bytes_searched;
    }

    if (record->match_seen) {
        if (state->match_seen)
            *state->match_seen = true;
        if (state->exit_status && *state->exit_status != 2)
            *state->exit_status = 0;
    }
    if (record->error_seen) {
        if (state->error_seen)
            *state->error_seen = true;
        if (state->exit_status)
            *state->exit_status = 2;
    }
}

void bx_rg_publish_dispose_record(struct bx_rg_publish_record *record) {
    if (!record)
        return;

    free(record->stdout_buf);
    free(record->stderr_buf);
    free(record);
}

static uint64_t bx_rg_publish_record_seq_bridge(const void *record_ptr, void *user) {
    const struct bx_rg_publish_record *record = record_ptr;
    struct bx_rg_publish_state *state = user;

    return state->opts.record_seq(record, state->opts.user);
}

static void bx_rg_publish_emit_record_bridge(void *user, void *record_ptr) {
    struct bx_rg_publish_state *state = user;
    struct bx_rg_publish_record *record = record_ptr;

    state->opts.emit_record(state->opts.user, record);
}

static void bx_rg_publish_dispose_record_bridge(void *user, void *record_ptr) {
    struct bx_rg_publish_state *state = user;
    struct bx_rg_publish_record *record = record_ptr;

    if (state->opts.dispose_record)
        state->opts.dispose_record(state->opts.user, record);
}

bool bx_rg_publish_init(struct bx_rg_publish_state *state,
                        const struct bx_rg_publish_opts *opts) {
    if (!state || !opts || !opts->emit_record)
        return false;

    memset(state, 0, sizeof(*state));
    state->opts = *opts;

    if (opts->mode == BX_RG_PUBLISH_ORDERED) {
        if (!opts->record_seq)
            return false;
        struct bx_output_sink_opts sink_opts = {
            .max_pending = opts->max_pending,
            .first_seq = opts->first_seq,
            .ordered = true,
            .user = state,
            .record_seq = bx_rg_publish_record_seq_bridge,
            .emit_record = bx_rg_publish_emit_record_bridge,
            .dispose_record = bx_rg_publish_dispose_record_bridge,
        };
        if (!bx_output_sink_init(&state->ordered_sink, &sink_opts))
            return false;
        state->ordered_sink_ready = true;
        return true;
    }

    if (pthread_mutex_init(&state->unordered_lock, NULL) != 0)
        return false;
    state->unordered_lock_ready = true;
    return true;
}

bool bx_rg_publish_submit(struct bx_rg_publish_state *state,
                          struct bx_rg_publish_record *record) {
    if (!state || !record)
        return false;

    bx_search_dev_counters_note_rg_sched(BX_SEARCH_RG_SCHED_QUEUED_OUTPUT_BATCHES, 1u);
    if (record->stdout_len == 0u && record->stderr_len == 0u)
        bx_search_dev_counters_note_rg_sched(BX_SEARCH_RG_SCHED_EMPTY_OUTPUT_BATCHES, 1u);

    if (state->opts.mode == BX_RG_PUBLISH_ORDERED) {
        if (!bx_output_sink_submit(&state->ordered_sink, record))
            return false;
        bx_search_dev_counters_note_rg_sched(BX_SEARCH_RG_SCHED_OUTPUT_RECORDS_SUBMITTED, 1u);
        bx_search_dev_counters_note_rg_sched(BX_SEARCH_RG_SCHED_ORDERED_OUTPUT_RECORDS, 1u);
        return true;
    }

    pthread_mutex_lock(&state->unordered_lock);
    state->opts.emit_record(state->opts.user, record);
    pthread_mutex_unlock(&state->unordered_lock);
    bx_search_dev_counters_note_rg_sched(BX_SEARCH_RG_SCHED_UNORDERED_OUTPUT_FLUSHES, 1u);
    if (state->opts.dispose_record)
        state->opts.dispose_record(state->opts.user, record);
    return true;
}

bool bx_rg_publish_skip_seq(struct bx_rg_publish_state *state, uint64_t seq) {
    if (!state)
        return false;
    if (state->opts.mode != BX_RG_PUBLISH_ORDERED)
        return true;
    if (!bx_output_sink_skip_seq(&state->ordered_sink, seq))
        return false;
    bx_search_dev_counters_note_rg_sched(BX_SEARCH_RG_SCHED_SKIPPED_OUTPUT_SEQS, 1u);
    return true;
}

void bx_rg_publish_close(struct bx_rg_publish_state *state) {
    if (!state)
        return;
    if (state->opts.mode == BX_RG_PUBLISH_ORDERED)
        bx_output_sink_close(&state->ordered_sink);
}

void bx_rg_publish_wake(struct bx_rg_publish_state *state) {
    if (!state)
        return;
    if (state->opts.mode == BX_RG_PUBLISH_ORDERED)
        bx_output_sink_wake(&state->ordered_sink);
}

bool bx_rg_publish_join(struct bx_rg_publish_state *state) {
    if (!state)
        return false;
    if (state->opts.mode == BX_RG_PUBLISH_ORDERED)
        return bx_output_sink_join(&state->ordered_sink);
    return true;
}

void bx_rg_publish_dispose(struct bx_rg_publish_state *state) {
    if (!state)
        return;
    if (state->ordered_sink_ready)
        bx_output_sink_dispose(&state->ordered_sink);
    if (state->unordered_lock_ready)
        pthread_mutex_destroy(&state->unordered_lock);
    memset(state, 0, sizeof(*state));
}
