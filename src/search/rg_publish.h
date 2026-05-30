#ifndef BX_SEARCH_RG_PUBLISH_H
#define BX_SEARCH_RG_PUBLISH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lib/output_publication.h"
#include "search_internal.h"

enum bx_rg_publish_mode {
    BX_RG_PUBLISH_UNORDERED = 0,
    BX_RG_PUBLISH_ORDERED,
};

struct bx_rg_publish_record {
    uint64_t seq;
    uint64_t debug_id;
    char *stdout_buf;
    size_t stdout_len;
    char *stderr_buf;
    size_t stderr_len;
    struct bx_search_stats stats;
    int status;
    bool match_seen;
    bool error_seen;
    bool used_heading;
};

struct bx_rg_publish_aggregate {
    FILE *stdout_stream;
    struct bx_search_stats *stats;
    int *exit_status;
    bool *match_seen;
    bool *error_seen;
    bool *heading_output_started;
};

struct bx_rg_publish_opts {
    enum bx_rg_publish_mode mode;
    size_t max_pending;
    size_t max_pending_bytes;
    uint64_t first_seq;
    bool order_irrelevant;
    const char *order_irrelevant_reason;
    void *user;
    uint64_t (*record_seq)(const struct bx_rg_publish_record *record, void *user);
    void (*emit_record)(void *user, struct bx_rg_publish_record *record);
    void (*dispose_record)(void *user, struct bx_rg_publish_record *record);
};

struct bx_rg_publish_state {
    struct bx_rg_publish_opts opts;
    struct bx_output_publication publication;
    bool publication_ready;
};

/*
 * Successful bx_rg_publish_submit consumes record ownership. Failed submit
 * leaves the caller responsible for disposal. Ordered publication transfers to
 * bx_output_publication's deterministic ordered path; unordered publication is
 * accepted only when the caller explicitly proves output order is irrelevant
 * for the selected rg plan.
 */
bool bx_rg_publish_init(struct bx_rg_publish_state *state,
                        const struct bx_rg_publish_opts *opts);
bool bx_rg_publish_submit(struct bx_rg_publish_state *state,
                          struct bx_rg_publish_record *record);
bool bx_rg_publish_skip_seq(struct bx_rg_publish_state *state, uint64_t seq);
void bx_rg_publish_lock_unordered_fast(struct bx_rg_publish_state *state);
void bx_rg_publish_unlock_unordered_fast(struct bx_rg_publish_state *state);
void bx_rg_publish_close(struct bx_rg_publish_state *state);
void bx_rg_publish_wake(struct bx_rg_publish_state *state);
bool bx_rg_publish_join(struct bx_rg_publish_state *state);
void bx_rg_publish_dispose(struct bx_rg_publish_state *state);
void bx_rg_publish_dispose_record(struct bx_rg_publish_record *record);
void bx_rg_publish_emit_record_default(void *user,
                                       struct bx_rg_publish_record *record);

#endif
