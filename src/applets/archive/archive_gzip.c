#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef ZLIB_CONST
#define ZLIB_CONST 1
#endif
#include <zlib.h>

#include "applets/archive/archive_gzip.h"
#include "lib/ordered_publication.h"
#include "bx/libbx.h"
#include "lib/backpressure_limit.h"
#include "lib/cancel_state.h"
#include "lib/time_parse.h"
#include "lib/work_pool.h"

#define BX_ARCHIVE_GZIP_IO_CHUNK 8192u

enum bx_archive_gzip_packet_status {
    BX_ARCHIVE_GZIP_PACKET_PENDING = 0,
    BX_ARCHIVE_GZIP_PACKET_OK,
    BX_ARCHIVE_GZIP_PACKET_FAILED,
    BX_ARCHIVE_GZIP_PACKET_CANCELLED,
};

struct bx_archive_gzip_stream_state;

struct bx_archive_gzip_stream_job {
    struct bx_ordered_publication_packet ordered;
    unsigned char* input;
    size_t input_len;
    unsigned char* compressed;
    size_t compressed_len;
    enum bx_archive_gzip_packet_status status;
};

struct bx_archive_gzip_stream_state {
    const struct bx_archive_gzip_stream_sink* output_sink;
    struct bx_diag_ctx* diag;
    struct bx_work_pool pool;
    struct bx_work_pool_opts pool_opts;
    struct bx_cancel_state cancel;
    struct bx_ordered_publication_state ordered;
    struct bx_archive_buffer current_chunk;
    size_t chunk_size;
    size_t max_inflight_chunks;
    unsigned int test_delay_first_chunk_ms;
    bool pool_initialized;
};

struct bx_archive_gzip_filter_stream_state {
    const struct bx_archive_gzip_stream_sink* output_sink;
    struct bx_diag_ctx* diag;
    z_stream stream;
    bool stream_initialized;
};

static void bx_archive_gzip_diag_compression_failed(struct bx_diag_ctx* diag, const char* zmsg) {
    bx_diag(diag,
            "gzip compression failed%s%s",
            zmsg != NULL ? ": " : "",
            zmsg != NULL ? zmsg : "");
}

static unsigned int bx_archive_gzip_test_delay_first_chunk_ms(void) {
    const char* value = getenv("BX_TAR_TEST_GZIP_DELAY_FIRST_CHUNK_MS");
    char* end = NULL;
    unsigned long parsed;

    if (value == NULL || *value == '\0') {
        return 0u;
    }

    errno = 0;
    parsed = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed > UINT_MAX) {
        return 0u;
    }

    return (unsigned int)parsed;
}

static void bx_archive_gzip_test_delay_job(const struct bx_archive_gzip_stream_state* state,
                                           const struct bx_archive_gzip_stream_job* job) {
    struct timespec ts;

    if (state->test_delay_first_chunk_ms == 0u || job->ordered.seq != 0u) {
        return;
    }

    if (!bx_time_milliseconds_to_timespec((intmax_t)state->test_delay_first_chunk_ms, &ts)) {
        return;
    }
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

static bool bx_archive_gzip_compress_member(const unsigned char* input,
                                            size_t input_len,
                                            struct bx_archive_buffer* output,
                                            const char** zmsg_out) {
    z_stream stream;
    size_t input_pos = 0u;
    int rc;

    memset(&stream, 0, sizeof(stream));
    rc = deflateInit2(&stream,
                      Z_DEFAULT_COMPRESSION,
                      Z_DEFLATED,
                      MAX_WBITS + 16,
                      8,
                      Z_DEFAULT_STRATEGY);
    if (rc != Z_OK) {
        if (zmsg_out != NULL) {
            *zmsg_out = stream.msg;
        }
        return false;
    }

    do {
        unsigned char outbuf[BX_ARCHIVE_GZIP_IO_CHUNK];
        int flush = Z_NO_FLUSH;
        size_t produced;

        if (stream.avail_in == 0u && input_pos < input_len) {
            size_t chunk = input_len - input_pos;
            if (chunk > UINT_MAX) {
                chunk = UINT_MAX;
            }
            stream.next_in = input + input_pos;
            stream.avail_in = (uInt)chunk;
            input_pos += chunk;
        }
        if (input_pos == input_len && stream.avail_in == 0u) {
            flush = Z_FINISH;
        }

        stream.next_out = outbuf;
        stream.avail_out = sizeof(outbuf);
        rc = deflate(&stream, flush);
        if (rc != Z_OK && rc != Z_STREAM_END) {
            if (zmsg_out != NULL) {
                *zmsg_out = stream.msg;
            }
            deflateEnd(&stream);
            return false;
        }

        produced = sizeof(outbuf) - stream.avail_out;
        if (produced > 0u && !bx_archive_buffer_append(output, outbuf, produced)) {
            if (zmsg_out != NULL) {
                *zmsg_out = "buffer growth failed";
            }
            deflateEnd(&stream);
            return false;
        }
    } while (rc != Z_STREAM_END);

    deflateEnd(&stream);
    return true;
}

static bool bx_archive_gzip_decompress_all(const unsigned char* input,
                                           size_t input_len,
                                           struct bx_archive_buffer* output,
                                           struct bx_diag_ctx* diag) {
    z_stream stream;
    size_t input_pos = 0u;
    int rc;

    memset(&stream, 0, sizeof(stream));
    rc = inflateInit2(&stream, MAX_WBITS + 16);
    if (rc != Z_OK) {
        bx_diag(diag, "gzip decompression failed");
        return false;
    }

    for (;;) {
        unsigned char outbuf[BX_ARCHIVE_GZIP_IO_CHUNK];
        size_t produced;

        if (stream.avail_in == 0u && input_pos < input_len) {
            size_t chunk = input_len - input_pos;
            if (chunk > UINT_MAX) {
                chunk = UINT_MAX;
            }
            stream.next_in = input + input_pos;
            stream.avail_in = (uInt)chunk;
            input_pos += chunk;
        }

        stream.next_out = outbuf;
        stream.avail_out = sizeof(outbuf);
        rc = inflate(&stream, Z_NO_FLUSH);
        produced = sizeof(outbuf) - stream.avail_out;
        if (produced > 0u && !bx_archive_buffer_append(output, outbuf, produced)) {
            inflateEnd(&stream);
            bx_diag(diag, "buffer growth failed: %s", strerror(errno));
            return false;
        }

        if (rc == Z_STREAM_END) {
            if (input_pos == input_len && stream.avail_in == 0u) {
                break;
            }
            rc = inflateReset(&stream);
            if (rc != Z_OK) {
                inflateEnd(&stream);
                bx_diag(diag, "gzip decompression failed");
                return false;
            }
            continue;
        }

        if (rc == Z_OK) {
            if (input_pos == input_len && stream.avail_in == 0u && produced == 0u) {
                inflateEnd(&stream);
                bx_diag(diag, "gzip decompression failed");
                return false;
            }
            continue;
        }

        if (rc == Z_BUF_ERROR && (input_pos < input_len || stream.avail_in > 0u)) {
            continue;
        }

        inflateEnd(&stream);
        bx_diag(diag,
                "gzip decompression failed%s%s",
                stream.msg != NULL ? ": " : "",
                stream.msg != NULL ? stream.msg : "");
        return false;
    }

    inflateEnd(&stream);
    return true;
}

bool bx_archive_run_gzip_filter(const struct bx_archive_buffer* input,
                                struct bx_archive_buffer* output,
                                bool decompress,
                                struct bx_diag_ctx* diag) {
    const char* zmsg = NULL;

    if (decompress) {
        return bx_archive_gzip_decompress_all(input->data, input->len, output, diag);
    }

    if (!bx_archive_gzip_compress_member(input->data, input->len, output, &zmsg)) {
        bx_archive_gzip_diag_compression_failed(diag, zmsg);
        return false;
    }
    return true;
}

static bool bx_archive_gzip_filter_stream_write_output(struct bx_archive_gzip_filter_stream_state* state,
                                                       const unsigned char* data,
                                                       size_t len) {
    if (len == 0u) {
        return true;
    }
    if (!state->output_sink->write(state->output_sink->user, data, len)) {
        bx_diag(state->diag, "write error: %s", strerror(errno));
        return false;
    }
    return true;
}

static bool bx_archive_gzip_filter_stream_feed(struct bx_archive_gzip_filter_stream_state* state,
                                               const unsigned char* data,
                                               size_t len) {
    while (len > 0u) {
        unsigned char outbuf[BX_ARCHIVE_GZIP_IO_CHUNK];
        size_t chunk = len > UINT_MAX ? UINT_MAX : len;
        size_t produced;
        int rc;

        state->stream.next_in = data;
        state->stream.avail_in = (uInt)chunk;
        data += chunk;
        len -= chunk;

        while (state->stream.avail_in > 0u) {
            state->stream.next_out = outbuf;
            state->stream.avail_out = sizeof(outbuf);
            rc = deflate(&state->stream, Z_NO_FLUSH);
            if (rc != Z_OK) {
                bx_archive_gzip_diag_compression_failed(state->diag, state->stream.msg);
                return false;
            }

            produced = sizeof(outbuf) - state->stream.avail_out;
            if (!bx_archive_gzip_filter_stream_write_output(state, outbuf, produced)) {
                return false;
            }
        }
    }

    return true;
}

static bool bx_archive_gzip_filter_stream_finish(struct bx_archive_gzip_filter_stream_state* state) {
    int rc;

    do {
        unsigned char outbuf[BX_ARCHIVE_GZIP_IO_CHUNK];
        size_t produced;

        state->stream.next_out = outbuf;
        state->stream.avail_out = sizeof(outbuf);
        rc = deflate(&state->stream, Z_FINISH);
        if (rc != Z_OK && rc != Z_STREAM_END) {
            bx_archive_gzip_diag_compression_failed(state->diag, state->stream.msg);
            return false;
        }

        produced = sizeof(outbuf) - state->stream.avail_out;
        if (!bx_archive_gzip_filter_stream_write_output(state, outbuf, produced)) {
            return false;
        }
    } while (rc != Z_STREAM_END);

    return true;
}

static bool bx_archive_gzip_filter_stream_input_write(void* user, const void* data, size_t len) {
    struct bx_archive_gzip_filter_stream_state* state = user;
    return bx_archive_gzip_filter_stream_feed(state, data, len);
}

bool bx_archive_run_gzip_filter_stream(bx_archive_gzip_stream_producer_fn producer,
                                       void* producer_user,
                                       const struct bx_archive_gzip_stream_sink* output_sink,
                                       struct bx_diag_ctx* diag) {
    struct bx_archive_gzip_filter_stream_state state;
    struct bx_archive_gzip_stream_sink input_sink;
    int rc;
    bool ok = false;

    if (producer == NULL || output_sink == NULL || output_sink->write == NULL) {
        bx_diag(diag, "invalid gzip stream configuration");
        return false;
    }

    memset(&state, 0, sizeof(state));
    state.output_sink = output_sink;
    state.diag = diag;

    rc = deflateInit2(&state.stream,
                      Z_DEFAULT_COMPRESSION,
                      Z_DEFLATED,
                      MAX_WBITS + 16,
                      8,
                      Z_DEFAULT_STRATEGY);
    if (rc != Z_OK) {
        bx_archive_gzip_diag_compression_failed(diag, state.stream.msg);
        return false;
    }
    state.stream_initialized = true;

    input_sink.user = &state;
    input_sink.write = bx_archive_gzip_filter_stream_input_write;
    if (!producer(producer_user, &input_sink, diag)) {
        goto out;
    }

    ok = bx_archive_gzip_filter_stream_finish(&state);

out:
    if (state.stream_initialized) {
        deflateEnd(&state.stream);
    }
    return ok;
}

static void bx_archive_gzip_stream_job_free(struct bx_archive_gzip_stream_job* job) {
    if (job == NULL) {
        return;
    }
    free(job->input);
    free(job->compressed);
    free(job);
}

static void bx_archive_gzip_stream_dispose_job(void* user, void* job_ptr) {
    (void)user;
    bx_archive_gzip_stream_job_free(job_ptr);
}

static void bx_archive_gzip_stream_dispose_packet(void* user,
                                                  struct bx_ordered_publication_packet* packet) {
    (void)user;
    bx_archive_gzip_stream_job_free((struct bx_archive_gzip_stream_job*)packet);
}

static bool bx_archive_gzip_stream_publish_packet(void* user,
                                                  struct bx_ordered_publication_packet* packet) {
    struct bx_archive_gzip_stream_state* state = user;
    struct bx_archive_gzip_stream_job* job = (struct bx_archive_gzip_stream_job*)packet;

    if (job->status != BX_ARCHIVE_GZIP_PACKET_OK) {
        bx_diag(state->diag, "gzip compression failed");
        return false;
    }

    if (!state->output_sink->write(state->output_sink->user, job->compressed, job->compressed_len)) {
        bx_diag(state->diag, "write error: %s", strerror(errno));
        return false;
    }

    return true;
}

static void bx_archive_gzip_stream_process_job(void* user,
                                               void* worker_local,
                                               void* job_ptr,
                                               size_t worker_index) {
    struct bx_archive_gzip_stream_state* state = user;
    struct bx_archive_gzip_stream_job* job = job_ptr;
    struct bx_archive_buffer output;

    (void)worker_local;
    (void)worker_index;

    if (bx_cancel_state_requested(&state->cancel)) {
        job->status = BX_ARCHIVE_GZIP_PACKET_CANCELLED;
    }
    else {
        bx_archive_buffer_init(&output);
        if (!bx_archive_gzip_compress_member(job->input, job->input_len, &output, NULL)) {
            bx_archive_buffer_free(&output);
            job->status = BX_ARCHIVE_GZIP_PACKET_FAILED;
            bx_cancel_state_request(&state->cancel);
        }
        else {
            job->compressed = output.data;
            job->compressed_len = output.len;
            job->status = BX_ARCHIVE_GZIP_PACKET_OK;
            bx_archive_gzip_test_delay_job(state, job);
        }
    }

    bx_ordered_publication_publish_packet(&state->ordered, &job->ordered);
}

static bool bx_archive_gzip_stream_submit_chunk(struct bx_archive_gzip_stream_state* state) {
    struct bx_archive_gzip_stream_job* job;
    uint64_t seq;

    if (state->current_chunk.len == 0u) {
        return true;
    }
    if (!bx_ordered_publication_reserve_slot(&state->ordered, &seq)) {
        return false;
    }

    job = xmalloc(sizeof(*job));
    memset(job, 0, sizeof(*job));
    job->ordered.seq = seq;
    job->input = state->current_chunk.data;
    job->input_len = state->current_chunk.len;
    state->current_chunk.data = NULL;
    state->current_chunk.len = 0u;
    state->current_chunk.cap = 0u;

    if (!bx_work_pool_submit(&state->pool, job)) {
        bx_cancel_state_request(&state->cancel);
        bx_diag(state->diag, "failed to submit gzip compression work");
        bx_ordered_publication_release_slot(&state->ordered);
        bx_archive_gzip_stream_job_free(job);
        return false;
    }
    return true;
}

static bool bx_archive_gzip_stream_input_write(void* user, const void* data, size_t len) {
    struct bx_archive_gzip_stream_state* state = user;
    const unsigned char* cursor = data;
    size_t remaining = len;

    while (remaining > 0u) {
        size_t space = state->chunk_size - state->current_chunk.len;
        size_t chunk = remaining < space ? remaining : space;

        if (!bx_archive_buffer_append(&state->current_chunk, cursor, chunk)) {
            bx_diag(state->diag, "buffer growth failed: %s", strerror(errno));
            return false;
        }
        cursor += chunk;
        remaining -= chunk;

        if (state->current_chunk.len == state->chunk_size
            && !bx_archive_gzip_stream_submit_chunk(state)) {
            return false;
        }
    }

    return true;
}

static void bx_archive_gzip_stream_state_cleanup(struct bx_archive_gzip_stream_state* state) {
    if (state->pool_initialized) {
        bx_work_pool_dispose(&state->pool);
    }
    bx_ordered_publication_cleanup(&state->ordered);
    bx_archive_buffer_free(&state->current_chunk);
}

bool bx_archive_run_gzip_filter_mt_stream(bx_archive_gzip_stream_producer_fn producer,
                                          void* producer_user,
                                          const struct bx_archive_gzip_stream_sink* output_sink,
                                          size_t thread_count,
                                          size_t chunk_size,
                                          size_t max_inflight_chunks,
                                          struct bx_diag_ctx* diag) {
    struct bx_archive_gzip_stream_state state;
    struct bx_archive_gzip_stream_sink input_sink;
    bool ok = false;

    if (producer == NULL || output_sink == NULL || output_sink->write == NULL
        || thread_count == 0u || chunk_size == 0u) {
        bx_diag(diag, "invalid multithreaded gzip stream configuration");
        return false;
    }

    memset(&state, 0, sizeof(state));
    state.output_sink = output_sink;
    state.diag = diag;
    state.chunk_size = chunk_size;
    state.max_inflight_chunks = max_inflight_chunks != 0u ? max_inflight_chunks : thread_count * 4u;
    size_t archive_member_limit =
        bx_backpressure_limit_default(BX_BACKPRESSURE_LIMIT_PENDING_ARCHIVE_MEMBERS);
    if (archive_member_limit > 0u && state.max_inflight_chunks > archive_member_limit)
        state.max_inflight_chunks = archive_member_limit;
    state.test_delay_first_chunk_ms = bx_archive_gzip_test_delay_first_chunk_ms();
    if (state.max_inflight_chunks < thread_count) {
        state.max_inflight_chunks = thread_count;
    }
    bx_cancel_state_init(&state.cancel);
    bx_archive_buffer_init(&state.current_chunk);

    state.pool_opts = (struct bx_work_pool_opts){
        .thread_count = thread_count,
        .queue_capacity = state.max_inflight_chunks,
        .queue_limit_kind = BX_BACKPRESSURE_LIMIT_PENDING_ARCHIVE_MEMBERS,
        .user = &state,
        .cancel = &state.cancel,
        .worker_init = NULL,
        .worker_fini = NULL,
        .process_job = bx_archive_gzip_stream_process_job,
        .dispose_job = bx_archive_gzip_stream_dispose_job,
    };
    if (!bx_work_pool_init(&state.pool, &state.pool_opts)) {
        bx_diag(diag, "failed to initialize gzip worker pool");
        goto out;
    }
    state.pool_initialized = true;

    if (!bx_ordered_publication_init(&state.ordered,
                                 &(struct bx_ordered_publication_opts){
                                     .cancel = &state.cancel,
                                     .max_inflight = state.max_inflight_chunks,
                                     .user = &state,
                                     .publish = bx_archive_gzip_stream_publish_packet,
                                     .dispose = bx_archive_gzip_stream_dispose_packet,
                                 })) {
        bx_diag(diag, "failed to initialize ordered gzip publication");
        goto out;
    }

    input_sink.user = &state;
    input_sink.write = bx_archive_gzip_stream_input_write;
    if (!producer(producer_user, &input_sink, diag)) {
        bx_cancel_state_request(&state.cancel);
        goto out_join;
    }
    if (!bx_archive_gzip_stream_submit_chunk(&state)) {
        goto out_join;
    }

    bx_work_pool_close(&state.pool);
    if (!bx_ordered_publication_drain(&state.ordered)) {
        goto out_join_no_ok;
    }
    ok = true;

out_join_no_ok:
out_join:
    bx_work_pool_close(&state.pool);
    if (!bx_work_pool_join(&state.pool) && ok) {
        bx_diag(diag, "gzip worker pool failed");
        ok = false;
    }

out:
    if (bx_cancel_state_requested(&state.cancel))
        (void)bx_cancel_state_mark_published(&state.cancel);
    bx_archive_gzip_stream_state_cleanup(&state);
    return ok;
}
