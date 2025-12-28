#ifndef BX_LIB_OUTPUT_SINK_H
#define BX_LIB_OUTPUT_SINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

struct bx_output_sink_opts {
    size_t max_pending;
    uint64_t first_seq;
    bool ordered;
    void *user;
    uint64_t (*record_seq)(const void *record, void *user);
    void (*emit_record)(void *user, void *record);
    void (*dispose_record)(void *user, void *record);
};

struct bx_output_sink {
    struct bx_output_sink_opts opts;
    pthread_mutex_t lock;
    pthread_cond_t can_submit;
    pthread_cond_t can_emit;
    pthread_t thread;
    struct bx_output_sink_node *head;
    size_t pending;
    uint64_t next_seq;
    bool closed;
    bool failed;
    bool started;
};

bool bx_output_sink_init(struct bx_output_sink *sink, const struct bx_output_sink_opts *opts);
bool bx_output_sink_submit(struct bx_output_sink *sink, void *record);
void bx_output_sink_close(struct bx_output_sink *sink);
void bx_output_sink_wake(struct bx_output_sink *sink);
bool bx_output_sink_join(struct bx_output_sink *sink);
void bx_output_sink_dispose(struct bx_output_sink *sink);

#endif
