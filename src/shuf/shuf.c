#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "applets.h"
#include "diag.h"
#include "libbx.h"

enum {
    BX_SHUF_OPT_HELP = 256,
    BX_SHUF_OPT_VERSION,
    BX_SHUF_OPT_RANDOM_SOURCE,
};

struct bx_shuf_options {
    const char* progname;
    bool echo_mode;
    bool input_range_specified;
    intmax_t range_lo;
    intmax_t range_hi;
    bool head_count_specified;
    uintmax_t head_count;
    const char* output_path;
    bool repeat;
    bool zero_terminated;
    const char* random_source_path;
    bool random_source_specified;
    bool show_help;
    bool show_version;
};

struct bx_shuf_record {
    char* data;
    size_t len;
};

struct bx_shuf_records {
    struct bx_shuf_record* items;
    size_t len;
    size_t cap;
};

struct bx_shuf_rng {
    int fd;
    const char* source_path;
};

static const char* bx_shuf_progname(const char* argv0) {
    if (argv0 == NULL || argv0[0] == '\0') {
        return "shuf";
    }

    const char* base = strrchr(argv0, '/');
    if (base != NULL && base[1] != '\0') {
        return base + 1;
    }

    return argv0;
}

static void bx_shuf_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... [FILE]\n", progname);
    fprintf(stream, "  or:  %s -e [OPTION]... [ARG]...\n", progname);
    fprintf(stream, "  or:  %s -i LO-HI [OPTION]...\n", progname);
    fprintf(stream, "Write a random permutation of the input lines to standard output.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -e, --echo                 treat each ARG as an input line\n");
    fprintf(stream, "  -i, --input-range=LO-HI    treat each number LO through HI as an input line\n");
    fprintf(stream, "  -n, --head-count=COUNT     output at most COUNT lines\n");
    fprintf(stream, "  -o, --output=FILE          write result to FILE instead of standard output\n");
    fprintf(stream, "      --random-source=FILE   get random bytes from FILE\n");
    fprintf(stream, "  -r, --repeat               output lines can be repeated\n");
    fprintf(stream, "  -z, --zero-terminated      line delimiter is NUL, not newline\n");
    fprintf(stream, "      --help                 display this help and exit\n");
    fprintf(stream, "      --version              output version information and exit\n");
}

static void bx_shuf_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

static bool bx_shuf_parse_uintmax(const char* text, uintmax_t* value_out) {
    if (text == NULL || text[0] == '\0' || value_out == NULL) {
        return false;
    }

    errno = 0;
    char* end = NULL;
    uintmax_t value = strtoumax(text, &end, 10);
    if (errno == ERANGE || end == text || end == NULL || end[0] != '\0') {
        return false;
    }

    *value_out = value;
    return true;
}

static bool bx_shuf_parse_input_range(const char* text, intmax_t* lo_out, intmax_t* hi_out) {
    if (text == NULL || text[0] == '\0' || lo_out == NULL || hi_out == NULL) {
        return false;
    }

    errno = 0;
    char* lo_end = NULL;
    intmax_t lo = strtoimax(text, &lo_end, 10);
    if (errno == ERANGE || lo_end == text || lo_end == NULL || lo_end[0] != '-') {
        return false;
    }

    const char* hi_text = lo_end + 1;
    if (hi_text[0] == '\0') {
        return false;
    }

    errno = 0;
    char* hi_end = NULL;
    intmax_t hi = strtoimax(hi_text, &hi_end, 10);
    if (errno == ERANGE || hi_end == hi_text || hi_end == NULL || hi_end[0] != '\0') {
        return false;
    }

    if (lo > hi) {
        return false;
    }

    *lo_out = lo;
    *hi_out = hi;
    return true;
}

static bool bx_shuf_parse_options(int argc, char** argv, struct bx_shuf_options* options, int* first_operand, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"echo", no_argument, NULL, 'e'},
        {"input-range", required_argument, NULL, 'i'},
        {"head-count", required_argument, NULL, 'n'},
        {"output", required_argument, NULL, 'o'},
        {"random-source", required_argument, NULL, BX_SHUF_OPT_RANDOM_SOURCE},
        {"repeat", no_argument, NULL, 'r'},
        {"zero-terminated", no_argument, NULL, 'z'},
        {"help", no_argument, NULL, BX_SHUF_OPT_HELP},
        {"version", no_argument, NULL, BX_SHUF_OPT_VERSION},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_shuf_progname((argc > 0) ? argv[0] : NULL);
    options->random_source_path = "/dev/urandom";
    diag->progname = options->progname;

    opterr = 0;
    optind = 1;

    while (true) {
        int option_index = 0;
        int c = getopt_long(argc, argv, "+:ei:n:o:rz", long_options, &option_index);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 'e':
                options->echo_mode = true;
                break;
            case 'i':
                if (!bx_shuf_parse_input_range(optarg, &options->range_lo, &options->range_hi)) {
                    bx_diag(diag, "invalid input range: '%s'", optarg ? optarg : "");
                    return false;
                }
                options->input_range_specified = true;
                break;
            case 'n':
                if (!bx_shuf_parse_uintmax(optarg, &options->head_count)) {
                    bx_diag(diag, "invalid line count: '%s'", optarg ? optarg : "");
                    return false;
                }
                options->head_count_specified = true;
                break;
            case 'o':
                options->output_path = optarg;
                break;
            case BX_SHUF_OPT_RANDOM_SOURCE:
                options->random_source_path = optarg;
                options->random_source_specified = true;
                break;
            case 'r':
                options->repeat = true;
                break;
            case 'z':
                options->zero_terminated = true;
                break;
            case BX_SHUF_OPT_HELP:
                options->show_help = true;
                return true;
            case BX_SHUF_OPT_VERSION:
                options->show_version = true;
                return true;
            case ':':
                if (optopt != 0) {
                    bx_diag(diag, "option requires an argument -- '%c'", optopt);
                }
                else if (optind > 0 && optind <= argc && argv[optind - 1] != NULL) {
                    bx_diag(diag, "option requires an argument -- '%s'", argv[optind - 1]);
                }
                else {
                    bx_diag(diag, "option requires an argument");
                }
                return false;
            case '?':
                if (optopt != 0) {
                    bx_diag(diag, "invalid option -- '%c'", optopt);
                }
                else if (optind > 0 && optind <= argc && argv[optind - 1] != NULL) {
                    bx_diag(diag, "unrecognized option '%s'", argv[optind - 1]);
                }
                else {
                    bx_diag(diag, "unrecognized option");
                }
                return false;
            default:
                return false;
        }
    }

    *first_operand = optind;
    return true;
}

static void bx_shuf_records_init(struct bx_shuf_records* records) {
    records->items = NULL;
    records->len = 0;
    records->cap = 0;
}

static void bx_shuf_records_free(struct bx_shuf_records* records) {
    if (records == NULL) {
        return;
    }

    for (size_t i = 0; i < records->len; i++) {
        free(records->items[i].data);
    }
    free(records->items);
    records->items = NULL;
    records->len = 0;
    records->cap = 0;
}

static void bx_shuf_records_push_copy(struct bx_shuf_records* records, const char* data, size_t len) {
    if (records->len == records->cap) {
        size_t new_cap = records->cap == 0 ? 16u : records->cap * 2u;
        records->items = xrealloc(records->items, new_cap * sizeof(*records->items));
        records->cap = new_cap;
    }

    char* copy = xmalloc(len + 1u);
    if (len > 0) {
        memcpy(copy, data, len);
    }
    copy[len] = '\0';

    records->items[records->len].data = copy;
    records->items[records->len].len = len;
    records->len++;
}

static bool bx_shuf_load_echo_input(char** argv, int first_operand, int argc, struct bx_shuf_records* records) {
    for (int i = first_operand; i < argc; i++) {
        bx_shuf_records_push_copy(records, argv[i], strlen(argv[i]));
    }
    return true;
}

static bool bx_shuf_load_range_input(intmax_t lo, intmax_t hi, struct bx_shuf_records* records, struct bx_diag_ctx* diag) {
    uintmax_t span = (uintmax_t)hi - (uintmax_t)lo;
    span += 1u;
    if (span == 0 || span > (uintmax_t)SIZE_MAX) {
        bx_diag(diag, "input range too large");
        return false;
    }

    intmax_t value = lo;
    while (true) {
        char number_buf[64];
        int printed = snprintf(number_buf, sizeof(number_buf), "%" PRIdMAX, value);
        if (printed < 0 || (size_t)printed >= sizeof(number_buf)) {
            bx_diag(diag, "failed to format input range value");
            return false;
        }
        bx_shuf_records_push_copy(records, number_buf, (size_t)printed);

        if (value == hi) {
            break;
        }
        value++;
    }

    return true;
}

static bool bx_shuf_load_stream_input(FILE* stream, int delimiter, struct bx_shuf_records* records, struct bx_diag_ctx* diag) {
    char* line = NULL;
    size_t cap = 0;

    while (true) {
        errno = 0;
        ssize_t nread = getdelim(&line, &cap, delimiter, stream);
        if (nread < 0) {
            if (feof(stream)) {
                break;
            }
            bx_diag(diag, "read error: %s", strerror(errno));
            free(line);
            return false;
        }

        size_t len = (size_t)nread;
        if (len > 0 && line[len - 1u] == delimiter) {
            len--;
        }

        bx_shuf_records_push_copy(records, line, len);
    }

    free(line);
    return true;
}

static bool bx_shuf_load_file_input(const char* path, int delimiter, struct bx_shuf_records* records, struct bx_diag_ctx* diag) {
    FILE* stream = stdin;
    bool close_stream = false;

    if (strcmp(path, "-") != 0) {
        stream = fopen(path, "rb");
        if (stream == NULL) {
            bx_perror_path(diag, path);
            return false;
        }
        close_stream = true;
    }

    bool ok = bx_shuf_load_stream_input(stream, delimiter, records, diag);

    if (close_stream && fclose(stream) != 0) {
        bx_perror_path(diag, path);
        ok = false;
    }

    return ok;
}

static bool bx_shuf_rng_open(struct bx_shuf_rng* rng, const char* source_path, struct bx_diag_ctx* diag) {
    rng->fd = open(source_path, O_RDONLY);
    if (rng->fd < 0) {
        bx_perror_path(diag, source_path);
        return false;
    }

    rng->source_path = source_path;
    return true;
}

static void bx_shuf_rng_close(struct bx_shuf_rng* rng, struct bx_diag_ctx* diag) {
    if (rng->fd < 0) {
        return;
    }

    if (close(rng->fd) != 0) {
        bx_perror_path(diag, rng->source_path ? rng->source_path : "random source");
    }

    rng->fd = -1;
}

static bool bx_shuf_rng_read(struct bx_shuf_rng* rng, void* buffer, size_t length, struct bx_diag_ctx* diag) {
    unsigned char* out = buffer;
    size_t done = 0;

    while (done < length) {
        ssize_t nread = read(rng->fd, out + done, length - done);
        if (nread < 0) {
            if (errno == EINTR) {
                continue;
            }
            bx_perror_path(diag, rng->source_path ? rng->source_path : "random source");
            return false;
        }
        if (nread == 0) {
            bx_diag(diag, "%s: unexpected end of file", rng->source_path ? rng->source_path : "random source");
            return false;
        }
        done += (size_t)nread;
    }

    return true;
}

static bool bx_shuf_rng_u64(struct bx_shuf_rng* rng, uint64_t* value_out, struct bx_diag_ctx* diag) {
    return bx_shuf_rng_read(rng, value_out, sizeof(*value_out), diag);
}

static bool bx_shuf_rng_uniform(struct bx_shuf_rng* rng, size_t upper_bound, size_t* value_out, struct bx_diag_ctx* diag) {
    if (upper_bound == 0 || value_out == NULL) {
        return false;
    }

    if ((uintmax_t)upper_bound > UINT64_MAX) {
        bx_diag(diag, "too many input lines");
        return false;
    }

    uint64_t upper = (uint64_t)upper_bound;
    if (upper == 1) {
        *value_out = 0;
        return true;
    }

    uint64_t threshold = (uint64_t)(-upper) % upper;

    while (true) {
        uint64_t raw = 0;
        if (!bx_shuf_rng_u64(rng, &raw, diag)) {
            return false;
        }

        if (raw >= threshold) {
            *value_out = (size_t)(raw % upper);
            return true;
        }
    }
}

static void bx_shuf_swap_records(struct bx_shuf_record* a, struct bx_shuf_record* b) {
    if (a == b) {
        return;
    }

    struct bx_shuf_record tmp = *a;
    *a = *b;
    *b = tmp;
}

static bool bx_shuf_write_all(FILE* stream, const void* buffer, size_t length, const char* output_name, struct bx_diag_ctx* diag) {
    const unsigned char* p = buffer;
    size_t remaining = length;

    while (remaining > 0) {
        size_t written = fwrite(p, 1, remaining, stream);
        if (written == 0) {
            int saved_errno = errno;
            if (saved_errno == 0) {
                saved_errno = EIO;
            }
            bx_diag(diag, "%s: %s", output_name, strerror(saved_errno));
            return false;
        }

        p += written;
        remaining -= written;
    }

    return true;
}

static bool bx_shuf_emit_record(const struct bx_shuf_record* record, int delimiter, FILE* output_stream, const char* output_name, struct bx_diag_ctx* diag) {
    if (!bx_shuf_write_all(output_stream, record->data, record->len, output_name, diag)) {
        return false;
    }

    unsigned char delim_byte = (unsigned char)delimiter;
    if (!bx_shuf_write_all(output_stream, &delim_byte, 1, output_name, diag)) {
        return false;
    }

    return true;
}

static bool
bx_shuf_emit_without_repeat(struct bx_shuf_records* records, size_t output_count, int delimiter, FILE* output_stream, const char* output_name, struct bx_shuf_rng* rng, struct bx_diag_ctx* diag) {
    for (size_t i = 0; i < output_count; i++) {
        size_t remaining = records->len - i;
        size_t pick_offset = 0;
        if (remaining > 1) {
            if (!bx_shuf_rng_uniform(rng, remaining, &pick_offset, diag)) {
                return false;
            }
        }

        size_t pick_index = i + pick_offset;
        bx_shuf_swap_records(&records->items[i], &records->items[pick_index]);

        if (!bx_shuf_emit_record(&records->items[i], delimiter, output_stream, output_name, diag)) {
            return false;
        }
    }

    return true;
}

static bool bx_shuf_emit_with_repeat(const struct bx_shuf_records* records,
                                     bool bounded_output,
                                     uintmax_t output_count,
                                     int delimiter,
                                     FILE* output_stream,
                                     const char* output_name,
                                     struct bx_shuf_rng* rng,
                                     struct bx_diag_ctx* diag) {
    if (records->len == 0) {
        return bounded_output && output_count == 0;
    }

    uintmax_t produced = 0;
    while (!bounded_output || produced < output_count) {
        size_t pick_index = 0;
        if (records->len > 1) {
            if (!bx_shuf_rng_uniform(rng, records->len, &pick_index, diag)) {
                return false;
            }
        }

        if (!bx_shuf_emit_record(&records->items[pick_index], delimiter, output_stream, output_name, diag)) {
            return false;
        }

        produced++;
    }

    return true;
}

static bool bx_shuf_needs_random(const struct bx_shuf_options* options, const struct bx_shuf_records* records) {
    if (records->len <= 1) {
        return false;
    }

    if (options->repeat) {
        if (options->head_count_specified) {
            return options->head_count > 0;
        }
        return true;
    }

    size_t output_count = records->len;
    if (options->head_count_specified && options->head_count < (uintmax_t)output_count) {
        output_count = (size_t)options->head_count;
    }
    return output_count > 0;
}

int bx_shuf_main(int argc, char** argv) {
    struct bx_shuf_options options;
    struct bx_diag_ctx diag = {
        .progname = "shuf",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };
    int first_operand = 0;

    if (!bx_shuf_parse_options(argc, argv, &options, &first_operand, &diag)) {
        return diag.exit_status != 0 ? diag.exit_status : 1;
    }

    if (options.show_help) {
        bx_shuf_print_help(stdout, options.progname);
        return 0;
    }

    if (options.show_version) {
        bx_shuf_print_version(options.progname);
        return 0;
    }

    if (options.echo_mode && options.input_range_specified) {
        bx_diag(&diag, "cannot combine -e/--echo with -i/--input-range");
        return diag.exit_status;
    }

    int operand_count = argc - first_operand;
    const char* input_path = "-";
    if (options.echo_mode) {
        /* Operands are input lines. */
    }
    else if (options.input_range_specified) {
        if (operand_count > 0) {
            bx_diag(&diag, "extra operand '%s'", argv[first_operand]);
            return diag.exit_status;
        }
    }
    else {
        if (operand_count > 1) {
            bx_diag(&diag, "extra operand '%s'", argv[first_operand + 1]);
            return diag.exit_status;
        }
        if (operand_count == 1) {
            input_path = argv[first_operand];
        }
    }

    int delimiter = options.zero_terminated ? '\0' : '\n';
    struct bx_shuf_records records;
    bx_shuf_records_init(&records);

    struct bx_shuf_rng rng = {
        .fd = -1,
        .source_path = NULL,
    };

    FILE* output_stream = stdout;
    bool close_output_stream = false;

    if (options.input_range_specified) {
        if (!bx_shuf_load_range_input(options.range_lo, options.range_hi, &records, &diag)) {
            goto cleanup;
        }
    }
    else if (options.echo_mode) {
        if (!bx_shuf_load_echo_input(argv, first_operand, argc, &records)) {
            goto cleanup;
        }
    }
    else {
        if (!bx_shuf_load_file_input(input_path, delimiter, &records, &diag)) {
            goto cleanup;
        }
    }

    if (options.repeat && records.len == 0 && (!options.head_count_specified || options.head_count != 0)) {
        bx_diag(&diag, "no lines to repeat");
        goto cleanup;
    }

    if (options.random_source_specified || bx_shuf_needs_random(&options, &records)) {
        if (!bx_shuf_rng_open(&rng, options.random_source_path, &diag)) {
            goto cleanup;
        }
    }

    const char* output_name = options.output_path ? options.output_path : "standard output";
    if (options.output_path != NULL) {
        output_stream = fopen(options.output_path, "wb");
        if (output_stream == NULL) {
            bx_perror_path(&diag, options.output_path);
            goto cleanup;
        }
        close_output_stream = true;
    }

    if (options.repeat) {
        bool bounded_output = options.head_count_specified;
        uintmax_t output_count = options.head_count_specified ? options.head_count : 0;
        if (!bx_shuf_emit_with_repeat(&records, bounded_output, output_count, delimiter, output_stream, output_name, &rng, &diag)) {
            goto cleanup;
        }
    }
    else {
        size_t output_count = records.len;
        if (options.head_count_specified && options.head_count < (uintmax_t)output_count) {
            output_count = (size_t)options.head_count;
        }

        if (!bx_shuf_emit_without_repeat(&records, output_count, delimiter, output_stream, output_name, &rng, &diag)) {
            goto cleanup;
        }
    }

    if (close_output_stream) {
        if (fclose(output_stream) != 0) {
            bx_perror_path(&diag, options.output_path);
        }
        output_stream = NULL;
        close_output_stream = false;
    }
    else if (fflush(output_stream) == EOF) {
        int saved_errno = errno;
        if (saved_errno == 0) {
            saved_errno = EIO;
        }
        bx_diag(&diag, "%s: %s", output_name, strerror(saved_errno));
    }

cleanup:
    if (close_output_stream && output_stream != NULL) {
        (void)fclose(output_stream);
    }
    bx_shuf_rng_close(&rng, &diag);
    bx_shuf_records_free(&records);
    return diag.exit_status;
}
