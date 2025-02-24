#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "applets.h"
#include "diag.h"
#include "libbx.h"
#include "common/size_parse.h"
#include "common/xreadwrite.h"

enum {
    BX_DD_CONV_SYNC = 1u << 0,
    BX_DD_CONV_NOERROR = 1u << 1,
    BX_DD_CONV_NOTRUNC = 1u << 2,
    BX_DD_CONV_FSYNC = 1u << 3,
    BX_DD_CONV_FDATASYNC = 1u << 4,
};

enum {
    BX_DD_IFLAG_FULLBLOCK = 1u << 0,
};

enum {
    BX_DD_STATUS_NONE = 1u << 0,
    BX_DD_STATUS_NOXFER = 1u << 1,
    BX_DD_STATUS_PROGRESS = 1u << 2,
};

enum bx_dd_mode {
    BX_DD_MODE_RUN = 0,
    BX_DD_MODE_HELP,
    BX_DD_MODE_VERSION,
};

struct bx_dd_config {
    const char* ifile;
    const char* ofile;

    uintmax_t ibs;
    uintmax_t obs;
    uintmax_t cbs;
    uintmax_t count;
    uintmax_t skip;
    uintmax_t seek;

    bool count_set;
    bool skip_set;
    bool seek_set;

    unsigned conv_mask;
    unsigned iflag_mask;
    unsigned oflag_mask;
    unsigned status_mask;

    mode_t create_mode;
};

struct bx_dd_stats {
    uintmax_t full_in;
    uintmax_t partial_in;
    uintmax_t full_out;
    uintmax_t partial_out;
    uintmax_t bytes_copied;
};

struct bx_dd_ctx {
    const char* progname;

    struct bx_dd_config cfg;
    struct bx_dd_stats st;

    int infd;
    int outfd;
    const char* input_path;
    const char* output_path;

    unsigned char* ibuf;
    unsigned char* obuf;
    size_t obuf_len;

    bool time_ready;
    struct timespec start_time;
    struct timespec last_progress_time;

    bool should_print_stats;
};

static const char* bx_dd_progname(const char* argv0) {
    if (argv0 == NULL || argv0[0] == '\0') {
        return "dd";
    }

    const char* base = strrchr(argv0, '/');
    if (base != NULL && base[1] != '\0') {
        return base + 1;
    }

    return argv0;
}

static void bx_dd_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPERAND]...\n", progname);
    fprintf(stream, "Copy data, converting and formatting according to operands.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  if=FILE               read from FILE instead of standard input\n");
    fprintf(stream, "  of=FILE               write to FILE instead of standard output\n");
    fprintf(stream, "  ibs=BYTES             read up to BYTES bytes at a time (default: 512)\n");
    fprintf(stream, "  obs=BYTES             write BYTES bytes at a time (default: 512)\n");
    fprintf(stream, "  bs=BYTES              set both ibs and obs to BYTES\n");
    fprintf(stream, "  count=N               copy only N input blocks\n");
    fprintf(stream, "  skip=N                skip N input blocks of size ibs\n");
    fprintf(stream, "  seek=N                skip N output blocks of size obs\n");
    fprintf(stream, "  conv=LIST             conversions: sync,noerror,notrunc,fsync,fdatasync\n");
    fprintf(stream, "  iflag=LIST            input flags: fullblock\n");
    fprintf(stream, "  status=LEVEL          none, noxfer, or progress\n");
    fprintf(stream, "\n");
    fprintf(stream, "  --help                display this help and exit\n");
    fprintf(stream, "  --version             output version information and exit\n");
}

static void bx_dd_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

static void bx_dd_vdiag(const char* progname, const char* fmt, va_list ap) {
    fprintf(stderr, "%s: ", progname);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
}

static void bx_dd_diag(const char* progname, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    bx_dd_vdiag(progname, fmt, ap);
    va_end(ap);
}

static void bx_dd_perror_path(const char* progname, const char* path) {
    fprintf(stderr, "%s: %s: %s\n", progname, path, strerror(errno));
}

static void bx_dd_perror_with_errno(const char* progname, const char* path, int errnum) {
    fprintf(stderr, "%s: %s: %s\n", progname, path, strerror(errnum));
}

static bool bx_dd_key_eq(const char* key, size_t key_len, const char* expected) {
    return strlen(expected) == key_len && strncmp(key, expected, key_len) == 0;
}

static bool bx_dd_safe_mul(uintmax_t a, uintmax_t b, uintmax_t* out) {
    if (out == NULL) {
        return false;
    }

    if (a != 0 && b > UINTMAX_MAX / a) {
        return false;
    }

    *out = a * b;
    return true;
}

static bool bx_dd_u64_to_off_t(uintmax_t value, off_t* out) {
    if (out == NULL) {
        return false;
    }

    off_t converted = (off_t)value;
    if (converted < 0 || (uintmax_t)converted != value) {
        return false;
    }

    *out = converted;
    return true;
}

static bool bx_dd_parse_nonzero_size(const char* text, uintmax_t* value_out, const char* name, const char* progname) {
    uintmax_t value = 0;
    if (!bx_dd_parse_size(text, &value)) {
        bx_dd_diag(progname, "invalid %s value '%s'", name, (text != NULL) ? text : "");
        return false;
    }

    if (value == 0) {
        bx_dd_diag(progname, "%s must be greater than zero", name);
        return false;
    }

    *value_out = value;
    return true;
}

static bool bx_dd_parse_count_like(const char* text, uintmax_t* value_out, const char* name, const char* progname) {
    uintmax_t value = 0;
    if (!bx_dd_parse_size(text, &value)) {
        bx_dd_diag(progname, "invalid %s value '%s'", name, (text != NULL) ? text : "");
        return false;
    }

    *value_out = value;
    return true;
}

static bool bx_dd_parse_conv(struct bx_dd_config* cfg, const char* value, const char* progname) {
    char* copy = xstrdup(value);
    char* saveptr = NULL;
    char* token = strtok_r(copy, ",", &saveptr);

    if (token == NULL) {
        bx_dd_diag(progname, "invalid conversion list");
        free(copy);
        return false;
    }

    while (token != NULL) {
        if (token[0] == '\0') {
            bx_dd_diag(progname, "invalid conversion list");
            free(copy);
            return false;
        }

        if (strcmp(token, "sync") == 0) {
            cfg->conv_mask |= BX_DD_CONV_SYNC;
        }
        else if (strcmp(token, "noerror") == 0) {
            cfg->conv_mask |= BX_DD_CONV_NOERROR;
        }
        else if (strcmp(token, "notrunc") == 0) {
            cfg->conv_mask |= BX_DD_CONV_NOTRUNC;
        }
        else if (strcmp(token, "fsync") == 0) {
            cfg->conv_mask |= BX_DD_CONV_FSYNC;
        }
        else if (strcmp(token, "fdatasync") == 0) {
            cfg->conv_mask |= BX_DD_CONV_FDATASYNC;
        }
        else {
            bx_dd_diag(progname, "unsupported conversion '%s'", token);
            free(copy);
            return false;
        }

        token = strtok_r(NULL, ",", &saveptr);
    }

    free(copy);
    return true;
}

static bool bx_dd_parse_iflag(struct bx_dd_config* cfg, const char* value, const char* progname) {
    char* copy = xstrdup(value);
    char* saveptr = NULL;
    char* token = strtok_r(copy, ",", &saveptr);

    if (token == NULL) {
        bx_dd_diag(progname, "invalid input flag list");
        free(copy);
        return false;
    }

    while (token != NULL) {
        if (token[0] == '\0') {
            bx_dd_diag(progname, "invalid input flag list");
            free(copy);
            return false;
        }

        if (strcmp(token, "fullblock") == 0) {
            cfg->iflag_mask |= BX_DD_IFLAG_FULLBLOCK;
        }
        else {
            bx_dd_diag(progname, "unsupported input flag '%s'", token);
            free(copy);
            return false;
        }

        token = strtok_r(NULL, ",", &saveptr);
    }

    free(copy);
    return true;
}

static bool bx_dd_parse_status(struct bx_dd_config* cfg, const char* value, const char* progname) {
    char* copy = xstrdup(value);
    char* saveptr = NULL;
    char* token = strtok_r(copy, ",", &saveptr);

    if (token == NULL) {
        bx_dd_diag(progname, "invalid status level");
        free(copy);
        return false;
    }

    while (token != NULL) {
        if (token[0] == '\0') {
            bx_dd_diag(progname, "invalid status level");
            free(copy);
            return false;
        }

        if (strcmp(token, "none") == 0) {
            cfg->status_mask = BX_DD_STATUS_NONE;
        }
        else if (strcmp(token, "noxfer") == 0) {
            cfg->status_mask = BX_DD_STATUS_NOXFER;
        }
        else if (strcmp(token, "progress") == 0) {
            cfg->status_mask = BX_DD_STATUS_PROGRESS;
        }
        else {
            bx_dd_diag(progname, "invalid status level '%s'", token);
            free(copy);
            return false;
        }

        token = strtok_r(NULL, ",", &saveptr);
    }

    free(copy);
    return true;
}

static bool bx_dd_parse_assignment(struct bx_dd_config* cfg, const char* arg, const char* progname) {
    const char* eq = strchr(arg, '=');
    if (eq == NULL || eq == arg) {
        bx_dd_diag(progname, "unrecognized operand '%s'", arg);
        return false;
    }

    size_t key_len = (size_t)(eq - arg);
    const char* value = eq + 1;

    if (bx_dd_key_eq(arg, key_len, "if")) {
        cfg->ifile = value;
        return true;
    }

    if (bx_dd_key_eq(arg, key_len, "of")) {
        cfg->ofile = value;
        return true;
    }

    if (bx_dd_key_eq(arg, key_len, "ibs")) {
        return bx_dd_parse_nonzero_size(value, &cfg->ibs, "ibs", progname);
    }

    if (bx_dd_key_eq(arg, key_len, "obs")) {
        return bx_dd_parse_nonzero_size(value, &cfg->obs, "obs", progname);
    }

    if (bx_dd_key_eq(arg, key_len, "cbs")) {
        return bx_dd_parse_nonzero_size(value, &cfg->cbs, "cbs", progname);
    }

    if (bx_dd_key_eq(arg, key_len, "bs")) {
        uintmax_t bs = 0;
        if (!bx_dd_parse_nonzero_size(value, &bs, "bs", progname)) {
            return false;
        }
        cfg->ibs = bs;
        cfg->obs = bs;
        return true;
    }

    if (bx_dd_key_eq(arg, key_len, "count")) {
        if (!bx_dd_parse_count_like(value, &cfg->count, "count", progname)) {
            return false;
        }
        cfg->count_set = true;
        return true;
    }

    if (bx_dd_key_eq(arg, key_len, "skip")) {
        if (!bx_dd_parse_count_like(value, &cfg->skip, "skip", progname)) {
            return false;
        }
        cfg->skip_set = true;
        return true;
    }

    if (bx_dd_key_eq(arg, key_len, "seek")) {
        if (!bx_dd_parse_count_like(value, &cfg->seek, "seek", progname)) {
            return false;
        }
        cfg->seek_set = true;
        return true;
    }

    if (bx_dd_key_eq(arg, key_len, "conv")) {
        return bx_dd_parse_conv(cfg, value, progname);
    }

    if (bx_dd_key_eq(arg, key_len, "iflag")) {
        return bx_dd_parse_iflag(cfg, value, progname);
    }

    if (bx_dd_key_eq(arg, key_len, "status")) {
        return bx_dd_parse_status(cfg, value, progname);
    }

    if (bx_dd_key_eq(arg, key_len, "oflag")) {
        bx_dd_diag(progname, "unsupported output flag operand '%s'", arg);
        return false;
    }

    bx_dd_diag(progname, "unrecognized operand '%s'", arg);
    return false;
}

static bool bx_dd_parse_args(struct bx_dd_config* cfg, int argc, char** argv, enum bx_dd_mode* mode_out, const char* progname) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->ibs = 512u;
    cfg->obs = 512u;
    cfg->create_mode = 0666u;

    *mode_out = BX_DD_MODE_RUN;

    for (int i = 1; i < argc; i++) {
        const char* arg = argv[i];

        if (strcmp(arg, "--help") == 0) {
            *mode_out = BX_DD_MODE_HELP;
            return true;
        }

        if (strcmp(arg, "--version") == 0) {
            *mode_out = BX_DD_MODE_VERSION;
            return true;
        }

        if (arg[0] == '-' && arg[1] != '\0') {
            bx_dd_diag(progname, "unrecognized option '%s'", arg);
            return false;
        }

        if (!bx_dd_parse_assignment(cfg, arg, progname)) {
            return false;
        }
    }

    return true;
}

static bool bx_dd_open_files(struct bx_dd_ctx* ctx) {
    ctx->input_path = (ctx->cfg.ifile != NULL) ? ctx->cfg.ifile : "/dev/stdin";
    ctx->output_path = (ctx->cfg.ofile != NULL) ? ctx->cfg.ofile : "/dev/stdout";

    int in_flags = O_RDONLY;
    ctx->infd = open(ctx->input_path, in_flags);
    if (ctx->infd < 0) {
        bx_dd_perror_path(ctx->progname, ctx->input_path);
        return false;
    }

    int out_flags = O_WRONLY | O_CREAT;
    if ((ctx->cfg.conv_mask & BX_DD_CONV_NOTRUNC) == 0u && !ctx->cfg.seek_set) {
        out_flags |= O_TRUNC;
    }

    ctx->outfd = open(ctx->output_path, out_flags, ctx->cfg.create_mode);
    if (ctx->outfd < 0) {
        bx_dd_perror_path(ctx->progname, ctx->output_path);
        return false;
    }

    return true;
}

static bool bx_dd_discard_input_bytes(struct bx_dd_ctx* ctx, uintmax_t byte_count) {
    unsigned char discard_buf[65536];

    while (byte_count > 0) {
        size_t chunk = sizeof(discard_buf);
        if ((uintmax_t)chunk > byte_count) {
            chunk = (size_t)byte_count;
        }

        ssize_t nread = bx_xread(ctx->infd, discard_buf, chunk);
        if (nread < 0) {
            bx_dd_perror_path(ctx->progname, ctx->input_path);
            return false;
        }

        if (nread == 0) {
            break;
        }

        byte_count -= (uintmax_t)nread;
    }

    return true;
}

static bool bx_dd_apply_skip_seek(struct bx_dd_ctx* ctx) {
    if (ctx->cfg.skip_set && ctx->cfg.skip > 0) {
        uintmax_t skip_bytes = 0;
        if (!bx_dd_safe_mul(ctx->cfg.skip, ctx->cfg.ibs, &skip_bytes)) {
            bx_dd_diag(ctx->progname, "skip offset overflow");
            return false;
        }

        off_t skip_offset = 0;
        bool offset_ok = bx_dd_u64_to_off_t(skip_bytes, &skip_offset);

        if (!offset_ok || lseek(ctx->infd, skip_offset, SEEK_CUR) < 0) {
            if (offset_ok && errno != ESPIPE && errno != EINVAL) {
                bx_dd_perror_path(ctx->progname, ctx->input_path);
                return false;
            }

            if (!bx_dd_discard_input_bytes(ctx, skip_bytes)) {
                return false;
            }
        }
    }

    if (ctx->cfg.seek_set && ctx->cfg.seek > 0) {
        uintmax_t seek_bytes = 0;
        if (!bx_dd_safe_mul(ctx->cfg.seek, ctx->cfg.obs, &seek_bytes)) {
            bx_dd_diag(ctx->progname, "seek offset overflow");
            return false;
        }

        off_t seek_offset = 0;
        if (!bx_dd_u64_to_off_t(seek_bytes, &seek_offset)) {
            bx_dd_diag(ctx->progname, "seek offset overflow");
            return false;
        }

        if (lseek(ctx->outfd, seek_offset, SEEK_CUR) < 0) {
            bx_dd_perror_path(ctx->progname, ctx->output_path);
            return false;
        }
    }

    return true;
}

static bool bx_dd_finalize_output_size(struct bx_dd_ctx* ctx) {
    if (!ctx->cfg.seek_set || (ctx->cfg.conv_mask & BX_DD_CONV_NOTRUNC) != 0u) {
        return true;
    }

    struct stat st;
    if (fstat(ctx->outfd, &st) != 0) {
        bx_dd_perror_path(ctx->progname, ctx->output_path);
        return false;
    }

    if (!S_ISREG(st.st_mode)) {
        return true;
    }

    off_t end_pos = lseek(ctx->outfd, 0, SEEK_CUR);
    if (end_pos < 0) {
        bx_dd_perror_path(ctx->progname, ctx->output_path);
        return false;
    }

    if (ftruncate(ctx->outfd, end_pos) != 0) {
        bx_dd_perror_path(ctx->progname, ctx->output_path);
        return false;
    }

    return true;
}

static bool bx_dd_now(struct timespec* ts_out) {
    if (ts_out == NULL) {
        return false;
    }

    if (clock_gettime(CLOCK_MONOTONIC, ts_out) != 0) {
        return false;
    }

    return true;
}

static double bx_dd_elapsed_seconds(const struct timespec* start, const struct timespec* end) {
    time_t sec = end->tv_sec - start->tv_sec;
    long nsec = end->tv_nsec - start->tv_nsec;

    if (nsec < 0) {
        sec -= 1;
        nsec += 1000000000L;
    }

    if (sec < 0) {
        return 0.0;
    }

    return (double)sec + (double)nsec / 1000000000.0;
}

static void bx_dd_print_progress_line(struct bx_dd_ctx* ctx, const struct timespec* now) {
    if (ctx->cfg.status_mask != BX_DD_STATUS_PROGRESS || !ctx->time_ready) {
        return;
    }

    double elapsed = bx_dd_elapsed_seconds(&ctx->start_time, now);
    if (elapsed <= 0.0) {
        elapsed = 0.000001;
    }

    double bytes_per_sec = (double)ctx->st.bytes_copied / elapsed;
    fprintf(stderr, "%ju bytes copied, %.6f s, %.1f bytes/s\n", ctx->st.bytes_copied, elapsed, bytes_per_sec);
}

static void bx_dd_maybe_print_progress(struct bx_dd_ctx* ctx) {
    if (ctx->cfg.status_mask != BX_DD_STATUS_PROGRESS || !ctx->time_ready) {
        return;
    }

    struct timespec now;
    if (!bx_dd_now(&now)) {
        return;
    }

    double since_last = bx_dd_elapsed_seconds(&ctx->last_progress_time, &now);
    if (since_last < 1.0) {
        return;
    }

    ctx->last_progress_time = now;
    bx_dd_print_progress_line(ctx, &now);
}

static void bx_dd_print_summary(const struct bx_dd_ctx* ctx) {
    if (ctx->cfg.status_mask == BX_DD_STATUS_NONE) {
        return;
    }

    fprintf(stderr, "%ju+%ju records in\n", ctx->st.full_in, ctx->st.partial_in);
    fprintf(stderr, "%ju+%ju records out\n", ctx->st.full_out, ctx->st.partial_out);

    if (ctx->cfg.status_mask == BX_DD_STATUS_NOXFER) {
        return;
    }

    struct timespec now;
    if (!ctx->time_ready || !bx_dd_now(&now)) {
        fprintf(stderr, "%ju bytes copied\n", ctx->st.bytes_copied);
        return;
    }

    double elapsed = bx_dd_elapsed_seconds(&ctx->start_time, &now);
    if (elapsed <= 0.0) {
        elapsed = 0.000001;
    }

    double bytes_per_sec = (double)ctx->st.bytes_copied / elapsed;
    fprintf(stderr, "%ju bytes copied, %.6f s, %.1f bytes/s\n", ctx->st.bytes_copied, elapsed, bytes_per_sec);
}

static bool bx_dd_alloc_buffers(struct bx_dd_ctx* ctx) {
    if (ctx->cfg.ibs == 0 || ctx->cfg.obs == 0) {
        bx_dd_diag(ctx->progname, "block sizes must be greater than zero");
        return false;
    }

    if (ctx->cfg.ibs > SIZE_MAX || ctx->cfg.obs > SIZE_MAX) {
        bx_dd_diag(ctx->progname, "block size too large");
        return false;
    }

    ctx->ibuf = xmalloc((size_t)ctx->cfg.ibs);
    ctx->obuf = xmalloc((size_t)ctx->cfg.obs);
    ctx->obuf_len = 0;
    return true;
}

static void bx_dd_read_input(struct bx_dd_ctx* ctx, size_t want, size_t* got_out, int* err_out) {
    size_t total = 0;
    bool fullblock = (ctx->cfg.iflag_mask & BX_DD_IFLAG_FULLBLOCK) != 0u;

    *got_out = 0;
    *err_out = 0;

    while (total < want) {
        ssize_t nread = bx_xread(ctx->infd, ctx->ibuf + total, want - total);

        if (nread > 0) {
            total += (size_t)nread;
            if (!fullblock) {
                break;
            }
            continue;
        }

        if (nread == 0) {
            break;
        }

        *err_out = errno;
        break;
    }

    *got_out = total;
}

static void bx_dd_advance_after_read_error(struct bx_dd_ctx* ctx) {
    off_t ibs_offset = 0;
    if (!bx_dd_u64_to_off_t(ctx->cfg.ibs, &ibs_offset)) {
        return;
    }

    if (lseek(ctx->infd, ibs_offset, SEEK_CUR) < 0) {
        return;
    }
}

static bool bx_dd_write_chunk(struct bx_dd_ctx* ctx, const unsigned char* data, size_t len) {
    if (len == 0) {
        return true;
    }

    if (!bx_xwrite_all(ctx->outfd, data, len)) {
        bx_dd_perror_path(ctx->progname, ctx->output_path);
        return false;
    }

    if (len == (size_t)ctx->cfg.obs) {
        ctx->st.full_out++;
    }
    else {
        ctx->st.partial_out++;
    }

    uintmax_t bytes = (uintmax_t)len;
    if (ctx->st.bytes_copied > UINTMAX_MAX - bytes) {
        ctx->st.bytes_copied = UINTMAX_MAX;
    }
    else {
        ctx->st.bytes_copied += bytes;
    }

    return true;
}

static bool bx_dd_queue_output(struct bx_dd_ctx* ctx, const unsigned char* data, size_t len) {
    size_t obs = (size_t)ctx->cfg.obs;
    size_t offset = 0;

    while (offset < len) {
        size_t space = obs - ctx->obuf_len;
        size_t take = len - offset;
        if (take > space) {
            take = space;
        }

        memcpy(ctx->obuf + ctx->obuf_len, data + offset, take);
        ctx->obuf_len += take;
        offset += take;

        if (ctx->obuf_len == obs) {
            if (!bx_dd_write_chunk(ctx, ctx->obuf, obs)) {
                return false;
            }
            ctx->obuf_len = 0;
        }
    }

    return true;
}

static bool bx_dd_flush_output(struct bx_dd_ctx* ctx, bool final) {
    if (!final || ctx->obuf_len == 0) {
        return true;
    }

    size_t tail = ctx->obuf_len;
    ctx->obuf_len = 0;
    return bx_dd_write_chunk(ctx, ctx->obuf, tail);
}

static int bx_dd_run(struct bx_dd_ctx* ctx) {
    uintmax_t records_done = 0;
    size_t ibs = (size_t)ctx->cfg.ibs;

    while (!ctx->cfg.count_set || records_done < ctx->cfg.count) {
        size_t nread = 0;
        int read_err = 0;
        bx_dd_read_input(ctx, ibs, &nread, &read_err);

        if (read_err != 0) {
            bx_dd_perror_with_errno(ctx->progname, ctx->input_path, read_err);

            if ((ctx->cfg.conv_mask & BX_DD_CONV_NOERROR) == 0u) {
                return 1;
            }

            if (nread > 0) {
                ctx->st.partial_in++;
                size_t out_len = nread;

                if ((ctx->cfg.conv_mask & BX_DD_CONV_SYNC) != 0u && nread < ibs) {
                    memset(ctx->ibuf + nread, 0, ibs - nread);
                    out_len = ibs;
                }

                if (!bx_dd_queue_output(ctx, ctx->ibuf, out_len)) {
                    return 1;
                }

                records_done++;
                bx_dd_maybe_print_progress(ctx);
                continue;
            }

            if ((ctx->cfg.conv_mask & BX_DD_CONV_SYNC) != 0u) {
                memset(ctx->ibuf, 0, ibs);
                ctx->st.partial_in++;

                if (!bx_dd_queue_output(ctx, ctx->ibuf, ibs)) {
                    return 1;
                }

                records_done++;
                bx_dd_maybe_print_progress(ctx);
            }
            else {
                bx_dd_advance_after_read_error(ctx);
            }

            continue;
        }

        if (nread == 0) {
            break;
        }

        if (nread == ibs) {
            ctx->st.full_in++;
        }
        else {
            ctx->st.partial_in++;
        }

        size_t out_len = nread;
        if ((ctx->cfg.conv_mask & BX_DD_CONV_SYNC) != 0u && nread < ibs) {
            memset(ctx->ibuf + nread, 0, ibs - nread);
            out_len = ibs;
        }

        if (!bx_dd_queue_output(ctx, ctx->ibuf, out_len)) {
            return 1;
        }

        records_done++;
        bx_dd_maybe_print_progress(ctx);
    }

    if (!bx_dd_flush_output(ctx, true)) {
        return 1;
    }

    return 0;
}

int bx_dd_main(int argc, char** argv) {
    struct bx_dd_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));

    ctx.progname = bx_dd_progname((argc > 0) ? argv[0] : NULL);
    ctx.infd = -1;
    ctx.outfd = -1;

    enum bx_dd_mode mode = BX_DD_MODE_RUN;
    if (!bx_dd_parse_args(&ctx.cfg, argc, argv, &mode, ctx.progname)) {
        return 1;
    }

    if (mode == BX_DD_MODE_HELP) {
        bx_dd_print_help(stdout, ctx.progname);
        return 0;
    }

    if (mode == BX_DD_MODE_VERSION) {
        bx_dd_print_version(ctx.progname);
        return 0;
    }

    int rc = 1;

    if (!bx_dd_open_files(&ctx)) {
        goto out;
    }

    if (!bx_dd_apply_skip_seek(&ctx)) {
        goto out;
    }

    if (!bx_dd_alloc_buffers(&ctx)) {
        goto out;
    }

    ctx.time_ready = bx_dd_now(&ctx.start_time);
    if (ctx.time_ready) {
        ctx.last_progress_time = ctx.start_time;
    }

    ctx.should_print_stats = true;
    rc = bx_dd_run(&ctx);

out:
    if (ctx.outfd >= 0) {
        if (!bx_dd_finalize_output_size(&ctx)) {
            rc = 1;
        }

        if ((ctx.cfg.conv_mask & BX_DD_CONV_FDATASYNC) != 0u) {
            if (fdatasync(ctx.outfd) != 0) {
                bx_dd_perror_path(ctx.progname, ctx.output_path);
                rc = 1;
            }
        }
        else if ((ctx.cfg.conv_mask & BX_DD_CONV_FSYNC) != 0u) {
            if (fsync(ctx.outfd) != 0) {
                bx_dd_perror_path(ctx.progname, ctx.output_path);
                rc = 1;
            }
        }
    }

    if (ctx.should_print_stats) {
        bx_dd_print_summary(&ctx);
    }

    if (ctx.infd >= 0 && close(ctx.infd) != 0) {
        bx_dd_perror_path(ctx.progname, ctx.input_path);
        rc = 1;
    }

    if (ctx.outfd >= 0 && close(ctx.outfd) != 0) {
        bx_dd_perror_path(ctx.progname, ctx.output_path);
        rc = 1;
    }

    free(ctx.ibuf);
    free(ctx.obuf);

    return rc;
}
