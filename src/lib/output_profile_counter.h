#ifndef BX_LIB_OUTPUT_PROFILE_COUNTER_H
#define BX_LIB_OUTPUT_PROFILE_COUNTER_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <pthread.h>

struct bx_output_profile_counts {
    uint_fast64_t bytes;
    uint_fast64_t records;
    uint_fast64_t flushes;
    uint_fast64_t short_writes;
    uint_fast64_t retries;
    uint_fast64_t epipe;
    uint_fast64_t allocations;
    uint_fast64_t allocation_bytes;
    uint_fast64_t formatting_ns;
};

struct bx_output_profile_sink {
    bool enabled;
    const char *name;
    pthread_mutex_t lock;
    struct bx_output_profile_counts counts;
};

bool bx_output_profile_env_enabled(void);
void bx_output_profile_sink_init(struct bx_output_profile_sink *sink,
                                 const char *name,
                                 bool enabled);
void bx_output_profile_sink_init_from_env(struct bx_output_profile_sink *sink,
                                          const char *name);
void bx_output_profile_sink_reset(struct bx_output_profile_sink *sink);
void bx_output_profile_sink_destroy(struct bx_output_profile_sink *sink);
bool bx_output_profile_sink_enabled(const struct bx_output_profile_sink *sink);
void bx_output_profile_note_bytes(struct bx_output_profile_sink *sink, uint_fast64_t bytes);
void bx_output_profile_note_record(struct bx_output_profile_sink *sink);
void bx_output_profile_note_flush(struct bx_output_profile_sink *sink);
void bx_output_profile_note_short_write(struct bx_output_profile_sink *sink);
void bx_output_profile_note_retry(struct bx_output_profile_sink *sink);
void bx_output_profile_note_epipe(struct bx_output_profile_sink *sink);
void bx_output_profile_note_allocation(struct bx_output_profile_sink *sink, uint_fast64_t bytes);
void bx_output_profile_note_formatting_ns(struct bx_output_profile_sink *sink, uint_fast64_t ns);
uint_fast64_t bx_output_profile_format_begin(const struct bx_output_profile_sink *sink);
void bx_output_profile_format_end(struct bx_output_profile_sink *sink, uint_fast64_t start_ns);
void bx_output_profile_snapshot(struct bx_output_profile_sink *sink,
                                struct bx_output_profile_counts *out);
void bx_output_profile_report(FILE *stream, struct bx_output_profile_sink *sink);
void bx_output_profile_report_stderr(struct bx_output_profile_sink *sink);

#endif /* BX_LIB_OUTPUT_PROFILE_COUNTER_H */
