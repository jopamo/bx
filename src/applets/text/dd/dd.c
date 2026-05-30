#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
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
#include "bx/diag.h"
#include "bx/libbx.h"
#include "lib/cli_common.h"
#include "lib/size_parse.h"
#include "lib/time_parse.h"
#include "lib/xreadwrite.h"

enum {
    BX_DD_CONV_SYNC = 1u << 0,
    BX_DD_CONV_NOERROR = 1u << 1,
    BX_DD_CONV_NOTRUNC = 1u << 2,
    BX_DD_CONV_FSYNC = 1u << 3,
    BX_DD_CONV_FDATASYNC = 1u << 4,
    BX_DD_CONV_ASCII = 1u << 5,
    BX_DD_CONV_EBCDIC = 1u << 6,
    BX_DD_CONV_IBM = 1u << 7,
    BX_DD_CONV_BLOCK = 1u << 8,
    BX_DD_CONV_UNBLOCK = 1u << 9,
    BX_DD_CONV_LCASE = 1u << 10,
    BX_DD_CONV_UCASE = 1u << 11,
    BX_DD_CONV_SPARSE = 1u << 12,
    BX_DD_CONV_SWAB = 1u << 13,
    BX_DD_CONV_EXCL = 1u << 14,
    BX_DD_CONV_NOCREAT = 1u << 15,
};

enum {
    BX_DD_IFLAG_FULLBLOCK = 1u << 0,
    BX_DD_FLAG_APPEND = 1u << 1,
    BX_DD_FLAG_DIRECT = 1u << 2,
    BX_DD_FLAG_DIRECTORY = 1u << 3,
    BX_DD_FLAG_DSYNC = 1u << 4,
    BX_DD_FLAG_SYNC = 1u << 5,
    BX_DD_FLAG_NONBLOCK = 1u << 6,
    BX_DD_FLAG_NOATIME = 1u << 7,
    BX_DD_FLAG_NOCACHE = 1u << 8,
    BX_DD_FLAG_NOCTTY = 1u << 9,
    BX_DD_FLAG_NOFOLLOW = 1u << 10,
    BX_DD_FLAG_COUNT_BYTES = 1u << 11,
    BX_DD_FLAG_SKIP_BYTES = 1u << 12,
    BX_DD_FLAG_SEEK_BYTES = 1u << 13,
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
    uintmax_t bs;
    uintmax_t cbs;
    uintmax_t count;
    uintmax_t skip;
    uintmax_t seek;

    bool bs_set;
    bool cbs_set;
    bool count_set;
    bool skip_set;
    bool seek_set;
    bool count_bytes;
    bool skip_bytes;
    bool seek_bytes;

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
    uintmax_t truncated_records;
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
    unsigned char* xbuf;
    unsigned char* cbuf;
    size_t obuf_len;
    size_t cbuf_len;

    bool block_truncating;
    bool block_truncated_counted;
    bool swab_have_saved;
    unsigned char swab_saved;

    bool time_ready;
    struct timespec start_time;
    struct timespec last_progress_time;

    bool should_print_stats;
};

static volatile sig_atomic_t bx_dd_usr1_requested = 0;

static void bx_dd_maybe_warn_truncated_records(const struct bx_dd_ctx* ctx);

static void bx_dd_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPERAND]...\n", progname);
    fprintf(stream, "  or:  %s OPTION\n", progname);
    fprintf(stream, "Copy a file, converting and formatting according to the operands.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  bs=BYTES        read and write up to BYTES bytes at a time (default: 512);\n");
    fprintf(stream, "                  overrides ibs and obs\n");
    fprintf(stream, "  cbs=BYTES       convert BYTES bytes at a time\n");
    fprintf(stream, "  conv=CONVS      convert the file as per the comma separated symbol list\n");
    fprintf(stream, "  count=N         copy only N input blocks\n");
    fprintf(stream, "  ibs=BYTES       read up to BYTES bytes at a time (default: 512)\n");
    fprintf(stream, "  if=FILE         read from FILE instead of standard input\n");
    fprintf(stream, "  iflag=FLAGS     read as per the comma separated symbol list\n");
    fprintf(stream, "  obs=BYTES       write BYTES bytes at a time (default: 512)\n");
    fprintf(stream, "  of=FILE         write to FILE instead of standard output\n");
    fprintf(stream, "  oflag=FLAGS     write as per the comma separated symbol list\n");
    fprintf(stream, "  seek=N          (or oseek=N) skip N obs sized output blocks\n");
    fprintf(stream, "  skip=N          (or iseek=N) skip N ibs sized input blocks\n");
    fprintf(stream, "  status=LEVEL    The LEVEL of information to print to standard error;\n");
    fprintf(stream, "                  'none' suppresses everything but error messages,\n");
    fprintf(stream, "                  'noxfer' suppresses the final transfer statistics,\n");
    fprintf(stream, "                  'progress' shows periodic transfer statistics\n");
    fprintf(stream, "\n");
    fprintf(stream, "N and BYTES may be followed by the following multiplicative suffixes:\n");
    fprintf(stream, "c=1, w=2, b=512, kB=1000, K=1024, MB=1000*1000, M=1024*1024, xM=M,\n");
    fprintf(stream, "GB=1000*1000*1000, G=1024*1024*1024, and so on for T, P, E, Z, Y, R, Q.\n");
    fprintf(stream, "Binary prefixes can be used, too: KiB=K, MiB=M, and so on.\n");
    fprintf(stream, "If N ends in 'B', it counts bytes not blocks.\n");
    fprintf(stream, "\n");
    fprintf(stream, "Each CONV symbol may be:\n");
    fprintf(stream, "\n");
    fprintf(stream, "  ascii     from EBCDIC to ASCII\n");
    fprintf(stream, "  ebcdic    from ASCII to EBCDIC\n");
    fprintf(stream, "  ibm       from ASCII to alternate EBCDIC\n");
    fprintf(stream, "  block     pad newline-terminated records with spaces to cbs-size\n");
    fprintf(stream, "  unblock   replace trailing spaces in cbs-size records with newline\n");
    fprintf(stream, "  lcase     change upper case to lower case\n");
    fprintf(stream, "  ucase     change lower case to upper case\n");
    fprintf(stream, "  sparse    try to seek rather than write all-NUL output blocks\n");
    fprintf(stream, "  swab      swap every pair of input bytes\n");
    fprintf(stream, "  sync      pad every input block with NULs to ibs-size; when used\n");
    fprintf(stream, "            with block or unblock, pad with spaces rather than NULs\n");
    fprintf(stream, "  excl      fail if the output file already exists\n");
    fprintf(stream, "  nocreat   do not create the output file\n");
    fprintf(stream, "  notrunc   do not truncate the output file\n");
    fprintf(stream, "  noerror   continue after read errors\n");
    fprintf(stream, "  fdatasync  physically write output file data before finishing\n");
    fprintf(stream, "  fsync     likewise, but also write metadata\n");
    fprintf(stream, "\n");
    fprintf(stream, "Each FLAG symbol may be:\n");
    fprintf(stream, "\n");
    fprintf(stream, "  append    append mode (makes sense only for output; conv=notrunc suggested)\n");
    fprintf(stream, "  direct    use direct I/O for data\n");
    fprintf(stream, "  directory  fail unless a directory\n");
    fprintf(stream, "  dsync     use synchronized I/O for data\n");
    fprintf(stream, "  sync      likewise, but also for metadata\n");
    fprintf(stream, "  fullblock  accumulate full blocks of input (iflag only)\n");
    fprintf(stream, "  nonblock  use non-blocking I/O\n");
    fprintf(stream, "  noatime   do not update access time\n");
    fprintf(stream, "  nocache   Request to drop cache.  See also oflag=sync\n");
    fprintf(stream, "  noctty    do not assign controlling terminal from file\n");
    fprintf(stream, "  nofollow  do not follow symlinks\n");
    fprintf(stream, "\n");
    fprintf(stream, "Sending a USR1 signal to a running 'dd' process makes it\n");
    fprintf(stream, "print I/O statistics to standard error and then resume copying.\n");
    fprintf(stream, "\n");
    fprintf(stream, "Options are:\n");
    fprintf(stream, "\n");
    fprintf(stream, "      --help\n");
    fprintf(stream, "         display this help and exit\n");
    fprintf(stream, "      --version\n");
    fprintf(stream, "         output version information and exit\n");
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

static void bx_dd_perror_setting_flags(const char* progname, const char* path, int errnum) {
    fprintf(stderr, "%s: setting flags for '%s': %s\n", progname, path, strerror(errnum));
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

static bool bx_dd_parse_count_like(const char* text, uintmax_t* value_out, bool* bytes_out, const char* name, const char* progname) {
    uintmax_t value = 0;
    if (!bx_dd_parse_size(text, &value)) {
        bx_dd_diag(progname, "invalid %s value '%s'", name, (text != NULL) ? text : "");
        return false;
    }

    *value_out = value;
    if (bytes_out != NULL) {
        size_t len = (text != NULL) ? strlen(text) : 0;
        *bytes_out = (len > 0 && text[len - 1] == 'B');
    }
    return true;
}

static char* bx_dd_token_dup(const char* start, size_t len) {
    char* token = xmalloc(len + 1u);
    if (len > 0) {
        memcpy(token, start, len);
    }
    token[len] = '\0';
    return token;
}

static bool bx_dd_parse_conv(struct bx_dd_config* cfg, const char* value, const char* progname) {
    const char* token_start = value;
    const char* p = value;

    while (true) {
        if (*p != ',' && *p != '\0') {
            p++;
            continue;
        }

        char* token = bx_dd_token_dup(token_start, (size_t)(p - token_start));
        if (token[0] == '\0') {
            bx_dd_diag(progname, "invalid conversion: ''");
            free(token);
            return false;
        }

        if (strcmp(token, "ascii") == 0) {
            cfg->conv_mask |= BX_DD_CONV_ASCII;
        }
        else if (strcmp(token, "ebcdic") == 0) {
            cfg->conv_mask |= BX_DD_CONV_EBCDIC;
        }
        else if (strcmp(token, "ibm") == 0) {
            cfg->conv_mask |= BX_DD_CONV_IBM;
        }
        else if (strcmp(token, "block") == 0) {
            cfg->conv_mask |= BX_DD_CONV_BLOCK;
        }
        else if (strcmp(token, "unblock") == 0) {
            cfg->conv_mask |= BX_DD_CONV_UNBLOCK;
        }
        else if (strcmp(token, "lcase") == 0) {
            cfg->conv_mask |= BX_DD_CONV_LCASE;
        }
        else if (strcmp(token, "ucase") == 0) {
            cfg->conv_mask |= BX_DD_CONV_UCASE;
        }
        else if (strcmp(token, "sparse") == 0) {
            cfg->conv_mask |= BX_DD_CONV_SPARSE;
        }
        else if (strcmp(token, "swab") == 0) {
            cfg->conv_mask |= BX_DD_CONV_SWAB;
        }
        else if (strcmp(token, "sync") == 0) {
            cfg->conv_mask |= BX_DD_CONV_SYNC;
        }
        else if (strcmp(token, "excl") == 0) {
            cfg->conv_mask |= BX_DD_CONV_EXCL;
        }
        else if (strcmp(token, "nocreat") == 0) {
            cfg->conv_mask |= BX_DD_CONV_NOCREAT;
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
            bx_dd_diag(progname, "invalid conversion: '%s'", token);
            free(token);
            return false;
        }

        free(token);

        if (*p == '\0') {
            break;
        }
        token_start = p + 1;
        p++;
    }

    return true;
}

static bool bx_dd_apply_flag_token(struct bx_dd_config* cfg, const char* token, bool input_flag, const char* progname) {
    unsigned* mask = input_flag ? &cfg->iflag_mask : &cfg->oflag_mask;

    if (strcmp(token, "append") == 0) {
        *mask |= BX_DD_FLAG_APPEND;
        return true;
    }

    if (strcmp(token, "fullblock") == 0) {
        if (!input_flag) {
            bx_dd_diag(progname, "invalid output flag '%s'", token);
            return false;
        }
        *mask |= BX_DD_IFLAG_FULLBLOCK;
        return true;
    }

    if (strcmp(token, "count_bytes") == 0) {
        *mask |= BX_DD_FLAG_COUNT_BYTES;
        if (input_flag) {
            cfg->count_bytes = true;
        }
        return true;
    }

    if (strcmp(token, "skip_bytes") == 0) {
        *mask |= BX_DD_FLAG_SKIP_BYTES;
        if (input_flag) {
            cfg->skip_bytes = true;
        }
        return true;
    }

    if (strcmp(token, "seek_bytes") == 0) {
        *mask |= BX_DD_FLAG_SEEK_BYTES;
        if (!input_flag) {
            cfg->seek_bytes = true;
        }
        return true;
    }

    if (strcmp(token, "direct") == 0) {
        *mask |= BX_DD_FLAG_DIRECT;
        return true;
    }
    if (strcmp(token, "directory") == 0) {
        *mask |= BX_DD_FLAG_DIRECTORY;
        return true;
    }
    if (strcmp(token, "dsync") == 0) {
        *mask |= BX_DD_FLAG_DSYNC;
        return true;
    }
    if (strcmp(token, "sync") == 0) {
        *mask |= BX_DD_FLAG_SYNC;
        return true;
    }
    if (strcmp(token, "nonblock") == 0) {
        *mask |= BX_DD_FLAG_NONBLOCK;
        return true;
    }
    if (strcmp(token, "noatime") == 0) {
        *mask |= BX_DD_FLAG_NOATIME;
        return true;
    }
    if (strcmp(token, "nocache") == 0) {
        *mask |= BX_DD_FLAG_NOCACHE;
        return true;
    }
    if (strcmp(token, "noctty") == 0) {
        *mask |= BX_DD_FLAG_NOCTTY;
        return true;
    }
    if (strcmp(token, "nofollow") == 0) {
        *mask |= BX_DD_FLAG_NOFOLLOW;
        return true;
    }

    bx_dd_diag(progname, "invalid %s flag '%s'", input_flag ? "input" : "output", token);
    return false;
}

static bool bx_dd_parse_flag_list(struct bx_dd_config* cfg, const char* value, bool input_flag, const char* progname) {
    const char* token_start = value;
    const char* p = value;

    while (true) {
        if (*p != ',' && *p != '\0') {
            p++;
            continue;
        }

        char* token = bx_dd_token_dup(token_start, (size_t)(p - token_start));
        if (token[0] == '\0') {
            bx_dd_diag(progname, "invalid %s flag: ''", input_flag ? "input" : "output");
            free(token);
            return false;
        }

        if (!bx_dd_apply_flag_token(cfg, token, input_flag, progname)) {
            free(token);
            return false;
        }

        free(token);

        if (*p == '\0') {
            break;
        }
        token_start = p + 1;
        p++;
    }

    return true;
}

static bool bx_dd_parse_iflag(struct bx_dd_config* cfg, const char* value, const char* progname) {
    return bx_dd_parse_flag_list(cfg, value, true, progname);
}

static bool bx_dd_parse_oflag(struct bx_dd_config* cfg, const char* value, const char* progname) {
    return bx_dd_parse_flag_list(cfg, value, false, progname);
}

static bool bx_dd_parse_status(struct bx_dd_config* cfg, const char* value, const char* progname) {
    const char* token_start = value;
    const char* p = value;

    while (true) {
        if (*p != ',' && *p != '\0') {
            p++;
            continue;
        }

        char* token = bx_dd_token_dup(token_start, (size_t)(p - token_start));
        if (token[0] == '\0') {
            bx_dd_diag(progname, "invalid status level: ''");
            free(token);
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
            bx_dd_diag(progname, "invalid status level: '%s'", token);
            free(token);
            return false;
        }

        free(token);

        if (*p == '\0') {
            break;
        }
        token_start = p + 1;
        p++;
    }

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
        if (!bx_dd_parse_nonzero_size(value, &cfg->cbs, "cbs", progname)) {
            return false;
        }
        cfg->cbs_set = true;
        return true;
    }

    if (bx_dd_key_eq(arg, key_len, "bs")) {
        uintmax_t bs = 0;
        if (!bx_dd_parse_nonzero_size(value, &bs, "bs", progname)) {
            return false;
        }
        cfg->bs = bs;
        cfg->bs_set = true;
        return true;
    }

    if (bx_dd_key_eq(arg, key_len, "count")) {
        bool bytes = false;
        if (!bx_dd_parse_count_like(value, &cfg->count, &bytes, "count", progname)) {
            return false;
        }
        cfg->count_bytes = bytes || (cfg->iflag_mask & BX_DD_FLAG_COUNT_BYTES) != 0u;
        cfg->count_set = true;
        return true;
    }

    if (bx_dd_key_eq(arg, key_len, "skip") || bx_dd_key_eq(arg, key_len, "iseek")) {
        bool bytes = false;
        if (!bx_dd_parse_count_like(value, &cfg->skip, &bytes, "skip", progname)) {
            return false;
        }
        cfg->skip_bytes = bytes || (cfg->iflag_mask & BX_DD_FLAG_SKIP_BYTES) != 0u;
        cfg->skip_set = true;
        return true;
    }

    if (bx_dd_key_eq(arg, key_len, "seek") || bx_dd_key_eq(arg, key_len, "oseek")) {
        bool bytes = false;
        if (!bx_dd_parse_count_like(value, &cfg->seek, &bytes, "seek", progname)) {
            return false;
        }
        cfg->seek_bytes = bytes || (cfg->oflag_mask & BX_DD_FLAG_SEEK_BYTES) != 0u;
        cfg->seek_set = true;
        return true;
    }

    if (bx_dd_key_eq(arg, key_len, "conv")) {
        return bx_dd_parse_conv(cfg, value, progname);
    }

    if (bx_dd_key_eq(arg, key_len, "iflag")) {
        return bx_dd_parse_iflag(cfg, value, progname);
    }

    if (bx_dd_key_eq(arg, key_len, "oflag")) {
        return bx_dd_parse_oflag(cfg, value, progname);
    }

    if (bx_dd_key_eq(arg, key_len, "status")) {
        return bx_dd_parse_status(cfg, value, progname);
    }

    bx_dd_diag(progname, "unrecognized operand '%s'", arg);
    return false;
}

static unsigned bx_dd_popcount(unsigned value) {
    unsigned count = 0;
    while (value != 0u) {
        count += value & 1u;
        value >>= 1u;
    }
    return count;
}

static bool bx_dd_validate_config(const struct bx_dd_config* cfg, const char* progname) {
    unsigned charset = cfg->conv_mask & (BX_DD_CONV_ASCII | BX_DD_CONV_EBCDIC | BX_DD_CONV_IBM);
    if (bx_dd_popcount(charset) > 1u) {
        bx_dd_diag(progname, "cannot combine any two of {ascii,ebcdic,ibm}");
        return false;
    }

    bool block_side = (cfg->conv_mask & (BX_DD_CONV_BLOCK | BX_DD_CONV_EBCDIC | BX_DD_CONV_IBM)) != 0u;
    bool unblock_side = (cfg->conv_mask & (BX_DD_CONV_UNBLOCK | BX_DD_CONV_ASCII)) != 0u;
    if (block_side && unblock_side) {
        bx_dd_diag(progname, "cannot combine block and unblock");
        return false;
    }

    if ((cfg->conv_mask & BX_DD_CONV_LCASE) != 0u && (cfg->conv_mask & BX_DD_CONV_UCASE) != 0u) {
        bx_dd_diag(progname, "cannot combine lcase and ucase");
        return false;
    }

    if ((cfg->conv_mask & BX_DD_CONV_EXCL) != 0u && (cfg->conv_mask & BX_DD_CONV_NOCREAT) != 0u) {
        bx_dd_diag(progname, "cannot combine excl and nocreat");
        return false;
    }

    return true;
}

static bool bx_dd_parse_args(struct bx_dd_config* cfg, int argc, char** argv, enum bx_dd_mode* mode_out, const char* progname) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->ibs = 512u;
    cfg->obs = 512u;
    cfg->create_mode = 0666u;

    *mode_out = BX_DD_MODE_RUN;

    bool end_options = false;

    for (int i = 1; i < argc; i++) {
        const char* arg = argv[i];

        if (!end_options && strcmp(arg, "--") == 0) {
            end_options = true;
            continue;
        }

        if (!end_options && strcmp(arg, "--help") == 0) {
            *mode_out = BX_DD_MODE_HELP;
            return true;
        }

        if (!end_options && strcmp(arg, "--version") == 0) {
            *mode_out = BX_DD_MODE_VERSION;
            return true;
        }

        if (!end_options && arg[0] == '-' && arg[1] != '\0') {
            bx_dd_diag(progname, "unrecognized option '%s'", arg);
            return false;
        }

        if (!bx_dd_parse_assignment(cfg, arg, progname)) {
            return false;
        }
    }

    if (cfg->bs_set) {
        cfg->ibs = cfg->bs;
        cfg->obs = cfg->bs;
    }

    if (!bx_dd_validate_config(cfg, progname)) {
        return false;
    }

    return true;
}

static bool bx_dd_add_open_flags(unsigned mask, int* flags_out, const char* progname) {
    (void)progname;
    int flags = 0;

    if ((mask & BX_DD_FLAG_APPEND) != 0u) {
        flags |= O_APPEND;
    }
#ifdef O_DIRECT
    if ((mask & BX_DD_FLAG_DIRECT) != 0u) {
        flags |= O_DIRECT;
    }
#else
    if ((mask & BX_DD_FLAG_DIRECT) != 0u) {
        bx_dd_diag(progname, "direct I/O is not supported on this platform");
        return false;
    }
#endif
#ifdef O_DIRECTORY
    if ((mask & BX_DD_FLAG_DIRECTORY) != 0u) {
        flags |= O_DIRECTORY;
    }
#else
    if ((mask & BX_DD_FLAG_DIRECTORY) != 0u) {
        bx_dd_diag(progname, "directory open flag is not supported on this platform");
        return false;
    }
#endif
#ifdef O_DSYNC
    if ((mask & BX_DD_FLAG_DSYNC) != 0u) {
        flags |= O_DSYNC;
    }
#else
    if ((mask & BX_DD_FLAG_DSYNC) != 0u) {
        bx_dd_diag(progname, "synchronized data I/O is not supported on this platform");
        return false;
    }
#endif
#ifdef O_SYNC
    if ((mask & BX_DD_FLAG_SYNC) != 0u) {
        flags |= O_SYNC;
    }
#else
    if ((mask & BX_DD_FLAG_SYNC) != 0u) {
        bx_dd_diag(progname, "synchronized I/O is not supported on this platform");
        return false;
    }
#endif
#ifdef O_NONBLOCK
    if ((mask & BX_DD_FLAG_NONBLOCK) != 0u) {
        flags |= O_NONBLOCK;
    }
#else
    if ((mask & BX_DD_FLAG_NONBLOCK) != 0u) {
        bx_dd_diag(progname, "nonblocking I/O is not supported on this platform");
        return false;
    }
#endif
#ifdef O_NOATIME
    if ((mask & BX_DD_FLAG_NOATIME) != 0u) {
        flags |= O_NOATIME;
    }
#else
    if ((mask & BX_DD_FLAG_NOATIME) != 0u) {
        bx_dd_diag(progname, "noatime is not supported on this platform");
        return false;
    }
#endif
#ifdef O_NOCTTY
    if ((mask & BX_DD_FLAG_NOCTTY) != 0u) {
        flags |= O_NOCTTY;
    }
#else
    if ((mask & BX_DD_FLAG_NOCTTY) != 0u) {
        bx_dd_diag(progname, "noctty is not supported on this platform");
        return false;
    }
#endif
#ifdef O_NOFOLLOW
    if ((mask & BX_DD_FLAG_NOFOLLOW) != 0u) {
        flags |= O_NOFOLLOW;
    }
#else
    if ((mask & BX_DD_FLAG_NOFOLLOW) != 0u) {
        bx_dd_diag(progname, "nofollow is not supported on this platform");
        return false;
    }
#endif

    *flags_out |= flags;
    return true;
}

static bool bx_dd_apply_standard_fd_flags(struct bx_dd_ctx* ctx, int fd, unsigned mask, const char* path) {
#ifdef O_DIRECTORY
    if ((mask & BX_DD_FLAG_DIRECTORY) != 0u) {
        struct stat st;
        if (fstat(fd, &st) != 0) {
            bx_dd_perror_setting_flags(ctx->progname, path, errno);
            return false;
        }
        if (!S_ISDIR(st.st_mode)) {
            bx_dd_perror_setting_flags(ctx->progname, path, ENOTDIR);
            return false;
        }
    }
#else
    if ((mask & BX_DD_FLAG_DIRECTORY) != 0u) {
        bx_dd_diag(ctx->progname, "directory open flag is not supported on this platform");
        return false;
    }
#endif

    int set_flags = 0;

    if ((mask & BX_DD_FLAG_APPEND) != 0u) {
        set_flags |= O_APPEND;
    }
#ifdef O_DIRECT
    if ((mask & BX_DD_FLAG_DIRECT) != 0u) {
        set_flags |= O_DIRECT;
    }
#else
    if ((mask & BX_DD_FLAG_DIRECT) != 0u) {
        bx_dd_diag(ctx->progname, "direct I/O is not supported on this platform");
        return false;
    }
#endif
#ifdef O_DSYNC
    if ((mask & BX_DD_FLAG_DSYNC) != 0u) {
        set_flags |= O_DSYNC;
    }
#else
    if ((mask & BX_DD_FLAG_DSYNC) != 0u) {
        bx_dd_diag(ctx->progname, "synchronized data I/O is not supported on this platform");
        return false;
    }
#endif
#ifdef O_SYNC
    if ((mask & BX_DD_FLAG_SYNC) != 0u) {
        set_flags |= O_SYNC;
    }
#else
    if ((mask & BX_DD_FLAG_SYNC) != 0u) {
        bx_dd_diag(ctx->progname, "synchronized I/O is not supported on this platform");
        return false;
    }
#endif
#ifdef O_NONBLOCK
    if ((mask & BX_DD_FLAG_NONBLOCK) != 0u) {
        set_flags |= O_NONBLOCK;
    }
#else
    if ((mask & BX_DD_FLAG_NONBLOCK) != 0u) {
        bx_dd_diag(ctx->progname, "nonblocking I/O is not supported on this platform");
        return false;
    }
#endif
#ifdef O_NOATIME
    if ((mask & BX_DD_FLAG_NOATIME) != 0u) {
        set_flags |= O_NOATIME;
    }
#else
    if ((mask & BX_DD_FLAG_NOATIME) != 0u) {
        bx_dd_diag(ctx->progname, "noatime is not supported on this platform");
        return false;
    }
#endif

    if (set_flags == 0) {
        return true;
    }

    int current = fcntl(fd, F_GETFL);
    if (current < 0) {
        bx_dd_perror_setting_flags(ctx->progname, path, errno);
        return false;
    }

    if (fcntl(fd, F_SETFL, current | set_flags) != 0) {
        bx_dd_perror_setting_flags(ctx->progname, path, errno);
        return false;
    }

    return true;
}

static bool bx_dd_open_files(struct bx_dd_ctx* ctx) {
    ctx->input_path = (ctx->cfg.ifile != NULL) ? ctx->cfg.ifile : "standard input";
    ctx->output_path = (ctx->cfg.ofile != NULL) ? ctx->cfg.ofile : "standard output";

    if (ctx->cfg.ifile == NULL) {
        ctx->infd = dup(STDIN_FILENO);
        if (ctx->infd < 0) {
            bx_dd_perror_path(ctx->progname, ctx->input_path);
            return false;
        }
        if (!bx_dd_apply_standard_fd_flags(ctx, ctx->infd, ctx->cfg.iflag_mask, ctx->input_path)) {
            return false;
        }
    }
    else {
        int in_flags = O_RDONLY;
        if (!bx_dd_add_open_flags(ctx->cfg.iflag_mask, &in_flags, ctx->progname)) {
            return false;
        }
        ctx->infd = open(ctx->input_path, in_flags);
        if (ctx->infd < 0) {
            bx_dd_perror_path(ctx->progname, ctx->input_path);
            return false;
        }
    }

    unsigned effective_oflag_mask = ctx->cfg.oflag_mask;
    if ((effective_oflag_mask & BX_DD_FLAG_APPEND) != 0u && ctx->cfg.seek_set && (ctx->cfg.conv_mask & BX_DD_CONV_NOTRUNC) == 0u) {
        effective_oflag_mask &= ~BX_DD_FLAG_APPEND;
    }

    if (ctx->cfg.ofile == NULL) {
        ctx->outfd = dup(STDOUT_FILENO);
        if (ctx->outfd < 0) {
            bx_dd_perror_path(ctx->progname, ctx->output_path);
            return false;
        }
        if (!bx_dd_apply_standard_fd_flags(ctx, ctx->outfd, effective_oflag_mask, ctx->output_path)) {
            return false;
        }
    }
    else {
        int out_flags = O_WRONLY;
        if ((ctx->cfg.conv_mask & BX_DD_CONV_NOCREAT) == 0u || (ctx->cfg.conv_mask & BX_DD_CONV_EXCL) != 0u) {
            out_flags |= O_CREAT;
        }
        if ((ctx->cfg.conv_mask & BX_DD_CONV_EXCL) != 0u) {
            out_flags |= O_EXCL;
        }
        if ((ctx->cfg.conv_mask & BX_DD_CONV_NOTRUNC) == 0u && !ctx->cfg.seek_set) {
            out_flags |= O_TRUNC;
        }
        if (!bx_dd_add_open_flags(effective_oflag_mask, &out_flags, ctx->progname)) {
            return false;
        }

        ctx->outfd = open(ctx->output_path, out_flags, ctx->cfg.create_mode);
        if (ctx->outfd < 0) {
            bx_dd_perror_path(ctx->progname, ctx->output_path);
            return false;
        }
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
        if (ctx->cfg.skip_bytes) {
            skip_bytes = ctx->cfg.skip;
        }
        else if (!bx_dd_safe_mul(ctx->cfg.skip, ctx->cfg.ibs, &skip_bytes)) {
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
        if (ctx->cfg.seek_bytes) {
            seek_bytes = ctx->cfg.seek;
        }
        else if (!bx_dd_safe_mul(ctx->cfg.seek, ctx->cfg.obs, &seek_bytes)) {
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
    bool force_truncate_to_position = ctx->cfg.seek_set && (ctx->cfg.conv_mask & BX_DD_CONV_NOTRUNC) == 0u;
    bool sparse_may_need_extend = (ctx->cfg.conv_mask & BX_DD_CONV_SPARSE) != 0u;

    if (!force_truncate_to_position && !sparse_may_need_extend) {
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

    if (!force_truncate_to_position && end_pos <= st.st_size) {
        return true;
    }

    if (ftruncate(ctx->outfd, end_pos) != 0) {
        bx_dd_perror_path(ctx->progname, ctx->output_path);
        return false;
    }

    return true;
}

static bool bx_dd_drop_cache_if_requested(struct bx_dd_ctx* ctx, int fd, unsigned mask, const char* path) {
    if ((mask & BX_DD_FLAG_NOCACHE) == 0u) {
        return true;
    }

#ifdef POSIX_FADV_DONTNEED
    int rc = posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
    if (rc != 0 && rc != ESPIPE && rc != EINVAL) {
        bx_dd_perror_with_errno(ctx->progname, path, rc);
        return false;
    }
    return true;
#else
    (void)fd;
    (void)path;
    bx_dd_diag(ctx->progname, "nocache is not supported on this platform");
    return false;
#endif
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
    double elapsed = 0.0;
    if (!bx_time_timespec_elapsed_seconds_double(start, end, &elapsed)) {
        return 0.0;
    }

    return elapsed;
}

static void bx_dd_format_amount(double value, char* buf, size_t buf_size) {
    snprintf(buf, buf_size, "%.1f", value);
}

static bool bx_dd_format_byte_humans(uintmax_t bytes, char* buf, size_t buf_size) {
    if (bytes < 1000) {
        if (buf_size > 0) {
            buf[0] = '\0';
        }
        return false;
    }

    double decimal = 0.0;
    unsigned int decimal_power = 0u;
    if (!bx_size_scale_human_double((double)bytes, 1000.0, 999.95, 6u, &decimal, &decimal_power)) {
        if (buf_size > 0) {
            buf[0] = '\0';
        }
        return false;
    }

    const char* decimal_unit = bx_size_unit_label(BX_SIZE_UNIT_LABEL_SI_LOWER_K, decimal_power);
    if (decimal_unit == NULL) {
        if (buf_size > 0) {
            buf[0] = '\0';
        }
        return false;
    }

    char decimal_number[64];
    bx_dd_format_amount(decimal, decimal_number, sizeof(decimal_number));

    if (bytes < 1024) {
        snprintf(buf, buf_size, " (%s %sB)", decimal_number, decimal_unit);
        return true;
    }

    double binary = 0.0;
    unsigned int binary_power = 0u;
    if (!bx_size_scale_human_double((double)bytes, 1024.0, 999.95, 6u, &binary, &binary_power)) {
        if (buf_size > 0) {
            buf[0] = '\0';
        }
        return false;
    }

    const char* binary_unit = bx_size_unit_label(BX_SIZE_UNIT_LABEL_IEC_I_SUFFIX, binary_power);
    if (binary_unit == NULL) {
        if (buf_size > 0) {
            buf[0] = '\0';
        }
        return false;
    }

    char binary_number[64];
    bx_dd_format_amount(binary, binary_number, sizeof(binary_number));
    snprintf(buf, buf_size, " (%s %sB, %s %sB)", decimal_number, decimal_unit, binary_number, binary_unit);
    return true;
}

static void bx_dd_print_bytes_copied_line(uintmax_t bytes, double elapsed) {
    if (elapsed <= 0.0) {
        elapsed = 0.000001;
    }

    char rate[128];
    bx_size_format_decimal_rate((double)bytes / elapsed, rate, sizeof(rate));

    char humans[160];
    bx_dd_format_byte_humans(bytes, humans, sizeof(humans));

    fprintf(stderr, "%ju %s%s copied, %.6g s, %s\n", bytes, (bytes == 1) ? "byte" : "bytes", humans, elapsed, rate);
}

static void bx_dd_print_progress_line(struct bx_dd_ctx* ctx, const struct timespec* now) {
    if (ctx->cfg.status_mask != BX_DD_STATUS_PROGRESS || !ctx->time_ready) {
        return;
    }

    double elapsed = bx_dd_elapsed_seconds(&ctx->start_time, now);
    bx_dd_print_bytes_copied_line(ctx->st.bytes_copied, elapsed);
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
    bx_dd_maybe_warn_truncated_records(ctx);

    if (ctx->cfg.status_mask == BX_DD_STATUS_NOXFER) {
        return;
    }

    struct timespec now;
    if (!ctx->time_ready || !bx_dd_now(&now)) {
        fprintf(stderr, "%ju bytes copied\n", ctx->st.bytes_copied);
        return;
    }

    double elapsed = bx_dd_elapsed_seconds(&ctx->start_time, &now);
    bx_dd_print_bytes_copied_line(ctx->st.bytes_copied, elapsed);
}

static void bx_dd_handle_usr1(int signo) {
    (void)signo;
    bx_dd_usr1_requested = 1;
}

static void bx_dd_install_usr1_handler(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = bx_dd_handle_usr1;
    sigemptyset(&sa.sa_mask);
#ifdef SA_RESTART
    sa.sa_flags = SA_RESTART;
#endif
    (void)sigaction(SIGUSR1, &sa, NULL);
}

static void bx_dd_maybe_print_usr1_stats(const struct bx_dd_ctx* ctx) {
    if (bx_dd_usr1_requested == 0) {
        return;
    }

    bx_dd_usr1_requested = 0;
    bx_dd_print_summary(ctx);
}

static void* bx_dd_alloc_buffer(size_t size) {
    if (size == 0) {
        size = 1;
    }

    void* ptr = NULL;
    if (posix_memalign(&ptr, 4096, size) == 0) {
        return ptr;
    }

    return xmalloc(size);
}

static bool bx_dd_record_conversion_active(const struct bx_dd_ctx* ctx) {
    if (!ctx->cfg.cbs_set || ctx->cfg.cbs == 0) {
        return false;
    }

    return (ctx->cfg.conv_mask & (BX_DD_CONV_BLOCK | BX_DD_CONV_UNBLOCK | BX_DD_CONV_ASCII | BX_DD_CONV_EBCDIC | BX_DD_CONV_IBM)) != 0u;
}

static bool bx_dd_effective_block(const struct bx_dd_ctx* ctx) {
    return bx_dd_record_conversion_active(ctx) && (ctx->cfg.conv_mask & (BX_DD_CONV_BLOCK | BX_DD_CONV_EBCDIC | BX_DD_CONV_IBM)) != 0u;
}

static bool bx_dd_effective_unblock(const struct bx_dd_ctx* ctx) {
    return bx_dd_record_conversion_active(ctx) && (ctx->cfg.conv_mask & (BX_DD_CONV_UNBLOCK | BX_DD_CONV_ASCII)) != 0u;
}

static unsigned char bx_dd_sync_pad_byte(const struct bx_dd_ctx* ctx) {
    if (bx_dd_effective_block(ctx) || bx_dd_effective_unblock(ctx)) {
        return (unsigned char)' ';
    }
    return (unsigned char)0;
}

static bool bx_dd_alloc_buffers(struct bx_dd_ctx* ctx) {
    if (ctx->cfg.ibs == 0 || ctx->cfg.obs == 0) {
        bx_dd_diag(ctx->progname, "block sizes must be greater than zero");
        return false;
    }

    if (ctx->cfg.ibs >= SIZE_MAX || ctx->cfg.obs > SIZE_MAX || ctx->cfg.cbs > SIZE_MAX) {
        bx_dd_diag(ctx->progname, "block size too large");
        return false;
    }

    ctx->ibuf = bx_dd_alloc_buffer((size_t)ctx->cfg.ibs);
    ctx->obuf = bx_dd_alloc_buffer((size_t)ctx->cfg.obs);
    ctx->xbuf = bx_dd_alloc_buffer((size_t)ctx->cfg.ibs + 1u);
    if (bx_dd_record_conversion_active(ctx)) {
        ctx->cbuf = bx_dd_alloc_buffer((size_t)ctx->cfg.cbs);
    }
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

static const unsigned char bx_dd_ascii_table[256] = {
    0x00, 0x01, 0x02, 0x03, 0x9c, 0x09, 0x86, 0x7f, 0x97, 0x8d, 0x8e, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    0x10, 0x11, 0x12, 0x13, 0x9d, 0x85, 0x08, 0x87, 0x18, 0x19, 0x92, 0x8f, 0x1c, 0x1d, 0x1e, 0x1f,
    0x80, 0x81, 0x82, 0x83, 0x84, 0x0a, 0x17, 0x1b, 0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x05, 0x06, 0x07,
    0x90, 0x91, 0x16, 0x93, 0x94, 0x95, 0x96, 0x04, 0x98, 0x99, 0x9a, 0x9b, 0x14, 0x15, 0x9e, 0x1a,
    0x20, 0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xd5, 0x2e, 0x3c, 0x28, 0x2b, 0x7c,
    0x26, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf, 0xb0, 0xb1, 0x21, 0x24, 0x2a, 0x29, 0x3b, 0x7e,
    0x2d, 0x2f, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xcb, 0x2c, 0x25, 0x5f, 0x3e, 0x3f,
    0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf, 0xc0, 0xc1, 0xc2, 0x60, 0x3a, 0x23, 0x40, 0x27, 0x3d, 0x22,
    0xc3, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9,
    0xca, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f, 0x70, 0x71, 0x72, 0x5e, 0xcc, 0xcd, 0xce, 0xcf, 0xd0,
    0xd1, 0xe5, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0xd2, 0xd3, 0xd4, 0x5b, 0xd6, 0xd7,
    0xd8, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde, 0xdf, 0xe0, 0xe1, 0xe2, 0xe3, 0xe4, 0x5d, 0xe6, 0xe7,
    0x7b, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0xe8, 0xe9, 0xea, 0xeb, 0xec, 0xed,
    0x7d, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f, 0x50, 0x51, 0x52, 0xee, 0xef, 0xf0, 0xf1, 0xf2, 0xf3,
    0x5c, 0x9f, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9,
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff,
};

static const unsigned char bx_dd_ebcdic_table[256] = {
    0x00, 0x01, 0x02, 0x03, 0x37, 0x2d, 0x2e, 0x2f, 0x16, 0x05, 0x25, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    0x10, 0x11, 0x12, 0x13, 0x3c, 0x3d, 0x32, 0x26, 0x18, 0x19, 0x3f, 0x27, 0x1c, 0x1d, 0x1e, 0x1f,
    0x40, 0x5a, 0x7f, 0x7b, 0x5b, 0x6c, 0x50, 0x7d, 0x4d, 0x5d, 0x5c, 0x4e, 0x6b, 0x60, 0x4b, 0x61,
    0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0x7a, 0x5e, 0x4c, 0x7e, 0x6e, 0x6f,
    0x7c, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6,
    0xd7, 0xd8, 0xd9, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xad, 0xe0, 0xbd, 0x9a, 0x6d,
    0x79, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96,
    0x97, 0x98, 0x99, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xc0, 0x4f, 0xd0, 0x5f, 0x07,
    0x20, 0x21, 0x22, 0x23, 0x24, 0x15, 0x06, 0x17, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x09, 0x0a, 0x1b,
    0x30, 0x31, 0x1a, 0x33, 0x34, 0x35, 0x36, 0x08, 0x38, 0x39, 0x3a, 0x3b, 0x04, 0x14, 0x3e, 0xe1,
    0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57,
    0x58, 0x59, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75,
    0x76, 0x77, 0x78, 0x80, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f, 0x90, 0x6a, 0x9b, 0x9c, 0x9d, 0x9e,
    0x9f, 0xa0, 0xaa, 0xab, 0xac, 0x4a, 0xae, 0xaf, 0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7,
    0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xa1, 0xbe, 0xbf, 0xca, 0xcb, 0xcc, 0xcd, 0xce, 0xcf, 0xda, 0xdb,
    0xdc, 0xdd, 0xde, 0xdf, 0xea, 0xeb, 0xec, 0xed, 0xee, 0xef, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff,
};

static const unsigned char bx_dd_ibm_table[256] = {
    0x00, 0x01, 0x02, 0x03, 0x37, 0x2d, 0x2e, 0x2f, 0x16, 0x05, 0x25, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    0x10, 0x11, 0x12, 0x13, 0x3c, 0x3d, 0x32, 0x26, 0x18, 0x19, 0x3f, 0x27, 0x1c, 0x1d, 0x1e, 0x1f,
    0x40, 0x5a, 0x7f, 0x7b, 0x5b, 0x6c, 0x50, 0x7d, 0x4d, 0x5d, 0x5c, 0x4e, 0x6b, 0x60, 0x4b, 0x61,
    0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0x7a, 0x5e, 0x4c, 0x7e, 0x6e, 0x6f,
    0x7c, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6,
    0xd7, 0xd8, 0xd9, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xad, 0xe0, 0xbd, 0x5f, 0x6d,
    0x79, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96,
    0x97, 0x98, 0x99, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xc0, 0x4f, 0xd0, 0xa1, 0x07,
    0x20, 0x21, 0x22, 0x23, 0x24, 0x15, 0x06, 0x17, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x09, 0x0a, 0x1b,
    0x30, 0x31, 0x1a, 0x33, 0x34, 0x35, 0x36, 0x08, 0x38, 0x39, 0x3a, 0x3b, 0x04, 0x14, 0x3e, 0xe1,
    0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57,
    0x58, 0x59, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75,
    0x76, 0x77, 0x78, 0x80, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f, 0x90, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e,
    0x9f, 0xa0, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf, 0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7,
    0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf, 0xca, 0xcb, 0xcc, 0xcd, 0xce, 0xcf, 0xda, 0xdb,
    0xdc, 0xdd, 0xde, 0xdf, 0xea, 0xeb, 0xec, 0xed, 0xee, 0xef, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff,
};

static bool bx_dd_write_chunk(struct bx_dd_ctx* ctx, const unsigned char* data, size_t len) {
    if (len == 0) {
        return true;
    }

    bool wrote = false;
    if ((ctx->cfg.conv_mask & BX_DD_CONV_SPARSE) != 0u) {
        bool all_zero = true;
        for (size_t i = 0; i < len; i++) {
            if (data[i] != 0) {
                all_zero = false;
                break;
            }
        }

        if (all_zero) {
            off_t skip = (off_t)len;
            if (skip >= 0 && (uintmax_t)skip == (uintmax_t)len && lseek(ctx->outfd, skip, SEEK_CUR) >= 0) {
                wrote = true;
            }
        }
    }

    if (!wrote) {
        if (!bx_xwrite_all(ctx->outfd, data, len)) {
            bx_dd_perror_path(ctx->progname, ctx->output_path);
            return false;
        }
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
        if (ctx->obuf_len == 0 && len - offset >= obs) {
            if (!bx_dd_write_chunk(ctx, data + offset, obs)) {
                return false;
            }
            offset += obs;
            continue;
        }

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

static const unsigned char* bx_dd_input_charset_table(const struct bx_dd_ctx* ctx) {
    if ((ctx->cfg.conv_mask & BX_DD_CONV_ASCII) != 0u) {
        return bx_dd_ascii_table;
    }

    return NULL;
}

static const unsigned char* bx_dd_output_charset_table(const struct bx_dd_ctx* ctx) {
    if ((ctx->cfg.conv_mask & BX_DD_CONV_EBCDIC) != 0u) {
        return bx_dd_ebcdic_table;
    }
    if ((ctx->cfg.conv_mask & BX_DD_CONV_IBM) != 0u) {
        return bx_dd_ibm_table;
    }

    return NULL;
}

static void bx_dd_apply_table(unsigned char* data, size_t len, const unsigned char* table) {
    if (table == NULL) {
        return;
    }

    for (size_t i = 0; i < len; i++) {
        data[i] = table[data[i]];
    }
}

static void bx_dd_apply_case_conversions(const struct bx_dd_ctx* ctx, unsigned char* data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = data[i];
        if ((ctx->cfg.conv_mask & BX_DD_CONV_LCASE) != 0u && ch >= 'A' && ch <= 'Z') {
            data[i] = (unsigned char)(ch - 'A' + 'a');
        }
        else if ((ctx->cfg.conv_mask & BX_DD_CONV_UCASE) != 0u && ch >= 'a' && ch <= 'z') {
            data[i] = (unsigned char)(ch - 'a' + 'A');
        }
    }
}

static void bx_dd_apply_pre_record_conversions(const struct bx_dd_ctx* ctx, unsigned char* data, size_t len) {
    const unsigned char* input_table = bx_dd_input_charset_table(ctx);
    if (input_table != NULL) {
        bx_dd_apply_table(data, len, input_table);
        bx_dd_apply_case_conversions(ctx, data, len);
        return;
    }

    bx_dd_apply_case_conversions(ctx, data, len);
}

static void bx_dd_apply_output_charset(const struct bx_dd_ctx* ctx, unsigned char* data, size_t len) {
    bx_dd_apply_table(data, len, bx_dd_output_charset_table(ctx));
}

static bool bx_dd_queue_block_payload(struct bx_dd_ctx* ctx, unsigned char* data, size_t len) {
    bx_dd_apply_output_charset(ctx, data, len);
    return bx_dd_queue_output(ctx, data, len);
}

static void bx_dd_note_truncated_record(struct bx_dd_ctx* ctx) {
    if (ctx->st.truncated_records < UINTMAX_MAX) {
        ctx->st.truncated_records++;
    }
}

static void bx_dd_enter_block_truncation(struct bx_dd_ctx* ctx) {
    ctx->block_truncating = true;
    ctx->block_truncated_counted = false;
}

static void bx_dd_count_current_truncated_record(struct bx_dd_ctx* ctx) {
    if (!ctx->block_truncated_counted) {
        bx_dd_note_truncated_record(ctx);
        ctx->block_truncated_counted = true;
    }
}

static void bx_dd_clear_block_record(struct bx_dd_ctx* ctx) {
    if (ctx->block_truncating) {
        ctx->block_truncating = false;
        ctx->block_truncated_counted = false;
    }
}

static bool bx_dd_current_block_record_has_data(const struct bx_dd_ctx* ctx) {
    return ctx->cbuf_len > 0;
}

static void bx_dd_maybe_warn_truncated_records(const struct bx_dd_ctx* ctx) {
    if (ctx->st.truncated_records == 0 || ctx->cfg.status_mask == BX_DD_STATUS_NONE) {
        return;
    }

    fprintf(stderr, "%ju truncated record%s\n", ctx->st.truncated_records, (ctx->st.truncated_records == 1) ? "" : "s");
}

static bool bx_dd_queue_block_record(struct bx_dd_ctx* ctx) {
    size_t cbs = (size_t)ctx->cfg.cbs;
    while (ctx->cbuf_len < cbs) {
        ctx->cbuf[ctx->cbuf_len++] = ' ';
    }

    if (!bx_dd_queue_block_payload(ctx, ctx->cbuf, cbs)) {
        return false;
    }

    ctx->cbuf_len = 0;
    bx_dd_clear_block_record(ctx);
    return true;
}

static bool bx_dd_process_block_bytes(struct bx_dd_ctx* ctx, const unsigned char* data, size_t len, bool final) {
    size_t cbs = (size_t)ctx->cfg.cbs;

    for (size_t i = 0; i < len; i++) {
        unsigned char ch = data[i];

        if (ctx->block_truncating) {
            if (ch == '\n') {
                bx_dd_clear_block_record(ctx);
            }
            else {
                bx_dd_count_current_truncated_record(ctx);
            }
            continue;
        }

        if (ch == '\n') {
            if (!bx_dd_queue_block_record(ctx)) {
                return false;
            }
            continue;
        }

        ctx->cbuf[ctx->cbuf_len++] = ch;
        if (ctx->cbuf_len == cbs) {
            if (!bx_dd_queue_block_payload(ctx, ctx->cbuf, cbs)) {
                return false;
            }
            ctx->cbuf_len = 0;
            bx_dd_enter_block_truncation(ctx);
        }
    }

    if (final && bx_dd_current_block_record_has_data(ctx)) {
        return bx_dd_queue_block_record(ctx);
    }

    return true;
}

static bool bx_dd_queue_unblock_record(struct bx_dd_ctx* ctx) {
    size_t len = ctx->cbuf_len;
    while (len > 0 && ctx->cbuf[len - 1] == ' ') {
        len--;
    }

    if (!bx_dd_queue_output(ctx, ctx->cbuf, len)) {
        return false;
    }

    unsigned char newline = '\n';
    if (!bx_dd_queue_output(ctx, &newline, 1)) {
        return false;
    }

    ctx->cbuf_len = 0;
    return true;
}

static bool bx_dd_process_unblock_bytes(struct bx_dd_ctx* ctx, const unsigned char* data, size_t len, bool final) {
    size_t cbs = (size_t)ctx->cfg.cbs;

    for (size_t i = 0; i < len; i++) {
        ctx->cbuf[ctx->cbuf_len++] = data[i];
        if (ctx->cbuf_len == cbs) {
            if (!bx_dd_queue_unblock_record(ctx)) {
                return false;
            }
        }
    }

    if (final && ctx->cbuf_len > 0) {
        return bx_dd_queue_unblock_record(ctx);
    }

    return true;
}

static bool bx_dd_queue_converted_bytes(struct bx_dd_ctx* ctx, unsigned char* data, size_t len, bool final) {
    bx_dd_apply_pre_record_conversions(ctx, data, len);

    if (bx_dd_effective_block(ctx)) {
        return bx_dd_process_block_bytes(ctx, data, len, final);
    }

    if (bx_dd_effective_unblock(ctx)) {
        return bx_dd_process_unblock_bytes(ctx, data, len, final);
    }

    bx_dd_apply_output_charset(ctx, data, len);
    if (len > 0 && !bx_dd_queue_output(ctx, data, len)) {
        return false;
    }

    (void)final;
    return true;
}

static bool bx_dd_process_input_bytes(struct bx_dd_ctx* ctx, unsigned char* data, size_t len) {
    if ((ctx->cfg.conv_mask & BX_DD_CONV_SWAB) == 0u) {
        return bx_dd_queue_converted_bytes(ctx, data, len, false);
    }

    size_t out_len = 0;
    size_t i = 0;

    if (ctx->swab_have_saved && len > 0) {
        ctx->xbuf[out_len++] = data[0];
        ctx->xbuf[out_len++] = ctx->swab_saved;
        ctx->swab_have_saved = false;
        i = 1;
    }

    while (i + 1 < len) {
        ctx->xbuf[out_len++] = data[i + 1];
        ctx->xbuf[out_len++] = data[i];
        i += 2;
    }

    if (i < len) {
        ctx->swab_saved = data[i];
        ctx->swab_have_saved = true;
    }

    return bx_dd_queue_converted_bytes(ctx, ctx->xbuf, out_len, false);
}

static bool bx_dd_finish_conversions(struct bx_dd_ctx* ctx) {
    if ((ctx->cfg.conv_mask & BX_DD_CONV_SWAB) != 0u && ctx->swab_have_saved) {
        unsigned char tail = ctx->swab_saved;
        ctx->swab_have_saved = false;
        if (!bx_dd_queue_converted_bytes(ctx, &tail, 1, false)) {
            return false;
        }
    }

    if (bx_dd_effective_block(ctx)) {
        return bx_dd_process_block_bytes(ctx, NULL, 0, true);
    }

    if (bx_dd_effective_unblock(ctx)) {
        return bx_dd_process_unblock_bytes(ctx, NULL, 0, true);
    }

    return true;
}

static int bx_dd_run(struct bx_dd_ctx* ctx) {
    uintmax_t records_done = 0;
    uintmax_t bytes_remaining = ctx->cfg.count;
    size_t ibs = (size_t)ctx->cfg.ibs;

    while (true) {
        size_t want = ibs;
        if (ctx->cfg.count_set) {
            if (ctx->cfg.count_bytes) {
                if (bytes_remaining == 0) {
                    break;
                }
                if (bytes_remaining < (uintmax_t)want) {
                    want = (size_t)bytes_remaining;
                }
            }
            else if (records_done >= ctx->cfg.count) {
                break;
            }
        }

        size_t nread = 0;
        int read_err = 0;
        bx_dd_read_input(ctx, want, &nread, &read_err);

        if (read_err != 0) {
            if ((ctx->cfg.conv_mask & BX_DD_CONV_NOERROR) == 0u) {
                bx_dd_perror_with_errno(ctx->progname, ctx->input_path, read_err);
                return 1;
            }

            if (ctx->cfg.status_mask != BX_DD_STATUS_NONE) {
                bx_dd_perror_with_errno(ctx->progname, ctx->input_path, read_err);
                bx_dd_print_summary(ctx);
            }

            if (nread > 0) {
                ctx->st.partial_in++;
                size_t out_len = nread;

                if ((ctx->cfg.conv_mask & BX_DD_CONV_SYNC) != 0u && nread < ibs) {
                    unsigned char pad = bx_dd_sync_pad_byte(ctx);
                    memset(ctx->ibuf + nread, pad, ibs - nread);
                    out_len = ibs;
                }

                if (!bx_dd_process_input_bytes(ctx, ctx->ibuf, out_len)) {
                    return 1;
                }

                if (ctx->cfg.count_bytes) {
                    bytes_remaining = (nread >= bytes_remaining) ? 0 : bytes_remaining - (uintmax_t)nread;
                }
                else {
                    records_done++;
                }
                bx_dd_maybe_print_progress(ctx);
                bx_dd_maybe_print_usr1_stats(ctx);
                continue;
            }

            if ((ctx->cfg.conv_mask & BX_DD_CONV_SYNC) != 0u) {
                unsigned char pad = bx_dd_sync_pad_byte(ctx);
                memset(ctx->ibuf, pad, ibs);
                ctx->st.partial_in++;

                if (!bx_dd_process_input_bytes(ctx, ctx->ibuf, ibs)) {
                    return 1;
                }

                if (ctx->cfg.count_bytes) {
                    bytes_remaining = (ibs >= bytes_remaining) ? 0 : bytes_remaining - (uintmax_t)ibs;
                }
                else {
                    records_done++;
                }
                bx_dd_maybe_print_progress(ctx);
                bx_dd_maybe_print_usr1_stats(ctx);
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
            unsigned char pad = bx_dd_sync_pad_byte(ctx);
            memset(ctx->ibuf + nread, pad, ibs - nread);
            out_len = ibs;
        }

        if (!bx_dd_process_input_bytes(ctx, ctx->ibuf, out_len)) {
            return 1;
        }

        if (ctx->cfg.count_bytes) {
            bytes_remaining = (nread >= bytes_remaining) ? 0 : bytes_remaining - (uintmax_t)nread;
        }
        else {
            records_done++;
        }
        bx_dd_maybe_print_progress(ctx);
        bx_dd_maybe_print_usr1_stats(ctx);
    }

    if (!bx_dd_finish_conversions(ctx)) {
        return 1;
    }

    if (!bx_dd_flush_output(ctx, true)) {
        return 1;
    }

    return 0;
}

int bx_dd_main(int argc, char** argv) {
    struct bx_dd_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));

    ctx.progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "dd");
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
        bx_cli_print_version(ctx.progname);
        return 0;
    }

    bx_dd_install_usr1_handler();

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

        if (!bx_dd_drop_cache_if_requested(&ctx, ctx.outfd, ctx.cfg.oflag_mask, ctx.output_path)) {
            rc = 1;
        }
    }

    if (ctx.should_print_stats) {
        bx_dd_print_summary(&ctx);
    }

    if (ctx.infd >= 0 && !bx_dd_drop_cache_if_requested(&ctx, ctx.infd, ctx.cfg.iflag_mask, ctx.input_path)) {
        rc = 1;
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
    free(ctx.xbuf);
    free(ctx.cbuf);

    return rc;
}
