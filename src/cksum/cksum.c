#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "applets.h"
#include "common/digest_util.h"
#include "common/md5.h"

enum {
    CKSUM_EXIT_OK = 0,
    CKSUM_EXIT_FAIL = 1,
    CKSUM_EXIT_USAGE = 2,
};

enum cksum_algorithm {
    CKSUM_ALGORITHM_CRC = 0,
    CKSUM_ALGORITHM_CRC32B,
    CKSUM_ALGORITHM_MD5,
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
    bool ignore_missing;
    bool quiet;
    bool status;
    bool strict;
    bool warn;
    bool zero_terminated;
    bool raw_output;
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
};

typedef void (*cksum_update_fn)(void* state, const uint8_t* data, size_t len);

struct cksum_crc_state {
    uint32_t crc;
};

struct cksum_crc32b_state {
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

static uint32_t cksum_crc_table[256];
static bool cksum_crc_table_ready;

static uint32_t cksum_crc32b_table[256];
static bool cksum_crc32b_table_ready;

static const char cksum_base64_alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static const char* cksum_progname(const char* argv0) {
    return (argv0 && argv0[0] != '\0') ? argv0 : "cksum";
}

static void cksum_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... [FILE]...\n", progname);
    fprintf(stream, "Print or check checksums and digest values.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -a, --algorithm=TYPE   select algorithm: bsd, sysv, crc, crc32b, md5\n");
    fprintf(stream, "      --base64           emit base64-encoded digests, not hexadecimal\n");
    fprintf(stream, "  -c, --check            read checksums from the FILEs and check them\n");
    fprintf(stream, "      --ignore-missing   don't fail or report status for missing files\n");
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
    if (strcmp(text, "sysv") == 0) {
        *out_algorithm = CKSUM_ALGORITHM_SYSV;
        return CKSUM_ALGORITHM_PARSE_OK;
    }
    if (strcmp(text, "bsd") == 0) {
        *out_algorithm = CKSUM_ALGORITHM_BSD;
        return CKSUM_ALGORITHM_PARSE_OK;
    }

    if (strcmp(text, "sha1") == 0 || strcmp(text, "sha224") == 0 || strcmp(text, "sha256") == 0 || strcmp(text, "sha384") == 0 || strcmp(text, "sha512") == 0 || strcmp(text, "blake2b") == 0 ||
        strcmp(text, "sm3") == 0) {
        return CKSUM_ALGORITHM_PARSE_UNSUPPORTED;
    }

    return CKSUM_ALGORITHM_PARSE_INVALID;
}

static bool cksum_parse_options(int argc, char** argv, struct cksum_options* options) {
    static const struct option long_options[] = {
        {"algorithm", required_argument, NULL, CKSUM_OPT_ALGORITHM},
        {"base64", no_argument, NULL, CKSUM_OPT_BASE64},
        {"check", no_argument, NULL, CKSUM_OPT_CHECK},
        {"ignore-missing", no_argument, NULL, CKSUM_OPT_IGNORE_MISSING},
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

    opterr = 0;
    optind = 1;

    while (true) {
        int c = getopt_long(argc, argv, "+:a:cwz", long_options, NULL);
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
            case CKSUM_OPT_IGNORE_MISSING:
                options->ignore_missing = true;
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

static void cksum_crc32b_init_table(void) {
    if (cksum_crc32b_table_ready) {
        return;
    }

    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int bit = 0; bit < 8; bit++) {
            if ((crc & 1u) != 0u) {
                crc = (crc >> 1) ^ 0xEDB88320u;
            }
            else {
                crc >>= 1;
            }
        }
        cksum_crc32b_table[i] = crc;
    }

    cksum_crc32b_table_ready = true;
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
    struct cksum_crc32b_state* state = (struct cksum_crc32b_state*)opaque;
    uint32_t crc = state->crc;

    for (size_t i = 0; i < len; i++) {
        crc = cksum_crc32b_table[(crc ^ data[i]) & 0xffu] ^ (crc >> 8);
    }

    state->crc = crc;
}

static uint32_t cksum_crc32b_finalize(uint32_t crc) {
    return crc ^ UINT32_MAX;
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

static bool cksum_parse_md5_digest_text(const char* text, uint8_t out[BX_MD5_DIGEST_SIZE]) {
    const size_t digest_hex_len = BX_MD5_DIGEST_SIZE * 2u;

    if (strlen(text) == digest_hex_len) {
        for (size_t i = 0; i < BX_MD5_DIGEST_SIZE; i++) {
            int hi = cksum_hex_value((unsigned char)text[i * 2u]);
            int lo = cksum_hex_value((unsigned char)text[i * 2u + 1u]);
            if (hi < 0 || lo < 0) {
                return false;
            }
            out[i] = (uint8_t)((hi << 4) | lo);
        }
        return true;
    }

    return cksum_decode_base64(text, BX_MD5_DIGEST_SIZE, out);
}

static bool cksum_parse_md5_untagged_base64_line(char* line, struct bx_checksum_record* record) {
    const size_t digest_b64_len = cksum_base64_encoded_length(BX_MD5_DIGEST_SIZE);
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
    bool digest_ok = cksum_decode_base64(line, BX_MD5_DIGEST_SIZE, record->digest);
    line[digest_b64_len] = saved_sep;
    if (!digest_ok) {
        return false;
    }

    record->digest_len = BX_MD5_DIGEST_SIZE;
    record->filename = line + digest_b64_len + 2u;
    return record->filename[0] != '\0';
}

static bool cksum_parse_md5_tagged_line(char* line, struct bx_checksum_record* record) {
    static const char prefix[] = "MD5 (";
    const size_t prefix_len = sizeof(prefix) - 1u;

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
    if (!cksum_parse_md5_digest_text(digest_text, record->digest)) {
        return false;
    }

    *close_paren = '\0';
    record->digest_len = BX_MD5_DIGEST_SIZE;
    record->binary_mode = false;
    record->filename = line + prefix_len;
    return record->filename[0] != '\0';
}

static bool cksum_parse_check_record(const struct cksum_options* options, char* line, struct bx_checksum_record* record) {
    if (options->algorithm_specified && options->algorithm == CKSUM_ALGORITHM_MD5) {
        if (bx_parse_check_line(line, BX_MD5_DIGEST_SIZE, record)) {
            return true;
        }
        if (cksum_parse_md5_untagged_base64_line(line, record)) {
            return true;
        }
    }

    return cksum_parse_md5_tagged_line(line, record);
}

static const char* cksum_plural_suffix(size_t count) {
    return (count == 1u) ? "" : "s";
}

static int cksum_verify_stream(FILE* stream, const char* source_name, const struct cksum_options* options) {
    char* line = NULL;
    size_t cap = 0u;
    size_t line_no = 0u;
    size_t parsed_count = 0u;
    size_t malformed_count = 0u;
    size_t success_count = 0u;
    size_t mismatch_count = 0u;
    size_t read_fail_count = 0u;
    bool failed = false;

    while (true) {
        ssize_t nread = getline(&line, &cap, stream);
        if (nread < 0) {
            break;
        }

        line_no++;
        if (nread > 0 && line[nread - 1] == '\n') {
            line[nread - 1] = '\0';
        }

        struct bx_checksum_record expected;
        if (!cksum_parse_check_record(options, line, &expected)) {
            malformed_count++;
            if (options->warn && !options->status) {
                fprintf(stderr, "%s: %s:%zu: improperly formatted checksum line\n", options->progname, source_name, line_no);
            }
            continue;
        }
        parsed_count++;

        uint8_t actual[BX_MD5_DIGEST_SIZE];
        if (cksum_md5_hash_path(expected.filename, actual) != 0) {
            int saved_errno = errno;
            if (options->ignore_missing && saved_errno == ENOENT) {
                continue;
            }

            fprintf(stderr, "%s: %s: %s\n", options->progname, expected.filename, strerror(saved_errno));
            if (!options->status) {
                printf("%s: FAILED open or read\n", expected.filename);
            }
            read_fail_count++;
            failed = true;
            continue;
        }

        if (memcmp(actual, expected.digest, BX_MD5_DIGEST_SIZE) == 0) {
            success_count++;
            if (!options->quiet && !options->status) {
                printf("%s: OK\n", expected.filename);
            }
            continue;
        }

        if (!options->status) {
            printf("%s: FAILED\n", expected.filename);
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

static int cksum_check_file(const struct cksum_options* options, const char* path) {
    if (strcmp(path, "-") == 0) {
        return cksum_verify_stream(stdin, "-", options);
    }

    FILE* stream = fopen(path, "r");
    if (stream == NULL) {
        fprintf(stderr, "%s: %s: %s\n", options->progname, path, strerror(errno));
        return CKSUM_EXIT_FAIL;
    }

    int rc = cksum_verify_stream(stream, path, options);
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
            cksum_crc32b_init_table();
            struct cksum_crc32b_state state = {
                .crc = UINT32_MAX,
            };
            if (!cksum_read_stream(stream, &state, cksum_crc32b_update, &out_result->size)) {
                return false;
            }
            out_result->value = cksum_crc32b_finalize(state.crc);
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

static bool cksum_process_path(const struct cksum_options* options, const char* path, bool show_name_for_numeric_algorithms) {
    FILE* stream = NULL;
    bool is_stdin = false;
    struct cksum_result result;

    if (!cksum_open_input(options, path, &stream, &is_stdin)) {
        return false;
    }

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
        }

        if (raw_len > 0u) {
            (void)fwrite(raw_data, 1, raw_len, stdout);
        }
        return true;
    }

    switch (result.algorithm) {
        case CKSUM_ALGORITHM_CRC:
        case CKSUM_ALGORITHM_CRC32B:
            if (show_name_for_numeric_algorithms) {
                printf("%" PRIu32 " %" PRIuMAX " %s", result.value, result.size, path);
            }
            else {
                printf("%" PRIu32 " %" PRIuMAX, result.value, result.size);
            }
            putchar(options->zero_terminated ? '\0' : '\n');
            break;
        case CKSUM_ALGORITHM_SYSV: {
            uintmax_t blocks = (result.size + 511u) / 512u;
            if (show_name_for_numeric_algorithms) {
                printf("%" PRIu32 " %" PRIuMAX " %s", result.value, blocks, path);
            }
            else {
                printf("%" PRIu32 " %" PRIuMAX, result.value, blocks);
            }
            putchar(options->zero_terminated ? '\0' : '\n');
            break;
        }
        case CKSUM_ALGORITHM_BSD: {
            uintmax_t blocks = (result.size + 1023u) / 1024u;
            if (show_name_for_numeric_algorithms) {
                printf("%05" PRIu32 " %5" PRIuMAX " %s", result.value, blocks, path);
            }
            else {
                printf("%05" PRIu32 " %5" PRIuMAX, result.value, blocks);
            }
            putchar(options->zero_terminated ? '\0' : '\n');
            break;
        }
        case CKSUM_ALGORITHM_MD5: {
            bool tagged = options->output_mode != CKSUM_OUTPUT_UNTAGGED;
            char digest_text[cksum_base64_encoded_length(BX_MD5_DIGEST_SIZE) + 1u];
            if (options->base64_output) {
                cksum_base64_encode(result.md5, BX_MD5_DIGEST_SIZE, digest_text);
            }
            else {
                bx_hex_encode_lower(result.md5, BX_MD5_DIGEST_SIZE, digest_text);
            }

            if (tagged) {
                printf("MD5 (%s) = %s", path, digest_text);
            }
            else {
                printf("%s  %s", digest_text, path);
            }
            putchar(options->zero_terminated ? '\0' : '\n');
            break;
        }
    }

    return true;
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

    if (options.show_help) {
        cksum_print_help(stdout, options.progname);
        return CKSUM_EXIT_OK;
    }

    if (options.show_version) {
        cksum_print_version(options.progname);
        return CKSUM_EXIT_OK;
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

    if (options.check_mode && options.algorithm_specified && options.algorithm != CKSUM_ALGORITHM_MD5) {
        fprintf(stderr, "%s: --check is not supported with --algorithm={bsd,sysv,crc,crc32b}\n", options.progname);
        return CKSUM_EXIT_FAIL;
    }

    if (options.check_mode) {
        if (options.first_operand >= argc) {
            return cksum_check_file(&options, "-");
        }

        int rc = CKSUM_EXIT_OK;
        for (int i = options.first_operand; i < argc; i++) {
            if (cksum_check_file(&options, argv[i]) != CKSUM_EXIT_OK) {
                rc = CKSUM_EXIT_FAIL;
            }
        }
        return rc;
    }

    int rc = CKSUM_EXIT_OK;

    if (options.first_operand >= argc) {
        if (!cksum_process_path(&options, "-", false)) {
            rc = CKSUM_EXIT_FAIL;
        }
    }
    else {
        for (int i = options.first_operand; i < argc; i++) {
            if (!cksum_process_path(&options, argv[i], true)) {
                rc = CKSUM_EXIT_FAIL;
            }
        }
    }

    return rc;
}
