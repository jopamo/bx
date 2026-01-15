#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef ZLIB_CONST
#define ZLIB_CONST 1
#endif
#include <zlib.h>

#include "applets/archive/archive_gzip.h"
#include "bx/libbx.h"
#include "lib/cancel_state.h"
#include "lib/work_pool.h"

#define BX_ARCHIVE_GZIP_IO_CHUNK 8192u

enum bx_archive_gzip_packet_status {
    BX_ARCHIVE_GZIP_PACKET_PENDING = 0,
    BX_ARCHIVE_GZIP_PACKET_OK,
    BX_ARCHIVE_GZIP_PACKET_FAILED,
    BX_ARCHIVE_GZIP_PACKET_CANCELLED,
};

struct bx_archive_gzip_mt_packet {
    unsigned char* data;
    size_t len;
    enum bx_archive_gzip_packet_status status;
};

struct bx_archive_gzip_mt_job {
    const unsigned char* input;
    size_t len;
    struct bx_archive_gzip_mt_packet* packet;
    struct bx_cancel_state* cancel;
};

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
        bx_diag(diag,
                "gzip compression failed%s%s",
                zmsg != NULL ? ": " : "",
                zmsg != NULL ? zmsg : "");
        return false;
    }
    return true;
}

static void bx_archive_gzip_mt_process_job(void* user,
                                           void* worker_local,
                                           void* job_ptr,
                                           size_t worker_index) {
    struct bx_archive_gzip_mt_job* job = job_ptr;
    struct bx_archive_buffer output;

    (void)user;
    (void)worker_local;
    (void)worker_index;

    if (bx_cancel_state_requested(job->cancel)) {
        job->packet->status = BX_ARCHIVE_GZIP_PACKET_CANCELLED;
        return;
    }

    bx_archive_buffer_init(&output);
    if (!bx_archive_gzip_compress_member(job->input, job->len, &output, NULL)) {
        bx_archive_buffer_free(&output);
        job->packet->status = BX_ARCHIVE_GZIP_PACKET_FAILED;
        bx_cancel_state_request(job->cancel);
        return;
    }

    job->packet->data = output.data;
    job->packet->len = output.len;
    job->packet->status = BX_ARCHIVE_GZIP_PACKET_OK;
}

bool bx_archive_run_gzip_filter_mt(const struct bx_archive_buffer* input,
                                   struct bx_archive_buffer* output,
                                   size_t thread_count,
                                   size_t chunk_size,
                                   struct bx_diag_ctx* diag) {
    struct bx_archive_gzip_mt_job* jobs = NULL;
    struct bx_archive_gzip_mt_packet* packets = NULL;
    struct bx_work_pool pool;
    struct bx_work_pool_opts opts;
    struct bx_cancel_state cancel;
    size_t chunk_count;
    size_t queue_capacity;
    size_t i;
    bool pool_initialized = false;
    bool ok = false;

    if (thread_count <= 1u || chunk_size == 0u || input->len <= chunk_size) {
        return bx_archive_run_gzip_filter(input, output, false, diag);
    }

    chunk_count = (input->len + chunk_size - 1u) / chunk_size;
    jobs = xmalloc(chunk_count * sizeof(*jobs));
    packets = xmalloc(chunk_count * sizeof(*packets));
    memset(packets, 0, chunk_count * sizeof(*packets));
    memset(&pool, 0, sizeof(pool));
    bx_cancel_state_init(&cancel);

    queue_capacity = thread_count > (SIZE_MAX / 4u) ? thread_count : thread_count * 4u;
    if (queue_capacity < thread_count) {
        queue_capacity = thread_count;
    }
    if (queue_capacity > chunk_count) {
        queue_capacity = chunk_count;
    }

    opts = (struct bx_work_pool_opts){
        .thread_count = thread_count,
        .queue_capacity = queue_capacity,
        .user = NULL,
        .cancel = &cancel,
        .worker_init = NULL,
        .worker_fini = NULL,
        .process_job = bx_archive_gzip_mt_process_job,
        .dispose_job = NULL,
    };
    if (!bx_work_pool_init(&pool, &opts)) {
        bx_diag(diag, "failed to initialize gzip worker pool");
        goto out;
    }
    pool_initialized = true;

    for (i = 0u; i < chunk_count; i++) {
        size_t offset = i * chunk_size;
        size_t len = input->len - offset;

        if (len > chunk_size) {
            len = chunk_size;
        }
        jobs[i] = (struct bx_archive_gzip_mt_job){
            .input = input->data + offset,
            .len = len,
            .packet = &packets[i],
            .cancel = &cancel,
        };
        if (!bx_work_pool_submit(&pool, &jobs[i])) {
            bx_cancel_state_request(&cancel);
            break;
        }
    }

    bx_work_pool_close(&pool);
    if (!bx_work_pool_join(&pool)) {
        bx_diag(diag, "gzip worker pool failed");
        goto out;
    }

    for (i = 0u; i < chunk_count; i++) {
        if (packets[i].status != BX_ARCHIVE_GZIP_PACKET_OK) {
            bx_diag(diag, "gzip compression failed");
            goto out;
        }
    }
    for (i = 0u; i < chunk_count; i++) {
        if (!bx_archive_buffer_append(output, packets[i].data, packets[i].len)) {
            bx_diag(diag, "buffer growth failed: %s", strerror(errno));
            goto out;
        }
    }

    ok = true;

out:
    if (pool_initialized) {
        bx_work_pool_dispose(&pool);
    }
    if (packets != NULL) {
        for (i = 0u; i < chunk_count; i++) {
            free(packets[i].data);
        }
    }
    free(packets);
    free(jobs);
    return ok;
}
