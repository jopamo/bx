#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    CKSUM_OPT_TAG,
    CKSUM_OPT_UNTAGGED,
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
    bool show_help;
    bool show_version;
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

static const char* cksum_progname(const char* argv0) {
    return (argv0 && argv0[0] != '\0') ? argv0 : "cksum";
}

static void cksum_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... [FILE]...\n", progname);
    fprintf(stream, "Print or check checksums and digest values.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -a, --algorithm=TYPE   select algorithm: bsd, sysv, crc, crc32b, md5\n");
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
        {"tag", no_argument, NULL, CKSUM_OPT_TAG},
        {"untagged", no_argument, NULL, CKSUM_OPT_UNTAGGED},
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
        int c = getopt_long(argc, argv, "+:a:", long_options, NULL);
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
                break;
            }
            case CKSUM_OPT_TAG:
                options->output_mode = CKSUM_OUTPUT_TAGGED;
                break;
            case CKSUM_OPT_UNTAGGED:
                options->output_mode = CKSUM_OUTPUT_UNTAGGED;
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

    switch (result.algorithm) {
        case CKSUM_ALGORITHM_CRC:
        case CKSUM_ALGORITHM_CRC32B:
            if (show_name_for_numeric_algorithms) {
                printf("%" PRIu32 " %" PRIuMAX " %s\n", result.value, result.size, path);
            }
            else {
                printf("%" PRIu32 " %" PRIuMAX "\n", result.value, result.size);
            }
            break;
        case CKSUM_ALGORITHM_SYSV: {
            uintmax_t blocks = (result.size + 511u) / 512u;
            if (show_name_for_numeric_algorithms) {
                printf("%" PRIu32 " %" PRIuMAX " %s\n", result.value, blocks, path);
            }
            else {
                printf("%" PRIu32 " %" PRIuMAX "\n", result.value, blocks);
            }
            break;
        }
        case CKSUM_ALGORITHM_BSD: {
            uintmax_t blocks = (result.size + 1023u) / 1024u;
            if (show_name_for_numeric_algorithms) {
                printf("%05" PRIu32 " %5" PRIuMAX " %s\n", result.value, blocks, path);
            }
            else {
                printf("%05" PRIu32 " %5" PRIuMAX "\n", result.value, blocks);
            }
            break;
        }
        case CKSUM_ALGORITHM_MD5: {
            bool tagged = options->output_mode != CKSUM_OUTPUT_UNTAGGED;
            char digest_hex[BX_MD5_DIGEST_SIZE * 2u + 1u];
            bx_hex_encode_lower(result.md5, BX_MD5_DIGEST_SIZE, digest_hex);

            if (tagged) {
                printf("MD5 (%s) = %s\n", path, digest_hex);
            }
            else {
                printf("%s  %s\n", digest_hex, path);
            }
            break;
        }
    }

    return true;
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
