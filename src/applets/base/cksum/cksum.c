#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "applets.h"
#include "crypto/crc32b.h"
#include "crypto/digest_util.h"
#include "crypto/md5.h"
#include "crypto/sha1.h"
#include "crypto/sha256.h"
#include "crypto/sha512.h"
#include "lib/args_common.h"
#include "lib/line_writer.h"
#include "lib/size_parse.h"

enum {
    CKSUM_EXIT_OK = 0,
    CKSUM_EXIT_FAIL = 1,
    CKSUM_EXIT_USAGE = 2,
};

enum cksum_algorithm {
    CKSUM_ALGORITHM_CRC = 0,
    CKSUM_ALGORITHM_CRC32B,
    CKSUM_ALGORITHM_MD5,
    CKSUM_ALGORITHM_SHA1,
    CKSUM_ALGORITHM_SHA224,
    CKSUM_ALGORITHM_SHA256,
    CKSUM_ALGORITHM_SHA384,
    CKSUM_ALGORITHM_SHA512,
    CKSUM_ALGORITHM_SYSV,
    CKSUM_ALGORITHM_BSD,
};

enum cksum_output_mode {
    CKSUM_OUTPUT_AUTO = 0,
    CKSUM_OUTPUT_TAGGED,
    CKSUM_OUTPUT_UNTAGGED,
};

enum cksum_longopt {
    CKSUM_OPT_ALGORITHM = 256,
    CKSUM_OPT_BASE64,
    CKSUM_OPT_CHECK,
    CKSUM_OPT_DEBUG,
    CKSUM_OPT_IGNORE_MISSING,
    CKSUM_OPT_QUIET,
    CKSUM_OPT_RAW,
    CKSUM_OPT_STATUS,
    CKSUM_OPT_STRICT,
    CKSUM_OPT_TAG,
    CKSUM_OPT_UNTAGGED,
    CKSUM_OPT_WARN,
    CKSUM_OPT_ZERO,
};

enum cksum_algorithm_parse_status {
    CKSUM_ALGORITHM_PARSE_OK = 0,
    CKSUM_ALGORITHM_PARSE_UNSUPPORTED,
    CKSUM_ALGORITHM_PARSE_INVALID,
};

struct cksum_options {
    const char* progname;
    enum cksum_algorithm algorithm;
    enum cksum_output_mode output_mode;
    bool algorithm_specified;
    bool check_mode;
    bool debug;
    bool ignore_missing;
    bool quiet;
    bool status;
    bool strict;
    bool warn;
    bool zero_terminated;
    bool raw_output;
    bool length_specified;
    uintmax_t length_bits;
    bool length_invalid;
    const char* invalid_length_text;
    bool show_help;
    bool show_version;
    bool base64_output;
    int first_operand;
};

struct cksum_result {
    enum cksum_algorithm algorithm;
    uint32_t value;
    uintmax_t size;
    uint8_t md5[BX_MD5_DIGEST_SIZE];
    uint8_t sha1[BX_SHA1_DIGEST_SIZE];
    uint8_t sha224[BX_SHA224_DIGEST_SIZE];
    uint8_t sha256[BX_SHA256_DIGEST_SIZE];
    uint8_t sha384[BX_SHA384_DIGEST_SIZE];
    uint8_t sha512[BX_SHA512_DIGEST_SIZE];
};

typedef void (*cksum_update_fn)(void* state, const uint8_t* data, size_t len);

struct cksum_crc_state {
    uint32_t crc;
};

struct cksum_sysv_state {
    uint32_t sum;
};

struct cksum_bsd_state {
    uint16_t sum;
};

struct cksum_md5_state {
    struct bx_md5_ctx ctx;
};

struct cksum_sha1_state {
    struct bx_sha1_ctx ctx;
};

struct cksum_sha224_state {
    struct bx_sha256_ctx ctx;
};

struct cksum_sha256_state {
    struct bx_sha256_ctx ctx;
};

struct cksum_sha384_state {
    struct bx_sha512_ctx ctx;
};

struct cksum_sha512_state {
    struct bx_sha512_ctx ctx;
};

static bool cksum_compute_result(FILE* stream, enum cksum_algorithm algorithm, struct cksum_result* out_result);

static uint32_t cksum_crc_table[256];
static bool cksum_crc_table_ready;

static const char cksum_base64_alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static const char* cksum_progname(const char* argv0) {
    return (argv0 && argv0[0] != '\0') ? argv0 : "cksum";
}

static void cksum_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... [FILE]...\n", progname);
    fprintf(stream, "Print or check checksums and digest values.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -a, --algorithm=TYPE   select algorithm: bsd, sysv, crc, crc32b, md5, sha1, sha224, sha256, sha384, sha512\n");
    fprintf(stream, "      --base64           emit base64-encoded digests, not hexadecimal\n");
    fprintf(stream, "  -c, --check            read checksums from the FILEs and check them\n");
    fprintf(stream, "      --debug            indicate which implementation is used\n");
    fprintf(stream, "      --ignore-missing   don't fail or report status for missing files\n");
    fprintf(stream, "  -l, --length=BITS      digest length in bits (blake2b only)\n");
    fprintf(stream, "      --quiet            don't print OK for each successfully verified file\n");
    fprintf(stream, "      --status           don't output anything, status code shows success\n");
    fprintf(stream, "      --strict           fail if checksum input has improperly formatted lines\n");
    fprintf(stream, "  -w, --warn             warn about improperly formatted checksum lines\n");
    fprintf(stream, "      --raw              emit a raw binary digest, not hexadecimal\n");
    fprintf(stream, "  -z, --zero             end each output line with NUL, not newline\n");
    fprintf(stream, "      --tag              create a BSD-style checksum\n");
    fprintf(stream, "      --untagged         create a reversed style checksum\n");
    fprintf(stream, "      --help             display this help and exit\n");
    fprintf(stream, "      --version          output version information and exit\n");
}

static void cksum_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

static enum cksum_algorithm_parse_status cksum_parse_algorithm_name(const char* text, enum cksum_algorithm* out_algorithm) {
    if (strcmp(text, "crc") == 0) {
        *out_algorithm = CKSUM_ALGORITHM_CRC;
        return CKSUM_ALGORITHM_PARSE_OK;
    }
    if (strcmp(text, "crc32b") == 0) {
        *out_algorithm = CKSUM_ALGORITHM_CRC32B;
        return CKSUM_ALGORITHM_PARSE_OK;
    }
    if (strcmp(text, "md5") == 0) {
        *out_algorithm = CKSUM_ALGORITHM_MD5;
        return CKSUM_ALGORITHM_PARSE_OK;
    }
    if (strcmp(text, "sha1") == 0) {
        *out_algorithm = CKSUM_ALGORITHM_SHA1;
        return CKSUM_ALGORITHM_PARSE_OK;
    }
    if (strcmp(text, "sha224") == 0) {
        *out_algorithm = CKSUM_ALGORITHM_SHA224;
        return CKSUM_ALGORITHM_PARSE_OK;
    }
    if (strcmp(text, "sha256") == 0) {
        *out_algorithm = CKSUM_ALGORITHM_SHA256;
        return CKSUM_ALGORITHM_PARSE_OK;
    }
    if (strcmp(text, "sha384") == 0) {
        *out_algorithm = CKSUM_ALGORITHM_SHA384;
        return CKSUM_ALGORITHM_PARSE_OK;
    }
    if (strcmp(text, "sha512") == 0) {
        *out_algorithm = CKSUM_ALGORITHM_SHA512;
        return CKSUM_ALGORITHM_PARSE_OK;
    }
    if (strcmp(text, "sysv") == 0) {
        *out_algorithm = CKSUM_ALGORITHM_SYSV;
        return CKSUM_ALGORITHM_PARSE_OK;
    }
    if (strcmp(text, "bsd") == 0) {
        *out_algorithm = CKSUM_ALGORITHM_BSD;
        return CKSUM_ALGORITHM_PARSE_OK;
    }

    if (strcmp(text, "blake2b") == 0 || strcmp(text, "sm3") == 0) {
        return CKSUM_ALGORITHM_PARSE_UNSUPPORTED;
    }

    return CKSUM_ALGORITHM_PARSE_INVALID;
}

static bool cksum_parse_length_bits(const char* text, uintmax_t* out_bits) {
    if (text == NULL) {
        return false;
    }

    const char* scan = text;
    while (*scan != '\0' && isspace((unsigned char)*scan)) {
        scan++;
    }
    if (*scan == '\0' || *scan == '-') {
        return false;
    }

    errno = 0;
    char* end = NULL;
    uintmax_t value = strtoumax(text, &end, 10);
    if (end == text || end == NULL || *end != '\0' || errno == ERANGE) {
        return false;
    }

    *out_bits = value;
    return true;
}

static bool cksum_parse_options(int argc, char** argv, struct cksum_options* options) {
    static const struct option long_options[] = {
        {"algorithm", required_argument, NULL, CKSUM_OPT_ALGORITHM},
        {"base64", no_argument, NULL, CKSUM_OPT_BASE64},
        {"check", no_argument, NULL, CKSUM_OPT_CHECK},
        {"debug", no_argument, NULL, CKSUM_OPT_DEBUG},
        {"ignore-missing", no_argument, NULL, CKSUM_OPT_IGNORE_MISSING},
        {"length", required_argument, NULL, 'l'},
        {"quiet", no_argument, NULL, CKSUM_OPT_QUIET},
        {"raw", no_argument, NULL, CKSUM_OPT_RAW},
        {"status", no_argument, NULL, CKSUM_OPT_STATUS},
        {"strict", no_argument, NULL, CKSUM_OPT_STRICT},
        {"tag", no_argument, NULL, CKSUM_OPT_TAG},
        {"untagged", no_argument, NULL, CKSUM_OPT_UNTAGGED},
        {"warn", no_argument, NULL, CKSUM_OPT_WARN},
        {"zero", no_argument, NULL, CKSUM_OPT_ZERO},
        {"help", no_argument, NULL, 1},
        {"version", no_argument, NULL, 2},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = cksum_progname(argv[0]);
    options->algorithm = CKSUM_ALGORITHM_CRC;
    options->output_mode = CKSUM_OUTPUT_AUTO;

    bx_args_getopt_reset();

    while (true) {
        int c = bx_args_getopt_long(argc, argv, "+:a:cl:wz", long_options, NULL);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 'a':
            case CKSUM_OPT_ALGORITHM: {
                enum cksum_algorithm parsed_algorithm = CKSUM_ALGORITHM_CRC;
                enum cksum_algorithm_parse_status status = cksum_parse_algorithm_name(optarg, &parsed_algorithm);
                if (status == CKSUM_ALGORITHM_PARSE_UNSUPPORTED) {
                    fprintf(stderr, "%s: algorithm '%s' is not yet supported\n", options->progname, optarg ? optarg : "");
                    return false;
                }
                if (status == CKSUM_ALGORITHM_PARSE_INVALID) {
                    fprintf(stderr, "%s: invalid argument '%s' for '--algorithm'\n", options->progname, optarg ? optarg : "");
                    return false;
                }
                options->algorithm = parsed_algorithm;
                options->algorithm_specified = true;
                break;
            }
            case 'c':
            case CKSUM_OPT_CHECK:
                options->check_mode = true;
                break;
            case CKSUM_OPT_BASE64:
                options->base64_output = true;
                break;
            case CKSUM_OPT_DEBUG:
                options->debug = true;
                break;
            case CKSUM_OPT_IGNORE_MISSING:
                options->ignore_missing = true;
                break;
            case 'l':
                options->length_specified = true;
                if (!cksum_parse_length_bits(optarg, &options->length_bits)) {
                    options->length_invalid = true;
                    if (options->invalid_length_text == NULL) {
                        options->invalid_length_text = optarg;
                    }
                }
                break;
            case CKSUM_OPT_QUIET:
                options->quiet = true;
                break;
            case CKSUM_OPT_RAW:
                options->raw_output = true;
                break;
            case CKSUM_OPT_STATUS:
                options->status = true;
                break;
            case CKSUM_OPT_STRICT:
                options->strict = true;
                break;
            case 'w':
            case CKSUM_OPT_WARN:
                options->warn = true;
                break;
            case CKSUM_OPT_TAG:
                options->output_mode = CKSUM_OUTPUT_TAGGED;
                break;
            case CKSUM_OPT_UNTAGGED:
                options->output_mode = CKSUM_OUTPUT_UNTAGGED;
                break;
            case 'z':
            case CKSUM_OPT_ZERO:
                options->zero_terminated = true;
                break;
            case 1:
                options->show_help = true;
                break;
            case 2:
                options->show_version = true;
                break;
            case ':':
                if (optopt != 0) {
                    fprintf(stderr, "%s: option requires an argument -- '%c'\n", options->progname, optopt);
                }
                else {
                    fprintf(stderr, "%s: option requires an argument\n", options->progname);
                }
                return false;
            case '?':
                if (optind > 0 && optind <= argc && argv[optind - 1] != NULL) {
                    fprintf(stderr, "%s: unrecognized option '%s'\n", options->progname, argv[optind - 1]);
                }
                else {
                    fprintf(stderr, "%s: unrecognized option\n", options->progname);
                }
                return false;
            default:
                return false;
        }
    }

    options->first_operand = optind;
    return true;
}

static void cksum_crc_init_table(void) {
    if (cksum_crc_table_ready) {
        return;
    }

    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i << 24;
        for (int bit = 0; bit < 8; bit++) {
            if ((crc & 0x80000000u) != 0u) {
                crc = (crc << 1) ^ 0x04C11DB7u;
            }
            else {
                crc <<= 1;
            }
        }
        cksum_crc_table[i] = crc;
    }

    cksum_crc_table_ready = true;
}

static bool cksum_read_stream(FILE* stream, void* state, cksum_update_fn update_fn, uintmax_t* out_size) {
    uint8_t buffer[32768];
    uintmax_t total_size = 0;

    while (true) {
        size_t nread = fread(buffer, 1, sizeof(buffer), stream);
        if (nread > 0) {
            update_fn(state, buffer, nread);
            if ((uintmax_t)nread > UINTMAX_MAX - total_size) {
                errno = EOVERFLOW;
                return false;
            }
            total_size += (uintmax_t)nread;
        }

        if (nread < sizeof(buffer)) {
            if (ferror(stream)) {
                if (errno == 0) {
                    errno = EIO;
                }
                return false;
            }
            break;
        }
    }

    *out_size = total_size;
    return true;
}

static void cksum_crc_update(void* opaque, const uint8_t* data, size_t len) {
    struct cksum_crc_state* state = (struct cksum_crc_state*)opaque;
    uint32_t crc = state->crc;

    for (size_t i = 0; i < len; i++) {
        crc = (crc << 8) ^ cksum_crc_table[((crc >> 24) ^ data[i]) & 0xffu];
    }

    state->crc = crc;
}

static uint32_t cksum_crc_finalize(uint32_t crc, uintmax_t size) {
    while (size != 0) {
        uint8_t byte = (uint8_t)(size & 0xffu);
        crc = (crc << 8) ^ cksum_crc_table[((crc >> 24) ^ byte) & 0xffu];
        size >>= 8;
    }

    return ~crc;
}

static void cksum_crc32b_update(void* opaque, const uint8_t* data, size_t len) {
    bx_crc32b_update((struct bx_crc32b_ctx*)opaque, data, len);
}

static void cksum_sysv_update(void* opaque, const uint8_t* data, size_t len) {
    struct cksum_sysv_state* state = (struct cksum_sysv_state*)opaque;

    for (size_t i = 0; i < len; i++) {
        state->sum += data[i];
    }
}

static uint32_t cksum_sysv_finalize(uint32_t sum) {
    uint32_t folded = (sum & 0xffffu) + (sum >> 16);
    folded = (folded & 0xffffu) + (folded >> 16);
    return folded & 0xffffu;
}

static void cksum_bsd_update(void* opaque, const uint8_t* data, size_t len) {
    struct cksum_bsd_state* state = (struct cksum_bsd_state*)opaque;
    uint16_t sum = state->sum;

    for (size_t i = 0; i < len; i++) {
        if ((sum & 1u) != 0u) {
            sum = (uint16_t)((sum >> 1) + 0x8000u);
        }
        else {
            sum >>= 1;
        }
        sum = (uint16_t)((sum + data[i]) & 0xffffu);
    }

    state->sum = sum;
}

static void cksum_md5_update(void* opaque, const uint8_t* data, size_t len) {
    struct cksum_md5_state* state = (struct cksum_md5_state*)opaque;
    bx_md5_update(&state->ctx, data, len);
}

static void cksum_sha1_update(void* opaque, const uint8_t* data, size_t len) {
    struct cksum_sha1_state* state = (struct cksum_sha1_state*)opaque;
    bx_sha1_update(&state->ctx, data, len);
}

static void cksum_sha224_update(void* opaque, const uint8_t* data, size_t len) {
    struct cksum_sha224_state* state = (struct cksum_sha224_state*)opaque;
    bx_sha256_update(&state->ctx, data, len);
}

static void cksum_sha256_update(void* opaque, const uint8_t* data, size_t len) {
    struct cksum_sha256_state* state = (struct cksum_sha256_state*)opaque;
    bx_sha256_update(&state->ctx, data, len);
}

static void cksum_sha384_update(void* opaque, const uint8_t* data, size_t len) {
    struct cksum_sha384_state* state = (struct cksum_sha384_state*)opaque;
    bx_sha512_update(&state->ctx, data, len);
}

static void cksum_sha512_update(void* opaque, const uint8_t* data, size_t len) {
    struct cksum_sha512_state* state = (struct cksum_sha512_state*)opaque;
    bx_sha512_update(&state->ctx, data, len);
}

static void cksum_md5_init_adapter(void* ctx) {
    bx_md5_init((struct bx_md5_ctx*)ctx);
}

static void cksum_md5_update_adapter(void* ctx, const void* data, size_t len) {
    bx_md5_update((struct bx_md5_ctx*)ctx, data, len);
}

static void cksum_md5_final_adapter(void* ctx, uint8_t* out) {
    bx_md5_final((struct bx_md5_ctx*)ctx, out);
}

static int cksum_md5_hash_path(const char* path, uint8_t out[BX_MD5_DIGEST_SIZE]) {
    struct bx_md5_ctx ctx;

    return bx_digest_file(&ctx, sizeof(ctx), cksum_md5_init_adapter, cksum_md5_update_adapter, cksum_md5_final_adapter, path, out, BX_MD5_DIGEST_SIZE);
}

static void cksum_sha1_init_adapter(void* ctx) {
    bx_sha1_init((struct bx_sha1_ctx*)ctx);
}

static void cksum_sha1_update_adapter(void* ctx, const void* data, size_t len) {
    bx_sha1_update((struct bx_sha1_ctx*)ctx, data, len);
}

static void cksum_sha1_final_adapter(void* ctx, uint8_t* out) {
    bx_sha1_final((struct bx_sha1_ctx*)ctx, out);
}

static int cksum_sha1_hash_path(const char* path, uint8_t out[BX_SHA1_DIGEST_SIZE]) {
    struct bx_sha1_ctx ctx;

    return bx_digest_file(&ctx, sizeof(ctx), cksum_sha1_init_adapter, cksum_sha1_update_adapter, cksum_sha1_final_adapter, path, out, BX_SHA1_DIGEST_SIZE);
}

static void cksum_sha224_init_adapter(void* ctx) {
    bx_sha224_init((struct bx_sha256_ctx*)ctx);
}

static void cksum_sha256_init_adapter(void* ctx) {
    bx_sha256_init((struct bx_sha256_ctx*)ctx);
}

static void cksum_sha256_update_adapter(void* ctx, const void* data, size_t len) {
    bx_sha256_update((struct bx_sha256_ctx*)ctx, data, len);
}

static void cksum_sha224_final_adapter(void* ctx, uint8_t* out) {
    bx_sha224_final((struct bx_sha256_ctx*)ctx, out);
}

static void cksum_sha256_final_adapter(void* ctx, uint8_t* out) {
    bx_sha256_final((struct bx_sha256_ctx*)ctx, out);
}

static int cksum_sha224_hash_path(const char* path, uint8_t out[BX_SHA224_DIGEST_SIZE]) {
    struct bx_sha256_ctx ctx;

    return bx_digest_file(&ctx, sizeof(ctx), cksum_sha224_init_adapter, cksum_sha256_update_adapter, cksum_sha224_final_adapter, path, out, BX_SHA224_DIGEST_SIZE);
}

static int cksum_sha256_hash_path(const char* path, uint8_t out[BX_SHA256_DIGEST_SIZE]) {
    struct bx_sha256_ctx ctx;

    return bx_digest_file(&ctx, sizeof(ctx), cksum_sha256_init_adapter, cksum_sha256_update_adapter, cksum_sha256_final_adapter, path, out, BX_SHA256_DIGEST_SIZE);
}

static void cksum_sha384_init_adapter(void* ctx) {
    bx_sha384_init((struct bx_sha512_ctx*)ctx);
}

static void cksum_sha512_init_adapter(void* ctx) {
    bx_sha512_init((struct bx_sha512_ctx*)ctx);
}

static void cksum_sha512_update_adapter(void* ctx, const void* data, size_t len) {
    bx_sha512_update((struct bx_sha512_ctx*)ctx, data, len);
}

static void cksum_sha384_final_adapter(void* ctx, uint8_t* out) {
    bx_sha384_final((struct bx_sha512_ctx*)ctx, out);
}

static void cksum_sha512_final_adapter(void* ctx, uint8_t* out) {
    bx_sha512_final((struct bx_sha512_ctx*)ctx, out);
}

static int cksum_sha384_hash_path(const char* path, uint8_t out[BX_SHA384_DIGEST_SIZE]) {
    struct bx_sha512_ctx ctx;

    return bx_digest_file(&ctx, sizeof(ctx), cksum_sha384_init_adapter, cksum_sha512_update_adapter, cksum_sha384_final_adapter, path, out, BX_SHA384_DIGEST_SIZE);
}

static int cksum_sha512_hash_path(const char* path, uint8_t out[BX_SHA512_DIGEST_SIZE]) {
    struct bx_sha512_ctx ctx;

    return bx_digest_file(&ctx, sizeof(ctx), cksum_sha512_init_adapter, cksum_sha512_update_adapter, cksum_sha512_final_adapter, path, out, BX_SHA512_DIGEST_SIZE);
}

static int cksum_hex_value(int ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

static int cksum_base64_value(int ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return ch - 'A';
    }
    if (ch >= 'a' && ch <= 'z') {
        return ch - 'a' + 26;
    }
    if (ch >= '0' && ch <= '9') {
        return ch - '0' + 52;
    }
    if (ch == '+') {
        return 62;
    }
    if (ch == '/') {
        return 63;
    }
    return -1;
}

static size_t cksum_base64_encoded_length(size_t input_len) {
    return ((input_len + 2u) / 3u) * 4u;
}

static void cksum_base64_encode(const uint8_t* in, size_t len, char* out) {
    size_t in_i = 0u;
    size_t out_i = 0u;

    while ((len - in_i) >= 3u) {
        uint8_t a = in[in_i];
        uint8_t b = in[in_i + 1u];
        uint8_t c = in[in_i + 2u];

        out[out_i++] = cksum_base64_alphabet[a >> 2u];
        out[out_i++] = cksum_base64_alphabet[((a & 0x03u) << 4u) | (b >> 4u)];
        out[out_i++] = cksum_base64_alphabet[((b & 0x0fu) << 2u) | (c >> 6u)];
        out[out_i++] = cksum_base64_alphabet[c & 0x3fu];
        in_i += 3u;
    }

    if ((len - in_i) == 1u) {
        uint8_t a = in[in_i];
        out[out_i++] = cksum_base64_alphabet[a >> 2u];
        out[out_i++] = cksum_base64_alphabet[(a & 0x03u) << 4u];
        out[out_i++] = '=';
        out[out_i++] = '=';
    }
    else if ((len - in_i) == 2u) {
        uint8_t a = in[in_i];
        uint8_t b = in[in_i + 1u];
        out[out_i++] = cksum_base64_alphabet[a >> 2u];
        out[out_i++] = cksum_base64_alphabet[((a & 0x03u) << 4u) | (b >> 4u)];
        out[out_i++] = cksum_base64_alphabet[(b & 0x0fu) << 2u];
        out[out_i++] = '=';
    }

    out[out_i] = '\0';
}

static bool cksum_decode_base64(const char* text, size_t expected_len, uint8_t* out) {
    size_t text_len = strlen(text);
    size_t expected_text_len = cksum_base64_encoded_length(expected_len);
    size_t out_i = 0u;

    if (text_len != expected_text_len) {
        return false;
    }

    for (size_t i = 0; i < text_len; i += 4u) {
        int v0 = cksum_base64_value((unsigned char)text[i]);
        int v1 = cksum_base64_value((unsigned char)text[i + 1u]);
        char c2 = text[i + 2u];
        char c3 = text[i + 3u];
        int v2 = (c2 == '=') ? 0 : cksum_base64_value((unsigned char)c2);
        int v3 = (c3 == '=') ? 0 : cksum_base64_value((unsigned char)c3);

        if (v0 < 0 || v1 < 0 || v2 < 0 || v3 < 0) {
            return false;
        }
        if (c2 == '=' && c3 != '=') {
            return false;
        }
        if ((c2 == '=' || c3 == '=') && (i + 4u) != text_len) {
            return false;
        }

        if (out_i >= expected_len) {
            return false;
        }
        out[out_i++] = (uint8_t)((v0 << 2) | (v1 >> 4));

        if (c2 != '=') {
            if (out_i >= expected_len) {
                return false;
            }
            out[out_i++] = (uint8_t)(((v1 & 0x0f) << 4) | (v2 >> 2));
        }
        if (c3 != '=') {
            if (out_i >= expected_len) {
                return false;
            }
            out[out_i++] = (uint8_t)(((v2 & 0x03) << 6) | v3);
        }
    }

    return out_i == expected_len;
}

static bool cksum_parse_digest_text(const char* text, size_t digest_len, uint8_t* out) {
    const size_t digest_hex_len = digest_len * 2u;

    if (strlen(text) == digest_hex_len) {
        for (size_t i = 0; i < digest_len; i++) {
            int hi = cksum_hex_value((unsigned char)text[i * 2u]);
            int lo = cksum_hex_value((unsigned char)text[i * 2u + 1u]);
            if (hi < 0 || lo < 0) {
                return false;
            }
            out[i] = (uint8_t)((hi << 4) | lo);
        }
        return true;
    }

    return cksum_decode_base64(text, digest_len, out);
}

static bool cksum_parse_untagged_base64_line(char* line, size_t digest_len, struct bx_checksum_record* record) {
    const size_t digest_b64_len = cksum_base64_encoded_length(digest_len);
    char saved_sep = '\0';

    if (strlen(line) < (digest_b64_len + 2u)) {
        return false;
    }
    if (line[digest_b64_len] != ' ') {
        return false;
    }

    if (line[digest_b64_len + 1u] == '*') {
        record->binary_mode = true;
    }
    else if (line[digest_b64_len + 1u] == ' ') {
        record->binary_mode = false;
    }
    else {
        return false;
    }

    saved_sep = line[digest_b64_len];
    line[digest_b64_len] = '\0';
    bool digest_ok = cksum_decode_base64(line, digest_len, record->digest);
    line[digest_b64_len] = saved_sep;
    if (!digest_ok) {
        return false;
    }

    record->digest_len = digest_len;
    record->filename = line + digest_b64_len + 2u;
    return record->filename[0] != '\0';
}

static bool cksum_parse_tagged_line(char* line, const char* prefix, size_t digest_len, struct bx_checksum_record* record) {
    size_t prefix_len = strlen(prefix);

    if (strncmp(line, prefix, prefix_len) != 0) {
        return false;
    }

    char* close_paren = strrchr(line, ')');
    if (close_paren == NULL || close_paren < line + prefix_len) {
        return false;
    }
    if (strncmp(close_paren, ") = ", 4u) != 0) {
        return false;
    }

    const char* digest_text = close_paren + 4;
    if (!cksum_parse_digest_text(digest_text, digest_len, record->digest)) {
        return false;
    }

    *close_paren = '\0';
    record->digest_len = digest_len;
    record->binary_mode = false;
    record->filename = line + prefix_len;
    return record->filename[0] != '\0';
}

static bool cksum_parse_md5_tagged_line(char* line, struct bx_checksum_record* record) {
    return cksum_parse_tagged_line(line, "MD5 (", BX_MD5_DIGEST_SIZE, record);
}

static bool cksum_parse_sha1_tagged_line(char* line, struct bx_checksum_record* record) {
    return cksum_parse_tagged_line(line, "SHA1 (", BX_SHA1_DIGEST_SIZE, record);
}

static bool cksum_parse_sha224_tagged_line(char* line, struct bx_checksum_record* record) {
    return cksum_parse_tagged_line(line, "SHA224 (", BX_SHA224_DIGEST_SIZE, record);
}

static bool cksum_parse_sha256_tagged_line(char* line, struct bx_checksum_record* record) {
    return cksum_parse_tagged_line(line, "SHA256 (", BX_SHA256_DIGEST_SIZE, record);
}

static bool cksum_parse_sha384_tagged_line(char* line, struct bx_checksum_record* record) {
    return cksum_parse_tagged_line(line, "SHA384 (", BX_SHA384_DIGEST_SIZE, record);
}

static bool cksum_parse_sha512_tagged_line(char* line, struct bx_checksum_record* record) {
    return cksum_parse_tagged_line(line, "SHA512 (", BX_SHA512_DIGEST_SIZE, record);
}

static bool cksum_algorithm_is_numeric(enum cksum_algorithm algorithm) {
    return algorithm == CKSUM_ALGORITHM_CRC || algorithm == CKSUM_ALGORITHM_CRC32B || algorithm == CKSUM_ALGORITHM_SYSV || algorithm == CKSUM_ALGORITHM_BSD;
}

static bool cksum_parse_numeric_check_field(const char* text, uintmax_t max_value, uintmax_t* out_value, char** out_end) {
    if (text == NULL || out_value == NULL || out_end == NULL) {
        return false;
    }

    errno = 0;
    char* end = NULL;
    uintmax_t parsed = strtoumax(text, &end, 10);
    if (end == text || end == NULL || errno != 0 || parsed > max_value) {
        return false;
    }

    *out_value = parsed;
    *out_end = end;
    return true;
}

static bool cksum_parse_numeric_check_line(char* line, uint32_t* out_value, uintmax_t* out_metric, const char** out_filename) {
    if (line == NULL || out_value == NULL || out_metric == NULL || out_filename == NULL) {
        return false;
    }

    char* value_end = NULL;
    uintmax_t parsed_value = 0;
    if (!cksum_parse_numeric_check_field(line, UINT32_MAX, &parsed_value, &value_end)) {
        return false;
    }
    if (*value_end != ' ') {
        return false;
    }

    char* metric_start = value_end + 1;
    char* metric_end = NULL;
    uintmax_t parsed_metric = 0;
    if (!cksum_parse_numeric_check_field(metric_start, UINTMAX_MAX, &parsed_metric, &metric_end)) {
        return false;
    }
    if (*metric_end != ' ') {
        return false;
    }

    const char* filename = metric_end + 1;
    if (*filename == '\0') {
        return false;
    }

    *out_value = (uint32_t)parsed_value;
    *out_metric = parsed_metric;
    *out_filename = filename;
    return true;
}

static bool cksum_parse_check_record(const struct cksum_options* options, char* line, struct bx_checksum_record* record) {
    if (options->algorithm_specified) {
        if (options->algorithm == CKSUM_ALGORITHM_MD5) {
            if (bx_parse_check_line(line, BX_MD5_DIGEST_SIZE, record)) {
                return true;
            }
            if (cksum_parse_untagged_base64_line(line, BX_MD5_DIGEST_SIZE, record)) {
                return true;
            }
            return cksum_parse_md5_tagged_line(line, record);
        }
        if (options->algorithm == CKSUM_ALGORITHM_SHA1) {
            if (bx_parse_check_line(line, BX_SHA1_DIGEST_SIZE, record)) {
                return true;
            }
            if (cksum_parse_untagged_base64_line(line, BX_SHA1_DIGEST_SIZE, record)) {
                return true;
            }
            return cksum_parse_sha1_tagged_line(line, record);
        }
        if (options->algorithm == CKSUM_ALGORITHM_SHA224) {
            if (bx_parse_check_line(line, BX_SHA224_DIGEST_SIZE, record)) {
                return true;
            }
            if (cksum_parse_untagged_base64_line(line, BX_SHA224_DIGEST_SIZE, record)) {
                return true;
            }
            return cksum_parse_sha224_tagged_line(line, record);
        }
        if (options->algorithm == CKSUM_ALGORITHM_SHA256) {
            if (bx_parse_check_line(line, BX_SHA256_DIGEST_SIZE, record)) {
                return true;
            }
            if (cksum_parse_untagged_base64_line(line, BX_SHA256_DIGEST_SIZE, record)) {
                return true;
            }
            return cksum_parse_sha256_tagged_line(line, record);
        }
        if (options->algorithm == CKSUM_ALGORITHM_SHA384) {
            if (bx_parse_check_line(line, BX_SHA384_DIGEST_SIZE, record)) {
                return true;
            }
            if (cksum_parse_untagged_base64_line(line, BX_SHA384_DIGEST_SIZE, record)) {
                return true;
            }
            return cksum_parse_sha384_tagged_line(line, record);
        }
        if (options->algorithm == CKSUM_ALGORITHM_SHA512) {
            if (bx_parse_check_line(line, BX_SHA512_DIGEST_SIZE, record)) {
                return true;
            }
            if (cksum_parse_untagged_base64_line(line, BX_SHA512_DIGEST_SIZE, record)) {
                return true;
            }
            return cksum_parse_sha512_tagged_line(line, record);
        }
        return false;
    }

    if (cksum_parse_md5_tagged_line(line, record)) {
        return true;
    }

    if (cksum_parse_sha1_tagged_line(line, record)) {
        return true;
    }

    if (cksum_parse_sha224_tagged_line(line, record)) {
        return true;
    }
    if (cksum_parse_sha256_tagged_line(line, record)) {
        return true;
    }
    if (cksum_parse_sha384_tagged_line(line, record)) {
        return true;
    }
    return cksum_parse_sha512_tagged_line(line, record);
}

static bool cksum_check_algorithm_supported(enum cksum_algorithm algorithm) {
    return algorithm == CKSUM_ALGORITHM_MD5 || algorithm == CKSUM_ALGORITHM_SHA1 || algorithm == CKSUM_ALGORITHM_SHA224 || algorithm == CKSUM_ALGORITHM_SHA256 || algorithm == CKSUM_ALGORITHM_SHA384 ||
           algorithm == CKSUM_ALGORITHM_SHA512;
}

static bool cksum_infer_check_algorithm(const struct cksum_options* options, const struct bx_checksum_record* record, enum cksum_algorithm* out_algorithm) {
    if (options->algorithm_specified) {
        if (!cksum_check_algorithm_supported(options->algorithm)) {
            return false;
        }
        *out_algorithm = options->algorithm;
        return true;
    }

    if (record->digest_len == BX_MD5_DIGEST_SIZE) {
        *out_algorithm = CKSUM_ALGORITHM_MD5;
        return true;
    }
    if (record->digest_len == BX_SHA1_DIGEST_SIZE) {
        *out_algorithm = CKSUM_ALGORITHM_SHA1;
        return true;
    }
    if (record->digest_len == BX_SHA224_DIGEST_SIZE) {
        *out_algorithm = CKSUM_ALGORITHM_SHA224;
        return true;
    }
    if (record->digest_len == BX_SHA256_DIGEST_SIZE) {
        *out_algorithm = CKSUM_ALGORITHM_SHA256;
        return true;
    }
    if (record->digest_len == BX_SHA384_DIGEST_SIZE) {
        *out_algorithm = CKSUM_ALGORITHM_SHA384;
        return true;
    }
    if (record->digest_len == BX_SHA512_DIGEST_SIZE) {
        *out_algorithm = CKSUM_ALGORITHM_SHA512;
        return true;
    }
    return false;
}

static int cksum_hash_path(enum cksum_algorithm algorithm, const char* path, uint8_t* out, size_t* out_len) {
    switch (algorithm) {
        case CKSUM_ALGORITHM_MD5:
            if (cksum_md5_hash_path(path, out) != 0) {
                return -1;
            }
            *out_len = BX_MD5_DIGEST_SIZE;
            return 0;
        case CKSUM_ALGORITHM_SHA1:
            if (cksum_sha1_hash_path(path, out) != 0) {
                return -1;
            }
            *out_len = BX_SHA1_DIGEST_SIZE;
            return 0;
        case CKSUM_ALGORITHM_SHA224:
            if (cksum_sha224_hash_path(path, out) != 0) {
                return -1;
            }
            *out_len = BX_SHA224_DIGEST_SIZE;
            return 0;
        case CKSUM_ALGORITHM_SHA256:
            if (cksum_sha256_hash_path(path, out) != 0) {
                return -1;
            }
            *out_len = BX_SHA256_DIGEST_SIZE;
            return 0;
        case CKSUM_ALGORITHM_SHA384:
            if (cksum_sha384_hash_path(path, out) != 0) {
                return -1;
            }
            *out_len = BX_SHA384_DIGEST_SIZE;
            return 0;
        case CKSUM_ALGORITHM_SHA512:
            if (cksum_sha512_hash_path(path, out) != 0) {
                return -1;
            }
            *out_len = BX_SHA512_DIGEST_SIZE;
            return 0;
        default:
            errno = EINVAL;
            return -1;
    }
}

static int cksum_hash_numeric_path(enum cksum_algorithm algorithm, const char* path, uint32_t* out_value, uintmax_t* out_metric) {
    FILE* stream = NULL;
    bool is_stdin = false;

    if (strcmp(path, "-") == 0) {
        stream = stdin;
        is_stdin = true;
    }
    else {
        stream = fopen(path, "rb");
        if (stream == NULL) {
            return -1;
        }
    }

    struct cksum_result result;
    bool ok = cksum_compute_result(stream, algorithm, &result);
    int saved_errno = errno;

    if (!is_stdin) {
        fclose(stream);
    }

    errno = saved_errno;
    if (!ok) {
        return -1;
    }

    *out_value = result.value;
    switch (algorithm) {
        case CKSUM_ALGORITHM_CRC:
        case CKSUM_ALGORITHM_CRC32B:
            *out_metric = result.size;
            return 0;
        case CKSUM_ALGORITHM_SYSV:
            if (!bx_size_block_count_ceil(result.size, 512u, out_metric)) {
                errno = EINVAL;
                return -1;
            }
            return 0;
        case CKSUM_ALGORITHM_BSD:
            if (!bx_size_block_count_ceil(result.size, 1024u, out_metric)) {
                errno = EINVAL;
                return -1;
            }
            return 0;
        case CKSUM_ALGORITHM_MD5:
        case CKSUM_ALGORITHM_SHA1:
        case CKSUM_ALGORITHM_SHA224:
        case CKSUM_ALGORITHM_SHA256:
        case CKSUM_ALGORITHM_SHA384:
        case CKSUM_ALGORITHM_SHA512:
            errno = EINVAL;
            return -1;
    }

    errno = EINVAL;
    return -1;
}

static const char* cksum_plural_suffix(size_t count) {
    return (count == 1u) ? "" : "s";
}

static bool cksum_write_status_line(struct bx_line_writer* writer, const char* filename, const char* suffix) {
    return bx_line_writer_puts(writer, filename)
        && bx_line_writer_puts(writer, suffix)
        && bx_line_writer_putc(writer, '\n');
}

static bool cksum_write_numeric_result(struct bx_line_writer* writer,
                                       uint32_t value,
                                       uintmax_t metric,
                                       const char* path,
                                       bool show_name,
                                       bool bsd_format,
                                       char terminator) {
    char prefix[96];
    int len = bsd_format
        ? snprintf(prefix, sizeof(prefix), "%05" PRIu32 " %5" PRIuMAX, value, metric)
        : snprintf(prefix, sizeof(prefix), "%" PRIu32 " %" PRIuMAX, value, metric);

    return len >= 0 && (size_t)len < sizeof(prefix)
        && bx_line_writer_write(writer, prefix, (size_t)len)
        && (!show_name || (bx_line_writer_putc(writer, ' ') && bx_line_writer_puts(writer, path)))
        && bx_line_writer_putc(writer, terminator);
}

static bool cksum_write_digest_result(struct bx_line_writer* writer,
                                      const char* algorithm_label,
                                      const char* digest_text,
                                      const char* path,
                                      bool tagged,
                                      char terminator) {
    if (tagged) {
        return bx_line_writer_puts(writer, algorithm_label)
            && bx_line_writer_write(writer, " (", 2u)
            && bx_line_writer_puts(writer, path)
            && bx_line_writer_write(writer, ") = ", 4u)
            && bx_line_writer_puts(writer, digest_text)
            && bx_line_writer_putc(writer, terminator);
    }

    return bx_line_writer_puts(writer, digest_text)
        && bx_line_writer_write(writer, "  ", 2u)
        && bx_line_writer_puts(writer, path)
        && bx_line_writer_putc(writer, terminator);
}

static int cksum_verify_stream(FILE* stream, const char* source_name, const struct cksum_options* options, struct bx_line_writer* writer) {
    char* line = NULL;
    size_t cap = 0u;
    size_t line_no = 0u;
    size_t parsed_count = 0u;
    size_t malformed_count = 0u;
    size_t success_count = 0u;
    size_t mismatch_count = 0u;
    size_t read_fail_count = 0u;
    bool failed = false;
    bool numeric_check_mode = options->algorithm_specified && cksum_algorithm_is_numeric(options->algorithm);

    while (true) {
        ssize_t nread = getline(&line, &cap, stream);
        if (nread < 0) {
            break;
        }

        line_no++;
        if (nread > 0 && line[nread - 1] == '\n') {
            line[nread - 1] = '\0';
        }

        if (numeric_check_mode) {
            uint32_t expected_value = 0u;
            uintmax_t expected_metric = 0u;
            const char* expected_filename = NULL;

            if (!cksum_parse_numeric_check_line(line, &expected_value, &expected_metric, &expected_filename)) {
                malformed_count++;
                if (options->warn && !options->status) {
                    fprintf(stderr, "%s: %s:%zu: improperly formatted checksum line\n", options->progname, source_name, line_no);
                }
                continue;
            }

            parsed_count++;

            uint32_t actual_value = 0u;
            uintmax_t actual_metric = 0u;
            if (cksum_hash_numeric_path(options->algorithm, expected_filename, &actual_value, &actual_metric) != 0) {
                int saved_errno = errno;
                if (options->ignore_missing && saved_errno == ENOENT) {
                    continue;
                }

                fprintf(stderr, "%s: %s: %s\n", options->progname, expected_filename, strerror(saved_errno));
                if (!options->status) {
                    if (!cksum_write_status_line(writer, expected_filename, ": FAILED open or read")) {
                        free(line);
                        return CKSUM_EXIT_FAIL;
                    }
                }
                read_fail_count++;
                failed = true;
                continue;
            }

            if (actual_value == expected_value && actual_metric == expected_metric) {
                success_count++;
                if (!options->quiet && !options->status) {
                    if (!cksum_write_status_line(writer, expected_filename, ": OK")) {
                        free(line);
                        return CKSUM_EXIT_FAIL;
                    }
                }
                continue;
            }

            if (!options->status) {
                if (!cksum_write_status_line(writer, expected_filename, ": FAILED")) {
                    free(line);
                    return CKSUM_EXIT_FAIL;
                }
            }
            mismatch_count++;
            failed = true;
            continue;
        }

        struct bx_checksum_record expected;
        if (!cksum_parse_check_record(options, line, &expected)) {
            malformed_count++;
            if (options->warn && !options->status) {
                fprintf(stderr, "%s: %s:%zu: improperly formatted checksum line\n", options->progname, source_name, line_no);
            }
            continue;
        }
        enum cksum_algorithm check_algorithm = CKSUM_ALGORITHM_MD5;
        if (!cksum_infer_check_algorithm(options, &expected, &check_algorithm)) {
            malformed_count++;
            if (options->warn && !options->status) {
                fprintf(stderr, "%s: %s:%zu: improperly formatted checksum line\n", options->progname, source_name, line_no);
            }
            continue;
        }
        parsed_count++;

        uint8_t actual[sizeof(expected.digest)];
        size_t actual_len = 0u;
        if (cksum_hash_path(check_algorithm, expected.filename, actual, &actual_len) != 0) {
            int saved_errno = errno;
            if (options->ignore_missing && saved_errno == ENOENT) {
                continue;
            }

            fprintf(stderr, "%s: %s: %s\n", options->progname, expected.filename, strerror(saved_errno));
            if (!options->status) {
                if (!cksum_write_status_line(writer, expected.filename, ": FAILED open or read")) {
                    free(line);
                    return CKSUM_EXIT_FAIL;
                }
            }
            read_fail_count++;
            failed = true;
            continue;
        }

        if (actual_len == expected.digest_len && memcmp(actual, expected.digest, expected.digest_len) == 0) {
            success_count++;
            if (!options->quiet && !options->status) {
                if (!cksum_write_status_line(writer, expected.filename, ": OK")) {
                    free(line);
                    return CKSUM_EXIT_FAIL;
                }
            }
            continue;
        }

        if (!options->status) {
            if (!cksum_write_status_line(writer, expected.filename, ": FAILED")) {
                free(line);
                return CKSUM_EXIT_FAIL;
            }
        }
        mismatch_count++;
        failed = true;
    }

    if (ferror(stream)) {
        fprintf(stderr, "%s: %s: read error\n", options->progname, source_name);
        failed = true;
    }

    if (parsed_count == 0u) {
        fprintf(stderr, "%s: %s: no properly formatted checksum lines found\n", options->progname, source_name);
        failed = true;
    }

    if (malformed_count > 0u) {
        if (!options->status) {
            fprintf(stderr, "%s: WARNING: %zu line%s is improperly formatted\n", options->progname, malformed_count, cksum_plural_suffix(malformed_count));
        }
        if (options->strict) {
            failed = true;
        }
    }

    if (mismatch_count > 0u && !options->status) {
        fprintf(stderr, "%s: WARNING: %zu computed checksum%s did NOT match\n", options->progname, mismatch_count, cksum_plural_suffix(mismatch_count));
    }

    if (read_fail_count > 0u && !options->status) {
        fprintf(stderr, "%s: WARNING: %zu listed file%s could not be read\n", options->progname, read_fail_count, cksum_plural_suffix(read_fail_count));
    }

    if (options->ignore_missing && parsed_count > 0u && success_count == 0u) {
        if (!options->status) {
            fprintf(stderr, "%s: %s: no file was verified\n", options->progname, source_name);
        }
        failed = true;
    }

    free(line);
    return failed ? CKSUM_EXIT_FAIL : CKSUM_EXIT_OK;
}

static int cksum_check_file(const struct cksum_options* options, const char* path, struct bx_line_writer* writer) {
    if (strcmp(path, "-") == 0) {
        return cksum_verify_stream(stdin, "-", options, writer);
    }

    FILE* stream = fopen(path, "r");
    if (stream == NULL) {
        fprintf(stderr, "%s: %s: %s\n", options->progname, path, strerror(errno));
        return CKSUM_EXIT_FAIL;
    }

    int rc = cksum_verify_stream(stream, path, options, writer);
    fclose(stream);
    return rc;
}

static bool cksum_compute_result(FILE* stream, enum cksum_algorithm algorithm, struct cksum_result* out_result) {
    memset(out_result, 0, sizeof(*out_result));
    out_result->algorithm = algorithm;

    switch (algorithm) {
        case CKSUM_ALGORITHM_CRC: {
            cksum_crc_init_table();
            struct cksum_crc_state state = {
                .crc = 0,
            };
            if (!cksum_read_stream(stream, &state, cksum_crc_update, &out_result->size)) {
                return false;
            }
            out_result->value = cksum_crc_finalize(state.crc, out_result->size);
            return true;
        }
        case CKSUM_ALGORITHM_CRC32B: {
            struct bx_crc32b_ctx state;
            bx_crc32b_init(&state);
            if (!cksum_read_stream(stream, &state, cksum_crc32b_update, &out_result->size)) {
                return false;
            }
            out_result->value = bx_crc32b_final(&state);
            return true;
        }
        case CKSUM_ALGORITHM_SYSV: {
            struct cksum_sysv_state state = {
                .sum = 0,
            };
            if (!cksum_read_stream(stream, &state, cksum_sysv_update, &out_result->size)) {
                return false;
            }
            out_result->value = cksum_sysv_finalize(state.sum);
            return true;
        }
        case CKSUM_ALGORITHM_BSD: {
            struct cksum_bsd_state state = {
                .sum = 0,
            };
            if (!cksum_read_stream(stream, &state, cksum_bsd_update, &out_result->size)) {
                return false;
            }
            out_result->value = state.sum;
            return true;
        }
        case CKSUM_ALGORITHM_MD5: {
            struct cksum_md5_state state;
            bx_md5_init(&state.ctx);
            if (!cksum_read_stream(stream, &state, cksum_md5_update, &out_result->size)) {
                return false;
            }
            bx_md5_final(&state.ctx, out_result->md5);
            return true;
        }
        case CKSUM_ALGORITHM_SHA1: {
            struct cksum_sha1_state state;
            bx_sha1_init(&state.ctx);
            if (!cksum_read_stream(stream, &state, cksum_sha1_update, &out_result->size)) {
                return false;
            }
            bx_sha1_final(&state.ctx, out_result->sha1);
            return true;
        }
        case CKSUM_ALGORITHM_SHA224: {
            struct cksum_sha224_state state;
            bx_sha224_init(&state.ctx);
            if (!cksum_read_stream(stream, &state, cksum_sha224_update, &out_result->size)) {
                return false;
            }
            bx_sha224_final(&state.ctx, out_result->sha224);
            return true;
        }
        case CKSUM_ALGORITHM_SHA256: {
            struct cksum_sha256_state state;
            bx_sha256_init(&state.ctx);
            if (!cksum_read_stream(stream, &state, cksum_sha256_update, &out_result->size)) {
                return false;
            }
            bx_sha256_final(&state.ctx, out_result->sha256);
            return true;
        }
        case CKSUM_ALGORITHM_SHA384: {
            struct cksum_sha384_state state;
            bx_sha384_init(&state.ctx);
            if (!cksum_read_stream(stream, &state, cksum_sha384_update, &out_result->size)) {
                return false;
            }
            bx_sha384_final(&state.ctx, out_result->sha384);
            return true;
        }
        case CKSUM_ALGORITHM_SHA512: {
            struct cksum_sha512_state state;
            bx_sha512_init(&state.ctx);
            if (!cksum_read_stream(stream, &state, cksum_sha512_update, &out_result->size)) {
                return false;
            }
            bx_sha512_final(&state.ctx, out_result->sha512);
            return true;
        }
    }

    errno = EINVAL;
    return false;
}

static bool cksum_open_input(const struct cksum_options* options, const char* path, FILE** out_stream, bool* out_is_stdin) {
    if (strcmp(path, "-") == 0) {
        *out_stream = stdin;
        *out_is_stdin = true;
        return true;
    }

    FILE* stream = fopen(path, "rb");
    if (stream == NULL) {
        fprintf(stderr, "%s: %s: %s\n", options->progname, path, strerror(errno));
        return false;
    }

    *out_stream = stream;
    *out_is_stdin = false;
    return true;
}

static void cksum_maybe_print_debug(const struct cksum_options* options) {
    static bool debug_printed = false;

    if (!options->debug || debug_printed) {
        return;
    }

    switch (options->algorithm) {
        case CKSUM_ALGORITHM_CRC:
            fprintf(stderr, "%s: using software crc implementation\n", options->progname);
            debug_printed = true;
            return;
        case CKSUM_ALGORITHM_CRC32B:
            fprintf(stderr, "%s: using software crc32b implementation\n", options->progname);
            debug_printed = true;
            return;
        case CKSUM_ALGORITHM_MD5:
        case CKSUM_ALGORITHM_SHA1:
        case CKSUM_ALGORITHM_SHA224:
        case CKSUM_ALGORITHM_SHA256:
        case CKSUM_ALGORITHM_SHA384:
        case CKSUM_ALGORITHM_SHA512:
        case CKSUM_ALGORITHM_SYSV:
        case CKSUM_ALGORITHM_BSD:
            return;
    }
}

static bool cksum_process_path(const struct cksum_options* options,
                               const char* path,
                               bool show_name_for_numeric_algorithms,
                               struct bx_line_writer* writer) {
    FILE* stream = NULL;
    bool is_stdin = false;
    struct cksum_result result;

    if (!cksum_open_input(options, path, &stream, &is_stdin)) {
        return false;
    }

    cksum_maybe_print_debug(options);

    bool ok = cksum_compute_result(stream, options->algorithm, &result);
    int saved_errno = errno;

    if (!is_stdin) {
        fclose(stream);
    }

    errno = saved_errno;
    if (!ok) {
        fprintf(stderr, "%s: %s: %s\n", options->progname, path, strerror(errno));
        return false;
    }

    if (options->raw_output) {
        uint8_t raw_buffer[4];
        const uint8_t* raw_data = NULL;
        size_t raw_len = 0u;

        switch (result.algorithm) {
            case CKSUM_ALGORITHM_CRC:
            case CKSUM_ALGORITHM_CRC32B:
                raw_buffer[0] = (uint8_t)((result.value >> 24) & 0xffu);
                raw_buffer[1] = (uint8_t)((result.value >> 16) & 0xffu);
                raw_buffer[2] = (uint8_t)((result.value >> 8) & 0xffu);
                raw_buffer[3] = (uint8_t)(result.value & 0xffu);
                raw_data = raw_buffer;
                raw_len = sizeof(raw_buffer);
                break;
            case CKSUM_ALGORITHM_SYSV:
            case CKSUM_ALGORITHM_BSD:
                raw_buffer[0] = (uint8_t)((result.value >> 8) & 0xffu);
                raw_buffer[1] = (uint8_t)(result.value & 0xffu);
                raw_data = raw_buffer;
                raw_len = 2u;
                break;
            case CKSUM_ALGORITHM_MD5:
                raw_data = result.md5;
                raw_len = BX_MD5_DIGEST_SIZE;
                break;
            case CKSUM_ALGORITHM_SHA1:
                raw_data = result.sha1;
                raw_len = BX_SHA1_DIGEST_SIZE;
                break;
            case CKSUM_ALGORITHM_SHA224:
                raw_data = result.sha224;
                raw_len = BX_SHA224_DIGEST_SIZE;
                break;
            case CKSUM_ALGORITHM_SHA256:
                raw_data = result.sha256;
                raw_len = BX_SHA256_DIGEST_SIZE;
                break;
            case CKSUM_ALGORITHM_SHA384:
                raw_data = result.sha384;
                raw_len = BX_SHA384_DIGEST_SIZE;
                break;
            case CKSUM_ALGORITHM_SHA512:
                raw_data = result.sha512;
                raw_len = BX_SHA512_DIGEST_SIZE;
                break;
        }

        return raw_len == 0u || bx_line_writer_write(writer, raw_data, raw_len);
    }

    char terminator = options->zero_terminated ? '\0' : '\n';
    switch (result.algorithm) {
        case CKSUM_ALGORITHM_CRC:
        case CKSUM_ALGORITHM_CRC32B:
            return cksum_write_numeric_result(writer, result.value, result.size, path, show_name_for_numeric_algorithms, false, terminator);
        case CKSUM_ALGORITHM_SYSV: {
            uintmax_t blocks = 0;
            (void)bx_size_block_count_ceil(result.size, 512u, &blocks);
            return cksum_write_numeric_result(writer, result.value, blocks, path, show_name_for_numeric_algorithms, false, terminator);
        }
        case CKSUM_ALGORITHM_BSD: {
            uintmax_t blocks = 0;
            (void)bx_size_block_count_ceil(result.size, 1024u, &blocks);
            return cksum_write_numeric_result(writer, result.value, blocks, path, show_name_for_numeric_algorithms, true, terminator);
        }
        case CKSUM_ALGORITHM_MD5: {
            bool tagged = options->output_mode != CKSUM_OUTPUT_UNTAGGED;
            char digest_text[(BX_MD5_DIGEST_SIZE * 2u) + 1u];
            if (options->base64_output) {
                cksum_base64_encode(result.md5, BX_MD5_DIGEST_SIZE, digest_text);
            }
            else {
                bx_hex_encode_lower(result.md5, BX_MD5_DIGEST_SIZE, digest_text);
            }

            return cksum_write_digest_result(writer, "MD5", digest_text, path, tagged, terminator);
        }
        case CKSUM_ALGORITHM_SHA1: {
            bool tagged = options->output_mode != CKSUM_OUTPUT_UNTAGGED;
            char digest_text[(BX_SHA1_DIGEST_SIZE * 2u) + 1u];
            if (options->base64_output) {
                cksum_base64_encode(result.sha1, BX_SHA1_DIGEST_SIZE, digest_text);
            }
            else {
                bx_hex_encode_lower(result.sha1, BX_SHA1_DIGEST_SIZE, digest_text);
            }

            return cksum_write_digest_result(writer, "SHA1", digest_text, path, tagged, terminator);
        }
        case CKSUM_ALGORITHM_SHA224: {
            bool tagged = options->output_mode != CKSUM_OUTPUT_UNTAGGED;
            char digest_text[(BX_SHA224_DIGEST_SIZE * 2u) + 1u];
            if (options->base64_output) {
                cksum_base64_encode(result.sha224, BX_SHA224_DIGEST_SIZE, digest_text);
            }
            else {
                bx_hex_encode_lower(result.sha224, BX_SHA224_DIGEST_SIZE, digest_text);
            }

            return cksum_write_digest_result(writer, "SHA224", digest_text, path, tagged, terminator);
        }
        case CKSUM_ALGORITHM_SHA256: {
            bool tagged = options->output_mode != CKSUM_OUTPUT_UNTAGGED;
            char digest_text[(BX_SHA256_DIGEST_SIZE * 2u) + 1u];
            if (options->base64_output) {
                cksum_base64_encode(result.sha256, BX_SHA256_DIGEST_SIZE, digest_text);
            }
            else {
                bx_hex_encode_lower(result.sha256, BX_SHA256_DIGEST_SIZE, digest_text);
            }

            return cksum_write_digest_result(writer, "SHA256", digest_text, path, tagged, terminator);
        }
        case CKSUM_ALGORITHM_SHA384: {
            bool tagged = options->output_mode != CKSUM_OUTPUT_UNTAGGED;
            char digest_text[(BX_SHA384_DIGEST_SIZE * 2u) + 1u];
            if (options->base64_output) {
                cksum_base64_encode(result.sha384, BX_SHA384_DIGEST_SIZE, digest_text);
            }
            else {
                bx_hex_encode_lower(result.sha384, BX_SHA384_DIGEST_SIZE, digest_text);
            }

            return cksum_write_digest_result(writer, "SHA384", digest_text, path, tagged, terminator);
        }
        case CKSUM_ALGORITHM_SHA512: {
            bool tagged = options->output_mode != CKSUM_OUTPUT_UNTAGGED;
            char digest_text[(BX_SHA512_DIGEST_SIZE * 2u) + 1u];
            if (options->base64_output) {
                cksum_base64_encode(result.sha512, BX_SHA512_DIGEST_SIZE, digest_text);
            }
            else {
                bx_hex_encode_lower(result.sha512, BX_SHA512_DIGEST_SIZE, digest_text);
            }

            return cksum_write_digest_result(writer, "SHA512", digest_text, path, tagged, terminator);
        }
    }

    errno = EINVAL;
    return false;
}

static const char* cksum_find_check_only_option(const struct cksum_options* options) {
    if (options->ignore_missing) {
        return "--ignore-missing";
    }
    if (options->quiet) {
        return "--quiet";
    }
    if (options->status) {
        return "--status";
    }
    if (options->strict) {
        return "--strict";
    }
    if (options->warn) {
        return "--warn";
    }
    return NULL;
}

int bx_cksum_main(int argc, char** argv) {
    struct cksum_options options;

    if (!cksum_parse_options(argc, argv, &options)) {
        cksum_print_help(stderr, cksum_progname(argv[0]));
        return CKSUM_EXIT_USAGE;
    }

    if (options.length_invalid) {
        fprintf(stderr, "%s: invalid length: '%s'\n", options.progname, options.invalid_length_text ? options.invalid_length_text : "");
        return CKSUM_EXIT_FAIL;
    }

    if (options.show_help) {
        cksum_print_help(stdout, options.progname);
        return CKSUM_EXIT_OK;
    }

    if (options.show_version) {
        cksum_print_version(options.progname);
        return CKSUM_EXIT_OK;
    }

    if (options.length_specified && options.length_bits != 0u) {
        fprintf(stderr, "%s: --length is only supported with --algorithm=blake2b\n", options.progname);
        return CKSUM_EXIT_FAIL;
    }

    if (!options.check_mode) {
        const char* check_only = cksum_find_check_only_option(&options);
        if (check_only != NULL) {
            fprintf(stderr, "%s: the %s option is meaningful only when verifying checksums\n", options.progname, check_only);
            fprintf(stderr, "Try '%s --help' for more information.\n", options.progname);
            return CKSUM_EXIT_FAIL;
        }
    }

    if (!options.check_mode && options.raw_output && (argc - options.first_operand) > 1) {
        fprintf(stderr, "%s: the --raw option is not supported with multiple files\n", options.progname);
        return CKSUM_EXIT_FAIL;
    }

    if (options.base64_output && options.raw_output) {
        fprintf(stderr, "%s: --base64 and --raw are mutually exclusive\n", options.progname);
        fprintf(stderr, "Try '%s --help' for more information.\n", options.progname);
        return CKSUM_EXIT_FAIL;
    }

    if (options.check_mode) {
        char output_buffer[8192];
        struct bx_line_writer writer;
        bx_line_writer_init(&writer, STDOUT_FILENO, output_buffer, sizeof(output_buffer));

        int rc = CKSUM_EXIT_OK;
        if (options.first_operand >= argc) {
            rc = cksum_check_file(&options, "-", &writer);
        }
        else {
            for (int i = options.first_operand; i < argc; i++) {
                if (cksum_check_file(&options, argv[i], &writer) != CKSUM_EXIT_OK) {
                    rc = CKSUM_EXIT_FAIL;
                    if (bx_line_writer_error(&writer) != 0) {
                        break;
                    }
                }
            }
        }

        if (!bx_line_writer_flush(&writer)) {
            return CKSUM_EXIT_FAIL;
        }
        return rc;
    }

    char output_buffer[8192];
    struct bx_line_writer writer;
    bx_line_writer_init(&writer, STDOUT_FILENO, output_buffer, sizeof(output_buffer));

    int rc = CKSUM_EXIT_OK;

    if (options.first_operand >= argc) {
        if (!cksum_process_path(&options, "-", false, &writer)) {
            rc = CKSUM_EXIT_FAIL;
        }
    }
    else {
        for (int i = options.first_operand; i < argc; i++) {
            if (!cksum_process_path(&options, argv[i], true, &writer)) {
                rc = CKSUM_EXIT_FAIL;
                if (bx_line_writer_error(&writer) != 0) {
                    break;
                }
            }
        }
    }

    if (!bx_line_writer_flush(&writer)) {
        return CKSUM_EXIT_FAIL;
    }
    return rc;
}
