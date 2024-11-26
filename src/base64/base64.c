#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "applets.h"

enum {
    BASE64_EXIT_OK = 0,
    BASE64_EXIT_FAIL = 1,
    BASE64_EXIT_USAGE = 2,
};

struct base64_options {
    const char* progname;
    bool decode;
    bool ignore_garbage;
    size_t wrap_cols;
    bool show_help;
    bool show_version;
    const char* input_path;
};

struct base64_encode_state {
    size_t wrap_cols;
    size_t line_len;
    uint8_t tail[2];
    size_t tail_len;
    bool wrote_output;
};

struct base64_decode_state {
    uint8_t quartet[4];
    size_t quartet_len;
    bool finished;
};

enum base64_decode_step {
    BASE64_DECODE_STEP_OK = 0,
    BASE64_DECODE_STEP_INVALID,
    BASE64_DECODE_STEP_IO_ERROR,
};

static const char base64_alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static const char* base64_progname(const char* argv0) {
    return (argv0 && argv0[0] != '\0') ? argv0 : "base64";
}

static void base64_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... [FILE]\n", progname);
    fprintf(stream, "Base64 encode or decode FILE, or standard input, to standard output.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -d, --decode          decode data\n");
    fprintf(stream, "  -i, --ignore-garbage  when decoding, ignore non-alphabet bytes\n");
    fprintf(stream, "  -w, --wrap=COLS       wrap encoded lines after COLS characters (default 76, 0 disables)\n");
    fprintf(stream, "      --help            display this help and exit\n");
    fprintf(stream, "      --version         output version information and exit\n");
}

static void base64_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

static void base64_print_write_error(const char* progname) {
    int saved_errno = errno;
    if (saved_errno == 0) {
        saved_errno = EIO;
    }
    fprintf(stderr, "%s: write error: %s\n", progname, strerror(saved_errno));
}

static bool base64_write_bytes(const uint8_t* data, size_t len, const char* progname) {
    while (len > 0) {
        size_t written = fwrite(data, 1, len, stdout);
        if (written == 0) {
            base64_print_write_error(progname);
            return false;
        }
        data += written;
        len -= written;
    }
    return true;
}

static bool base64_write_char(char c, const char* progname) {
    const uint8_t byte = (uint8_t)c;
    return base64_write_bytes(&byte, 1, progname);
}

static bool base64_parse_wrap_cols(const char* text, size_t* out_cols) {
    if (text == NULL || text[0] == '\0') {
        return false;
    }

    errno = 0;
    char* end = NULL;
    unsigned long long parsed = strtoull(text, &end, 10);
    if (errno != 0 || end == NULL || end[0] != '\0') {
        return false;
    }

    if (parsed > (unsigned long long)SIZE_MAX) {
        return false;
    }

    *out_cols = (size_t)parsed;
    return true;
}

static bool base64_parse_options(int argc, char** argv, struct base64_options* options) {
    static const struct option long_options[] = {
        {"decode", no_argument, NULL, 'd'}, {"ignore-garbage", no_argument, NULL, 'i'}, {"wrap", required_argument, NULL, 'w'},
        {"help", no_argument, NULL, 1},     {"version", no_argument, NULL, 2},          {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = base64_progname(argv[0]);
    options->wrap_cols = 76;
    options->input_path = "-";

    opterr = 0;
    optind = 1;

    while (true) {
        int option_index = 0;
        int c = getopt_long(argc, argv, "+:diw:", long_options, &option_index);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 'd':
                options->decode = true;
                break;
            case 'i':
                options->ignore_garbage = true;
                break;
            case 'w':
                if (!base64_parse_wrap_cols(optarg, &options->wrap_cols)) {
                    fprintf(stderr, "%s: invalid wrap size: '%s'\n", options->progname, optarg ? optarg : "");
                    return false;
                }
                break;
            case 1:
                options->show_help = true;
                break;
            case 2:
                options->show_version = true;
                break;
            case ':':
                fprintf(stderr, "%s: option requires an argument -- '%c'\n", options->progname, optopt);
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

    if (optind < argc) {
        options->input_path = argv[optind++];
    }

    if (optind < argc) {
        fprintf(stderr, "%s: extra operand '%s'\n", options->progname, argv[optind]);
        return false;
    }

    return true;
}

static bool base64_emit_encoded_char(struct base64_encode_state* state, char ch, const char* progname) {
    if (state->wrap_cols != 0 && state->line_len == state->wrap_cols) {
        if (!base64_write_char('\n', progname)) {
            return false;
        }
        state->line_len = 0;
    }

    if (!base64_write_char(ch, progname)) {
        return false;
    }

    state->line_len++;
    state->wrote_output = true;
    return true;
}

static bool base64_emit_triplet(struct base64_encode_state* state, uint8_t a, uint8_t b, uint8_t c, const char* progname) {
    const char out[4] = {
        base64_alphabet[a >> 2],
        base64_alphabet[((a & 0x03u) << 4) | (b >> 4)],
        base64_alphabet[((b & 0x0fu) << 2) | (c >> 6)],
        base64_alphabet[c & 0x3fu],
    };

    for (size_t i = 0; i < 4; i++) {
        if (!base64_emit_encoded_char(state, out[i], progname)) {
            return false;
        }
    }
    return true;
}

static bool base64_encode_update(struct base64_encode_state* state, const uint8_t* data, size_t len, const char* progname) {
    size_t i = 0;

    if (state->tail_len != 0) {
        while (state->tail_len < 3 && i < len) {
            state->tail[state->tail_len++] = data[i++];
        }
        if (state->tail_len == 3) {
            if (!base64_emit_triplet(state, state->tail[0], state->tail[1], state->tail[2], progname)) {
                return false;
            }
            state->tail_len = 0;
        }
    }

    while (i + 3 <= len) {
        if (!base64_emit_triplet(state, data[i], data[i + 1], data[i + 2], progname)) {
            return false;
        }
        i += 3;
    }

    while (i < len) {
        state->tail[state->tail_len++] = data[i++];
    }

    return true;
}

static bool base64_encode_finish(struct base64_encode_state* state, const char* progname) {
    if (state->tail_len == 1) {
        uint8_t a = state->tail[0];
        const char out[4] = {
            base64_alphabet[a >> 2],
            base64_alphabet[(a & 0x03u) << 4],
            '=',
            '=',
        };
        for (size_t i = 0; i < 4; i++) {
            if (!base64_emit_encoded_char(state, out[i], progname)) {
                return false;
            }
        }
    }
    else if (state->tail_len == 2) {
        uint8_t a = state->tail[0];
        uint8_t b = state->tail[1];
        const char out[4] = {
            base64_alphabet[a >> 2],
            base64_alphabet[((a & 0x03u) << 4) | (b >> 4)],
            base64_alphabet[(b & 0x0fu) << 2],
            '=',
        };
        for (size_t i = 0; i < 4; i++) {
            if (!base64_emit_encoded_char(state, out[i], progname)) {
                return false;
            }
        }
    }

    if (state->wrote_output) {
        if (!base64_write_char('\n', progname)) {
            return false;
        }
    }

    return true;
}

static bool base64_is_decode_whitespace(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static int base64_decode_value(unsigned char c) {
    if (c >= 'A' && c <= 'Z') {
        return (int)(c - 'A');
    }
    if (c >= 'a' && c <= 'z') {
        return (int)(c - 'a') + 26;
    }
    if (c >= '0' && c <= '9') {
        return (int)(c - '0') + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

static enum base64_decode_step base64_decode_emit_quartet(struct base64_decode_state* state, const char* progname) {
    const uint8_t* q = state->quartet;
    uint8_t out[3];
    size_t out_len = 0;

    if (q[0] == 64 || q[1] == 64) {
        return BASE64_DECODE_STEP_INVALID;
    }

    out[out_len++] = (uint8_t)((q[0] << 2) | (q[1] >> 4));

    if (q[2] == 64) {
        if (q[3] != 64) {
            return BASE64_DECODE_STEP_INVALID;
        }
        state->finished = true;
    }
    else {
        out[out_len++] = (uint8_t)(((q[1] & 0x0fu) << 4) | (q[2] >> 2));

        if (q[3] == 64) {
            state->finished = true;
        }
        else {
            out[out_len++] = (uint8_t)(((q[2] & 0x03u) << 6) | q[3]);
        }
    }

    state->quartet_len = 0;
    if (!base64_write_bytes(out, out_len, progname)) {
        return BASE64_DECODE_STEP_IO_ERROR;
    }

    return BASE64_DECODE_STEP_OK;
}

static enum base64_decode_step base64_decode_update_byte(struct base64_decode_state* state, unsigned char ch, const struct base64_options* options) {
    if (base64_is_decode_whitespace(ch)) {
        return BASE64_DECODE_STEP_OK;
    }

    if (state->finished) {
        int val = base64_decode_value(ch);
        if (val >= 0 || ch == '=') {
            return BASE64_DECODE_STEP_INVALID;
        }
        return options->ignore_garbage ? BASE64_DECODE_STEP_OK : BASE64_DECODE_STEP_INVALID;
    }

    if (ch == '=') {
        if (state->quartet_len < 2) {
            return BASE64_DECODE_STEP_INVALID;
        }
        state->quartet[state->quartet_len++] = 64;
    }
    else {
        int val = base64_decode_value(ch);
        if (val < 0) {
            return options->ignore_garbage ? BASE64_DECODE_STEP_OK : BASE64_DECODE_STEP_INVALID;
        }

        if (state->quartet_len > 0 && state->quartet[state->quartet_len - 1] == 64) {
            return BASE64_DECODE_STEP_INVALID;
        }

        state->quartet[state->quartet_len++] = (uint8_t)val;
    }

    if (state->quartet_len == 4) {
        return base64_decode_emit_quartet(state, options->progname);
    }

    return BASE64_DECODE_STEP_OK;
}

static bool base64_encode_stream(FILE* stream, const char* source_name, const struct base64_options* options) {
    uint8_t buf[8192];
    struct base64_encode_state state = {
        .wrap_cols = options->wrap_cols,
        .line_len = 0,
        .tail = {0, 0},
        .tail_len = 0,
        .wrote_output = false,
    };

    while (true) {
        size_t nread = fread(buf, 1, sizeof(buf), stream);
        if (nread == 0) {
            break;
        }
        if (!base64_encode_update(&state, buf, nread, options->progname)) {
            return false;
        }
    }

    if (ferror(stream)) {
        fprintf(stderr, "%s: %s: read error\n", options->progname, source_name);
        return false;
    }

    return base64_encode_finish(&state, options->progname);
}

static bool base64_decode_stream(FILE* stream, const char* source_name, const struct base64_options* options) {
    unsigned char buf[8192];
    struct base64_decode_state state = {
        .quartet = {0, 0, 0, 0},
        .quartet_len = 0,
        .finished = false,
    };

    while (true) {
        size_t nread = fread(buf, 1, sizeof(buf), stream);
        if (nread == 0) {
            break;
        }

        for (size_t i = 0; i < nread; i++) {
            enum base64_decode_step step = base64_decode_update_byte(&state, buf[i], options);
            if (step == BASE64_DECODE_STEP_OK) {
                continue;
            }
            if (step == BASE64_DECODE_STEP_INVALID) {
                fprintf(stderr, "%s: invalid input\n", options->progname);
                return false;
            }
            return false;
        }
    }

    if (ferror(stream)) {
        fprintf(stderr, "%s: %s: read error\n", options->progname, source_name);
        return false;
    }

    if (state.quartet_len != 0) {
        fprintf(stderr, "%s: invalid input\n", options->progname);
        return false;
    }

    return true;
}

static int base64_run_stream(FILE* stream, const char* source_name, const struct base64_options* options) {
    bool ok = options->decode ? base64_decode_stream(stream, source_name, options) : base64_encode_stream(stream, source_name, options);

    return ok ? BASE64_EXIT_OK : BASE64_EXIT_FAIL;
}

static int base64_run_file(const struct base64_options* options) {
    if (strcmp(options->input_path, "-") == 0) {
        return base64_run_stream(stdin, "-", options);
    }

    FILE* stream = fopen(options->input_path, "rb");
    if (!stream) {
        fprintf(stderr, "%s: %s: %s\n", options->progname, options->input_path, strerror(errno));
        return BASE64_EXIT_FAIL;
    }

    int rc = base64_run_stream(stream, options->input_path, options);
    if (fclose(stream) != 0 && rc == BASE64_EXIT_OK) {
        fprintf(stderr, "%s: %s: read error\n", options->progname, options->input_path);
        rc = BASE64_EXIT_FAIL;
    }

    return rc;
}

int bx_base64_main(int argc, char** argv) {
    struct base64_options options;
    if (!base64_parse_options(argc, argv, &options)) {
        base64_print_help(stderr, base64_progname(argv[0]));
        return BASE64_EXIT_USAGE;
    }

    if (options.show_help) {
        base64_print_help(stdout, options.progname);
        return BASE64_EXIT_OK;
    }

    if (options.show_version) {
        base64_print_version(options.progname);
        return BASE64_EXIT_OK;
    }

    int rc = base64_run_file(&options);
    if (rc != BASE64_EXIT_OK) {
        return rc;
    }

    if (fflush(stdout) != 0) {
        base64_print_write_error(options.progname);
        return BASE64_EXIT_FAIL;
    }

    return BASE64_EXIT_OK;
}
