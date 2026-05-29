#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

#include "fswalk/walk.h"
#include "lib/size_parse.h"
#include "search/dev_counters.h"
#include "search/literal.h"
#include "search/literal_scan.h"

#define BX_LITBENCH_DEFAULT_SAMPLE_FILES 4096u
#define BX_LITBENCH_RANDOM_ASCII_SEED UINT64_C(0x42584c49)
#define BX_LITBENCH_SOURCE_LIKE_SEED UINT64_C(0x4258534c)

struct bx_litbench_file {
    unsigned char* data;
    size_t len;
};

struct bx_litbench_input {
    struct bx_litbench_file* files;
    size_t count;
    size_t cap;
    size_t bytes;
    size_t tree_roots_walked;
    size_t tree_files_seen;
    size_t tree_errors;
    size_t random_ascii_bytes;
    size_t source_like_bytes;
};

struct bx_litbench_args {
    const char* needle;
    const char** files;
    size_t file_count;
    size_t file_cap;
    const char** tree_roots;
    size_t tree_root_count;
    size_t tree_root_cap;
    size_t sample_files;
    size_t random_ascii_bytes;
    size_t source_like_bytes;
    bool have_random_ascii;
    bool have_source_like;
    const char* literal_backend;
    bool print_counters;
    bool have_perf_cycles;
    uint64_t perf_cycles;
    size_t warmup;
    size_t iterations;
};

struct bx_litbench_run {
    size_t matches;
    size_t last_match_file_index;
    size_t last_match_offset;
    bool last_match_found;
    bool have_resolved_backend;
    enum bx_literal_backend resolved_backend;
    double median_seconds;
    double min_seconds;
    double max_seconds;
    double median_bytes_per_sec;
    double min_bytes_per_sec;
    double max_bytes_per_sec;
    bool have_cycles_per_byte;
    double cycles_per_byte;
};

static void bx_litbench_usage(FILE* stream) {
    fprintf(stream,
            "Usage: bx-litbench --needle NEEDLE --file PATH [--file PATH ...] "
            "[--warmup N] [--iterations N]\n"
            "       bx-litbench --needle NEEDLE --tree-sample ROOT [--tree-sample ROOT ...] "
            "[--sample-files N]\n"
            "       bx-litbench --needle NEEDLE --random-ascii SIZE\n"
            "       bx-litbench --needle NEEDLE --source-like SIZE\n"
            "\n"
            "Native literal microbenchmark for bx search mechanics.\n"
            "\n"
            "Modes:\n"
            "  --file PATH       scan one file held in memory\n"
            "  --file PATH ...   repeat --file to scan a list of in-memory files\n"
            "  --tree-sample ROOT\n"
            "                    walk ROOT with the shared walker and scan sampled regular files\n"
            "  --random-ascii SIZE\n"
            "                    generate deterministic printable ASCII and scan it in memory\n"
            "  --source-like SIZE\n"
            "                    generate deterministic source-like text and scan it in memory\n"
            "\n"
            "Options:\n"
            "  --needle NEEDLE   exact literal to scan for\n"
            "  --backend scalar|avx2|neon|sve\n"
            "                    force a literal backend for this process\n"
            "  --sample-files N  tree-sample file cap; 0 means every file (default: %u)\n"
            "  --print-counters\n"
            "                    print bx search dev counters after the run\n"
            "  --perf-cycles N  external cycle count for timed iterations; prints cycles/byte\n"
            "  --warmup N        untimed scans before measurement (default: 0)\n"
            "  --iterations N    timed scans to run (default: 1)\n"
            "  --help            show this help\n",
            BX_LITBENCH_DEFAULT_SAMPLE_FILES);
}

static const char* bx_litbench_option_value(const char* arg, const char* name) {
    size_t len = strlen(name);

    if (strncmp(arg, name, len) != 0 || arg[len] != '=')
        return NULL;
    return arg + len + 1u;
}

static bool bx_litbench_parse_size(const char* value, const char* name, bool allow_zero, size_t* out) {
    char* end = NULL;
    uintmax_t parsed;

    if (!value || !*value || !out) {
        fprintf(stderr, "bx-litbench: %s requires a numeric value\n", name);
        return false;
    }

    errno = 0;
    parsed = strtoumax(value, &end, 10);
    if (errno != 0 || end == value || (end && *end != '\0') || parsed > (uintmax_t)SIZE_MAX || (!allow_zero && parsed == 0u)) {
        fprintf(stderr, "bx-litbench: invalid %s: %s\n", name, value);
        return false;
    }

    *out = (size_t)parsed;
    return true;
}

static bool bx_litbench_parse_byte_size(const char* value, const char* name, size_t* out) {
    intmax_t parsed;

    if (!value || !*value || !out) {
        fprintf(stderr, "bx-litbench: %s requires a byte-size value\n", name);
        return false;
    }
    if (!bx_size_parse_scaled_count(value, &parsed) || parsed <= 0 || (uintmax_t)parsed > (uintmax_t)SIZE_MAX) {
        fprintf(stderr, "bx-litbench: invalid %s: %s\n", name, value);
        return false;
    }

    *out = (size_t)parsed;
    return true;
}

static bool bx_litbench_parse_u64(const char* value, const char* name, uint64_t* out) {
    char* end = NULL;
    uintmax_t parsed;

    if (!value || !*value || !out) {
        fprintf(stderr, "bx-litbench: %s requires a numeric value\n", name);
        return false;
    }

    errno = 0;
    parsed = strtoumax(value, &end, 10);
    if (errno != 0 || end == value || (end && *end != '\0') || parsed > (uintmax_t)UINT64_MAX) {
        fprintf(stderr, "bx-litbench: invalid %s: %s\n", name, value);
        return false;
    }

    *out = (uint64_t)parsed;
    return true;
}

static bool bx_litbench_next_arg(int argc, char** argv, int* index, const char* name, const char** out) {
    if (*index + 1 >= argc) {
        fprintf(stderr, "bx-litbench: %s requires an argument\n", name);
        return false;
    }

    *index += 1;
    *out = argv[*index];
    return true;
}

static bool bx_litbench_add_file_arg(struct bx_litbench_args* args, const char* path) {
    const char** new_files;
    size_t new_cap;

    if (!args || !path)
        return false;
    if (args->file_count == args->file_cap) {
        new_cap = args->file_cap == 0u ? 4u : args->file_cap * 2u;
        if (new_cap < args->file_cap || new_cap > SIZE_MAX / sizeof(*args->files)) {
            fprintf(stderr, "bx-litbench: too many --file operands\n");
            return false;
        }
        new_files = realloc(args->files, new_cap * sizeof(*args->files));
        if (!new_files) {
            fprintf(stderr, "bx-litbench: failed to allocate file operand storage\n");
            return false;
        }
        args->files = new_files;
        args->file_cap = new_cap;
    }
    args->files[args->file_count++] = path;
    return true;
}

static bool bx_litbench_add_tree_root_arg(struct bx_litbench_args* args, const char* root) {
    const char** new_roots;
    size_t new_cap;

    if (!args || !root)
        return false;
    if (args->tree_root_count == args->tree_root_cap) {
        new_cap = args->tree_root_cap == 0u ? 2u : args->tree_root_cap * 2u;
        if (new_cap < args->tree_root_cap || new_cap > SIZE_MAX / sizeof(*args->tree_roots)) {
            fprintf(stderr, "bx-litbench: too many --tree-sample roots\n");
            return false;
        }
        new_roots = realloc(args->tree_roots, new_cap * sizeof(*args->tree_roots));
        if (!new_roots) {
            fprintf(stderr, "bx-litbench: failed to allocate tree-sample root storage\n");
            return false;
        }
        args->tree_roots = new_roots;
        args->tree_root_cap = new_cap;
    }
    args->tree_roots[args->tree_root_count++] = root;
    return true;
}

static bool bx_litbench_parse_backend(const char* value, const char** backend_out) {
    if (!value || !*value || !backend_out) {
        fprintf(stderr, "bx-litbench: --backend requires an argument\n");
        return false;
    }
    if (strcmp(value, "scalar") != 0 && strcmp(value, "avx2") != 0 && strcmp(value, "neon") != 0 && strcmp(value, "sve") != 0) {
        fprintf(stderr, "bx-litbench: unsupported --backend: %s (currently supported: scalar, avx2, neon, sve)\n", value);
        return false;
    }
    *backend_out = value;
    return true;
}

static int bx_litbench_parse_args(int argc, char** argv, struct bx_litbench_args* args) {
    if (!args)
        return 2;

    *args = (struct bx_litbench_args){
        .needle = NULL,
        .files = NULL,
        .file_count = 0u,
        .file_cap = 0u,
        .tree_roots = NULL,
        .tree_root_count = 0u,
        .tree_root_cap = 0u,
        .sample_files = BX_LITBENCH_DEFAULT_SAMPLE_FILES,
        .random_ascii_bytes = 0u,
        .source_like_bytes = 0u,
        .have_random_ascii = false,
        .have_source_like = false,
        .literal_backend = NULL,
        .print_counters = false,
        .have_perf_cycles = false,
        .perf_cycles = 0u,
        .warmup = 0u,
        .iterations = 1u,
    };

    for (int i = 1; i < argc; ++i) {
        const char* value;

        if (strcmp(argv[i], "--help") == 0) {
            bx_litbench_usage(stdout);
            return 1;
        }
        if (strcmp(argv[i], "--print-counters") == 0) {
            args->print_counters = true;
            continue;
        }

        value = bx_litbench_option_value(argv[i], "--perf-cycles");
        if (value) {
            if (!bx_litbench_parse_u64(value, "--perf-cycles", &args->perf_cycles))
                return 2;
            args->have_perf_cycles = true;
            continue;
        }
        value = bx_litbench_option_value(argv[i], "--external-cycles");
        if (value) {
            if (!bx_litbench_parse_u64(value, "--external-cycles", &args->perf_cycles))
                return 2;
            args->have_perf_cycles = true;
            continue;
        }
        if (strcmp(argv[i], "--perf-cycles") == 0 || strcmp(argv[i], "--external-cycles") == 0) {
            const char* cycles = NULL;
            const char* option_name = argv[i];

            if (!bx_litbench_next_arg(argc, argv, &i, option_name, &cycles))
                return 2;
            if (!bx_litbench_parse_u64(cycles, option_name, &args->perf_cycles))
                return 2;
            args->have_perf_cycles = true;
            continue;
        }

        value = bx_litbench_option_value(argv[i], "--needle");
        if (value) {
            args->needle = value;
            continue;
        }
        if (strcmp(argv[i], "--needle") == 0) {
            if (!bx_litbench_next_arg(argc, argv, &i, "--needle", &args->needle))
                return 2;
            continue;
        }

        value = bx_litbench_option_value(argv[i], "--backend");
        if (value) {
            if (!bx_litbench_parse_backend(value, &args->literal_backend))
                return 2;
            continue;
        }
        if (strcmp(argv[i], "--backend") == 0 || strcmp(argv[i], "--literal-backend") == 0) {
            const char* backend = NULL;

            if (!bx_litbench_next_arg(argc, argv, &i, argv[i], &backend))
                return 2;
            if (!bx_litbench_parse_backend(backend, &args->literal_backend))
                return 2;
            continue;
        }

        value = bx_litbench_option_value(argv[i], "--file");
        if (value) {
            if (!bx_litbench_add_file_arg(args, value))
                return 2;
            continue;
        }
        if (strcmp(argv[i], "--file") == 0) {
            const char* path = NULL;

            if (!bx_litbench_next_arg(argc, argv, &i, "--file", &path))
                return 2;
            if (!bx_litbench_add_file_arg(args, path))
                return 2;
            continue;
        }

        value = bx_litbench_option_value(argv[i], "--tree-sample");
        if (value) {
            if (!bx_litbench_add_tree_root_arg(args, value))
                return 2;
            continue;
        }
        if (strcmp(argv[i], "--tree-sample") == 0) {
            const char* root = NULL;

            if (!bx_litbench_next_arg(argc, argv, &i, "--tree-sample", &root))
                return 2;
            if (!bx_litbench_add_tree_root_arg(args, root))
                return 2;
            continue;
        }

        value = bx_litbench_option_value(argv[i], "--random-ascii");
        if (value) {
            if (!bx_litbench_parse_byte_size(value, "--random-ascii", &args->random_ascii_bytes))
                return 2;
            args->have_random_ascii = true;
            continue;
        }
        if (strcmp(argv[i], "--random-ascii") == 0) {
            const char* bytes = NULL;

            if (!bx_litbench_next_arg(argc, argv, &i, "--random-ascii", &bytes))
                return 2;
            if (!bx_litbench_parse_byte_size(bytes, "--random-ascii", &args->random_ascii_bytes))
                return 2;
            args->have_random_ascii = true;
            continue;
        }

        value = bx_litbench_option_value(argv[i], "--source-like");
        if (value) {
            if (!bx_litbench_parse_byte_size(value, "--source-like", &args->source_like_bytes))
                return 2;
            args->have_source_like = true;
            continue;
        }
        if (strcmp(argv[i], "--source-like") == 0) {
            const char* bytes = NULL;

            if (!bx_litbench_next_arg(argc, argv, &i, "--source-like", &bytes))
                return 2;
            if (!bx_litbench_parse_byte_size(bytes, "--source-like", &args->source_like_bytes))
                return 2;
            args->have_source_like = true;
            continue;
        }

        value = bx_litbench_option_value(argv[i], "--sample-files");
        if (value) {
            if (!bx_litbench_parse_size(value, "--sample-files", true, &args->sample_files))
                return 2;
            continue;
        }
        if (strcmp(argv[i], "--sample-files") == 0) {
            const char* sample_files = NULL;

            if (!bx_litbench_next_arg(argc, argv, &i, "--sample-files", &sample_files))
                return 2;
            if (!bx_litbench_parse_size(sample_files, "--sample-files", true, &args->sample_files))
                return 2;
            continue;
        }

        value = bx_litbench_option_value(argv[i], "--warmup");
        if (value) {
            if (!bx_litbench_parse_size(value, "--warmup", true, &args->warmup))
                return 2;
            continue;
        }
        if (strcmp(argv[i], "--warmup") == 0) {
            const char* warmup = NULL;

            if (!bx_litbench_next_arg(argc, argv, &i, "--warmup", &warmup))
                return 2;
            if (!bx_litbench_parse_size(warmup, "--warmup", true, &args->warmup))
                return 2;
            continue;
        }

        value = bx_litbench_option_value(argv[i], "--iterations");
        if (value) {
            if (!bx_litbench_parse_size(value, "--iterations", false, &args->iterations))
                return 2;
            continue;
        }
        if (strcmp(argv[i], "--iterations") == 0) {
            const char* iterations = NULL;

            if (!bx_litbench_next_arg(argc, argv, &i, "--iterations", &iterations))
                return 2;
            if (!bx_litbench_parse_size(iterations, "--iterations", false, &args->iterations)) {
                return 2;
            }
            continue;
        }

        fprintf(stderr, "bx-litbench: unknown option: %s\n", argv[i]);
        return 2;
    }

    if (!args->needle) {
        fprintf(stderr, "bx-litbench: --needle is required\n");
        return 2;
    }
    size_t mode_count = (args->file_count > 0u ? 1u : 0u) + (args->tree_root_count > 0u ? 1u : 0u) + (args->have_random_ascii ? 1u : 0u) + (args->have_source_like ? 1u : 0u);
    if (mode_count == 0u) {
        fprintf(stderr, "bx-litbench: one input mode is required: --file, --tree-sample, --random-ascii, or --source-like\n");
        return 2;
    }
    if (mode_count > 1u) {
        fprintf(stderr, "bx-litbench: input modes must not be mixed\n");
        return 2;
    }

    return 0;
}

static bool bx_litbench_read_exact(int fd, unsigned char* buf, size_t len, size_t* read_out) {
    size_t offset = 0u;

    while (offset < len) {
        size_t want = len - offset;
        ssize_t nread;

        if (want > (size_t)SSIZE_MAX)
            want = (size_t)SSIZE_MAX;
        nread = read(fd, buf + offset, want);
        if (nread < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (nread == 0)
            break;
        offset += (size_t)nread;
    }

    if (read_out)
        *read_out = offset;
    return true;
}

static bool bx_litbench_read_file(const char* path, struct bx_litbench_file* file, bool report_errors) {
    int fd;
    struct stat st;
    size_t len;
    size_t read_len = 0u;
    unsigned char* data;

    if (!path || !file)
        return false;

    *file = (struct bx_litbench_file){0};
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        if (report_errors)
            fprintf(stderr, "bx-litbench: open %s: %s\n", path, strerror(errno));
        return false;
    }

    if (fstat(fd, &st) != 0) {
        if (report_errors)
            fprintf(stderr, "bx-litbench: stat %s: %s\n", path, strerror(errno));
        close(fd);
        return false;
    }
    if (!S_ISREG(st.st_mode)) {
        if (report_errors)
            fprintf(stderr, "bx-litbench: %s is not a regular file\n", path);
        close(fd);
        return false;
    }
    if (st.st_size < 0 || (uintmax_t)st.st_size > (uintmax_t)SIZE_MAX) {
        if (report_errors)
            fprintf(stderr, "bx-litbench: %s is too large to scan in one buffer\n", path);
        close(fd);
        return false;
    }

    len = (size_t)st.st_size;
    data = malloc(len > 0u ? len : 1u);
    if (!data) {
        if (report_errors)
            fprintf(stderr, "bx-litbench: failed to allocate %zu input bytes\n", len);
        close(fd);
        return false;
    }

    if (!bx_litbench_read_exact(fd, data, len, &read_len)) {
        if (report_errors)
            fprintf(stderr, "bx-litbench: read %s: %s\n", path, strerror(errno));
        free(data);
        close(fd);
        return false;
    }
    if (close(fd) != 0) {
        if (report_errors)
            fprintf(stderr, "bx-litbench: close %s: %s\n", path, strerror(errno));
        free(data);
        return false;
    }

    file->data = data;
    file->len = read_len;
    return true;
}

static void bx_litbench_free_input(struct bx_litbench_input* input) {
    if (!input)
        return;
    for (size_t i = 0u; i < input->count; ++i)
        free(input->files[i].data);
    free(input->files);
    *input = (struct bx_litbench_input){0};
}

static bool bx_litbench_input_append(struct bx_litbench_input* input, struct bx_litbench_file* file) {
    struct bx_litbench_file* new_files;
    size_t new_cap;

    if (!input || !file)
        return false;
    if (input->count == input->cap) {
        new_cap = input->cap == 0u ? 16u : input->cap * 2u;
        if (new_cap < input->cap || new_cap > SIZE_MAX / sizeof(*input->files)) {
            fprintf(stderr, "bx-litbench: too many input files\n");
            return false;
        }
        new_files = realloc(input->files, new_cap * sizeof(*input->files));
        if (!new_files) {
            fprintf(stderr, "bx-litbench: failed to grow input file storage\n");
            return false;
        }
        input->files = new_files;
        input->cap = new_cap;
    }
    if (SIZE_MAX - input->bytes < file->len) {
        fprintf(stderr, "bx-litbench: input byte count overflow\n");
        return false;
    }
    input->files[input->count++] = *file;
    input->bytes += file->len;
    *file = (struct bx_litbench_file){0};
    return true;
}

static bool bx_litbench_read_input(const struct bx_litbench_args* args, struct bx_litbench_input* input) {
    if (!args || !input || args->file_count == 0u)
        return false;

    *input = (struct bx_litbench_input){0};
    input->files = calloc(args->file_count, sizeof(*input->files));
    if (!input->files) {
        fprintf(stderr, "bx-litbench: failed to allocate input file storage\n");
        return false;
    }
    input->cap = args->file_count;

    for (size_t i = 0u; i < args->file_count; ++i) {
        struct bx_litbench_file file = {0};

        if (!bx_litbench_read_file(args->files[i], &file, true)) {
            bx_litbench_free_input(input);
            return false;
        }
        if (!bx_litbench_input_append(input, &file)) {
            free(file.data);
            bx_litbench_free_input(input);
            return false;
        }
    }
    return true;
}

struct bx_litbench_tree_sample_state {
    struct bx_litbench_input* input;
    size_t sample_limit;
    bool stop;
};

static bool bx_litbench_tree_sample_limit_reached(const struct bx_litbench_tree_sample_state* state) {
    return state && state->sample_limit != 0u && state->input && state->input->count >= state->sample_limit;
}

static enum bx_walk_action bx_litbench_tree_sample_visit(struct bx_walk_entry* entry, void* user) {
    struct bx_litbench_tree_sample_state* state = user;
    struct bx_litbench_file file = {0};

    if (!state || !state->input || !entry)
        return BX_WALK_ERROR;
    if (entry->is_dir || entry->is_symlink)
        return BX_WALK_CONTINUE;

    state->input->tree_files_seen++;
    if (bx_litbench_tree_sample_limit_reached(state)) {
        state->stop = true;
        return BX_WALK_STOP;
    }

    if (!bx_litbench_read_file(entry->path, &file, false)) {
        state->input->tree_errors++;
        return BX_WALK_CONTINUE;
    }
    if (!bx_litbench_input_append(state->input, &file)) {
        free(file.data);
        return BX_WALK_ERROR;
    }

    if (bx_litbench_tree_sample_limit_reached(state)) {
        state->stop = true;
        return BX_WALK_STOP;
    }
    return BX_WALK_CONTINUE;
}

static enum bx_walk_action bx_litbench_tree_sample_error(const char* path, int errnum, void* user) {
    struct bx_litbench_tree_sample_state* state = user;

    (void)path;
    (void)errnum;
    if (state && state->input)
        state->input->tree_errors++;
    return BX_WALK_CONTINUE;
}

static bool bx_litbench_read_tree_sample_input(const struct bx_litbench_args* args, struct bx_litbench_input* input) {
    if (!args || !input || args->tree_root_count == 0u)
        return false;

    *input = (struct bx_litbench_input){0};
    for (size_t i = 0u; i < args->tree_root_count; ++i) {
        struct bx_litbench_tree_sample_state state = {
            .input = input,
            .sample_limit = args->sample_files,
            .stop = false,
        };
        struct bx_walk_opts walk_opts = {
            .sort_entries = true,
            .follow_symlinks = false,
            .follow_root_symlink = false,
            .suppress_errors = true,
            .max_depth = -1,
            .cycle_mode = BX_WALK_CYCLE_DIR_REPEAT,
            .cycle_report = BX_WALK_CYCLE_IGNORE,
            .stop = &state.stop,
        };
        struct bx_walk_ops walk_ops = {
            .visit = bx_litbench_tree_sample_visit,
            .error = bx_litbench_tree_sample_error,
        };

        if (args->sample_files != 0u && input->count >= args->sample_files)
            break;
        input->tree_roots_walked++;
        if (bx_walk(args->tree_roots[i], &walk_opts, &walk_ops, &state) != 0) {
            bx_litbench_free_input(input);
            return false;
        }
    }
    return true;
}

static uint64_t bx_litbench_random_next(uint64_t* state) {
    uint64_t value = *state;

    value ^= value >> 12;
    value ^= value << 25;
    value ^= value >> 27;
    *state = value;
    return value * UINT64_C(2685821657736338717);
}

static bool bx_litbench_read_random_ascii_input(const struct bx_litbench_args* args, struct bx_litbench_input* input) {
    static const unsigned char alphabet[] =
        "abcdefghijklmopqrstuvwxyz"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "0123456789"
        " !\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~";
    struct bx_litbench_file file = {0};
    uint64_t rng = BX_LITBENCH_RANDOM_ASCII_SEED;
    size_t alphabet_len = sizeof(alphabet) - 1u;

    if (!args || !input || !args->have_random_ascii || args->random_ascii_bytes == 0u)
        return false;

    *input = (struct bx_litbench_input){0};
    file.data = malloc(args->random_ascii_bytes);
    if (!file.data) {
        fprintf(stderr, "bx-litbench: failed to allocate %zu random ASCII bytes\n", args->random_ascii_bytes);
        return false;
    }
    file.len = args->random_ascii_bytes;
    for (size_t i = 0u; i < file.len; ++i)
        file.data[i] = alphabet[bx_litbench_random_next(&rng) % alphabet_len];

    if (!bx_litbench_input_append(input, &file)) {
        free(file.data);
        return false;
    }
    input->random_ascii_bytes = args->random_ascii_bytes;
    return true;
}

static void bx_litbench_copy_truncated(unsigned char* dst, size_t dst_len, size_t* pos, const char* text, size_t text_len) {
    size_t avail;

    if (!dst || !pos || !text || *pos >= dst_len)
        return;
    avail = dst_len - *pos;
    if (text_len > avail)
        text_len = avail;
    memcpy(dst + *pos, text, text_len);
    *pos += text_len;
}

static bool bx_litbench_source_like_emit(unsigned char* dst, size_t dst_len, size_t* pos, uint64_t* rng, size_t line_index) {
    static const char* types[] = {"int", "size_t", "uint64_t", "bool", "char"};
    static const char* ops[] = {"+", "-", "^", "|", "&"};
    char line[512];
    uint64_t a = bx_litbench_random_next(rng);
    uint64_t b = bx_litbench_random_next(rng);
    uint64_t c = bx_litbench_random_next(rng);
    int n;

    switch (line_index % 5u) {
        case 0:
            n = snprintf(line, sizeof(line), "static %s bx_generated_%04" PRIu64 "(%s arg_%02" PRIu64 ") {\n", types[a % (sizeof(types) / sizeof(types[0]))], a % 10000u,
                         types[b % (sizeof(types) / sizeof(types[0]))], b % 64u);
            break;
        case 1:
            n = snprintf(line, sizeof(line), "    uint64_t value_%02" PRIu64 " = UINT64_C(%" PRIu64 ");\n", a % 64u, b);
            break;
        case 2:
            n = snprintf(line, sizeof(line), "    if ((value_%02" PRIu64 " %s %" PRIu64 ") != 0) return (%s)(arg_%02" PRIu64 " %s value_%02" PRIu64 ");\n", a % 64u,
                         ops[b % (sizeof(ops) / sizeof(ops[0]))], (c % 127u) + 1u, types[c % (sizeof(types) / sizeof(types[0]))], b % 64u, ops[a % (sizeof(ops) / sizeof(ops[0]))], a % 64u);
            break;
        case 3:
            n = snprintf(line, sizeof(line), "    /* branch marker:%04" PRIu64 " state:%08" PRIx64 " */\n", a % 10000u, b);
            break;
        default:
            n = snprintf(line, sizeof(line), "    return 0;\n}\n\n");
            break;
    }

    if (n < 0)
        return false;
    bx_litbench_copy_truncated(dst, dst_len, pos, line, n < (int)sizeof(line) ? (size_t)n : strlen(line));
    return true;
}

static bool bx_litbench_read_source_like_input(const struct bx_litbench_args* args, struct bx_litbench_input* input) {
    struct bx_litbench_file file = {0};
    uint64_t rng = BX_LITBENCH_SOURCE_LIKE_SEED;
    size_t pos = 0u;
    size_t line_index = 0u;

    if (!args || !input || !args->have_source_like || args->source_like_bytes == 0u)
        return false;

    *input = (struct bx_litbench_input){0};
    file.data = malloc(args->source_like_bytes);
    if (!file.data) {
        fprintf(stderr, "bx-litbench: failed to allocate %zu source-like bytes\n", args->source_like_bytes);
        return false;
    }
    file.len = args->source_like_bytes;
    while (pos < file.len) {
        if (!bx_litbench_source_like_emit(file.data, file.len, &pos, &rng, line_index++)) {
            free(file.data);
            return false;
        }
    }

    if (!bx_litbench_input_append(input, &file)) {
        free(file.data);
        return false;
    }
    input->source_like_bytes = args->source_like_bytes;
    return true;
}

static double bx_litbench_now_seconds(void) {
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0.0;
    return (double)ts.tv_sec + ((double)ts.tv_nsec / 1000000000.0);
}

static int bx_litbench_compare_double(const void* a, const void* b) {
    double left = *(const double*)a;
    double right = *(const double*)b;

    if (left < right)
        return -1;
    if (left > right)
        return 1;
    return 0;
}

static double bx_litbench_median(const double* values, size_t count) {
    double* copy;
    double result;

    if (!values || count == 0u)
        return 0.0;
    copy = malloc(count * sizeof(*copy));
    if (!copy)
        return 0.0;
    memcpy(copy, values, count * sizeof(*copy));
    qsort(copy, count, sizeof(*copy), bx_litbench_compare_double);
    if ((count & 1u) != 0u) {
        result = copy[count / 2u];
    }
    else {
        result = (copy[(count / 2u) - 1u] + copy[count / 2u]) / 2.0;
    }
    free(copy);
    return result;
}

static double bx_litbench_bytes_per_sec(size_t bytes, double seconds) {
    if (seconds <= 0.0)
        return 0.0;
    return (double)bytes / seconds;
}

static const char* bx_litbench_backend_name(enum bx_literal_backend backend) {
    switch (backend) {
        case BX_LITERAL_BACKEND_SCALAR:
            return "scalar";
        case BX_LITERAL_BACKEND_SSE2:
            return "sse2";
        case BX_LITERAL_BACKEND_AVX2:
            return "avx2";
        case BX_LITERAL_BACKEND_ARM64_NEON:
            return "neon";
        case BX_LITERAL_BACKEND_ARM64_SVE:
            return "sve";
    }
    return "unknown";
}

static bool bx_litbench_scan_once(struct bx_literal_matcher* matcher, const struct bx_litbench_file* file, bool* match_found, size_t* match_offset) {
    struct bx_match match = {0};
    int rc;

    if (!matcher || !file || !match_found || !match_offset)
        return false;

    rc = bx_literal_find(matcher, file->data, file->len, 0u, &match);
    if (rc == 0) {
        *match_found = true;
        *match_offset = match.start;
    }
    else {
        *match_found = false;
        *match_offset = SIZE_MAX;
    }
    return true;
}

static bool bx_litbench_scan_input_once(struct bx_literal_matcher* matcher, const struct bx_litbench_input* input, size_t* matches, bool* match_found, size_t* match_file_index, size_t* match_offset) {
    bool any_found = false;
    size_t last_index = SIZE_MAX;
    size_t last_offset = SIZE_MAX;
    size_t local_matches = 0u;

    if (!matcher || !input || !matches || !match_found || !match_file_index || !match_offset) {
        return false;
    }

    for (size_t i = 0u; i < input->count; ++i) {
        bool found = false;
        size_t offset = SIZE_MAX;

        if (!bx_litbench_scan_once(matcher, &input->files[i], &found, &offset))
            return false;
        if (!found)
            continue;
        any_found = true;
        last_index = i;
        last_offset = offset;
        local_matches++;
    }

    *matches = local_matches;
    *match_found = any_found;
    *match_file_index = last_index;
    *match_offset = last_offset;
    return true;
}

static bool bx_litbench_run_file(const struct bx_litbench_args* args, const struct bx_litbench_input* input, struct bx_litbench_run* run) {
    struct bx_literal_matcher* matcher = NULL;
    double* durations;
    double* throughputs;

    if (!args || !input || !run)
        return false;

    *run = (struct bx_litbench_run){0};
    run->last_match_file_index = SIZE_MAX;
    run->last_match_offset = SIZE_MAX;
    if (args->literal_backend && setenv("BX_SEARCH_LITERAL_BACKEND", args->literal_backend, 1) != 0) {
        fprintf(stderr, "bx-litbench: failed to force literal backend %s: %s\n", args->literal_backend, strerror(errno));
        return false;
    }
    durations = calloc(args->iterations, sizeof(*durations));
    if (!durations) {
        fprintf(stderr, "bx-litbench: failed to allocate timing storage\n");
        return false;
    }
    throughputs = calloc(args->iterations, sizeof(*throughputs));
    if (!throughputs) {
        fprintf(stderr, "bx-litbench: failed to allocate throughput storage\n");
        free(durations);
        return false;
    }
    if (bx_literal_compile(&matcher, args->needle, false, false) != 0 || !matcher) {
        fprintf(stderr, "bx-litbench: failed to compile literal needle\n");
        free(throughputs);
        free(durations);
        return false;
    }
    const struct bx_lit_plan* plan = bx_literal_absence_plan(matcher);
    if (plan) {
        run->resolved_backend = plan->backend;
        run->have_resolved_backend = true;
    }

    for (size_t i = 0u; i < args->warmup; ++i) {
        bool found = false;
        size_t matches = 0u;
        size_t file_index = SIZE_MAX;
        size_t offset = SIZE_MAX;

        if (!bx_litbench_scan_input_once(matcher, input, &matches, &found, &file_index, &offset)) {
            bx_literal_free(matcher);
            free(throughputs);
            free(durations);
            return false;
        }
    }

    for (size_t i = 0u; i < args->iterations; ++i) {
        bool found = false;
        size_t matches = 0u;
        size_t file_index = SIZE_MAX;
        size_t offset = SIZE_MAX;
        double started = bx_litbench_now_seconds();
        double elapsed;

        if (!bx_litbench_scan_input_once(matcher, input, &matches, &found, &file_index, &offset)) {
            bx_literal_free(matcher);
            free(throughputs);
            free(durations);
            return false;
        }
        elapsed = bx_litbench_now_seconds() - started;
        durations[i] = elapsed >= 0.0 ? elapsed : 0.0;
        throughputs[i] = bx_litbench_bytes_per_sec(input->bytes, durations[i]);
        run->matches += matches;
        run->last_match_found = found;
        run->last_match_file_index = file_index;
        run->last_match_offset = offset;
    }

    run->median_seconds = bx_litbench_median(durations, args->iterations);
    run->median_bytes_per_sec = bx_litbench_median(throughputs, args->iterations);
    run->min_seconds = durations[0];
    run->max_seconds = durations[0];
    run->min_bytes_per_sec = throughputs[0];
    run->max_bytes_per_sec = throughputs[0];
    for (size_t i = 1u; i < args->iterations; ++i) {
        if (durations[i] < run->min_seconds)
            run->min_seconds = durations[i];
        if (durations[i] > run->max_seconds)
            run->max_seconds = durations[i];
        if (throughputs[i] < run->min_bytes_per_sec)
            run->min_bytes_per_sec = throughputs[i];
        if (throughputs[i] > run->max_bytes_per_sec)
            run->max_bytes_per_sec = throughputs[i];
    }
    if (args->have_perf_cycles && input->bytes > 0u) {
        double timed_bytes = (double)input->bytes * (double)args->iterations;

        if (timed_bytes > 0.0) {
            run->cycles_per_byte = (double)args->perf_cycles / timed_bytes;
            run->have_cycles_per_byte = true;
        }
    }

    bx_literal_free(matcher);
    free(throughputs);
    free(durations);
    return true;
}

static void bx_litbench_free_args(struct bx_litbench_args* args) {
    if (!args)
        return;
    free(args->files);
    free(args->tree_roots);
    args->files = NULL;
    args->tree_roots = NULL;
}

static const char* bx_litbench_mode_name(const struct bx_litbench_args* args, const struct bx_litbench_input* input) {
    if (args && args->have_source_like)
        return "source_like";
    if (args && args->have_random_ascii)
        return "random_ascii";
    if (args && args->tree_root_count > 0u)
        return "tree_sample";
    if (input && input->count == 1u)
        return "file";
    return "file_list";
}

static int bx_litbench_finish(int status) {
    bx_search_dev_counters_report(stderr);
    bx_search_dev_counters_reset();
    return status;
}

int main(int argc, char** argv) {
    struct bx_litbench_args args;
    struct bx_litbench_input input = {0};
    struct bx_litbench_run run = {0};
    int parsed;

    parsed = bx_litbench_parse_args(argc, argv, &args);
    if (parsed == 1) {
        bx_litbench_free_args(&args);
        return bx_litbench_finish(0);
    }
    if (parsed != 0) {
        bx_litbench_usage(stderr);
        bx_litbench_free_args(&args);
        return bx_litbench_finish(parsed);
    }

    if (args.print_counters && setenv("BX_SEARCH_DEV_COUNTERS", "1", 1) != 0) {
        fprintf(stderr, "bx-litbench: failed to enable dev counters: %s\n", strerror(errno));
        bx_litbench_free_args(&args);
        return bx_litbench_finish(2);
    }
    bx_search_dev_counters_begin_from_env();

    if (args.tree_root_count > 0u) {
        if (!bx_litbench_read_tree_sample_input(&args, &input)) {
            bx_litbench_free_args(&args);
            return bx_litbench_finish(2);
        }
    }
    else if (args.have_random_ascii) {
        if (!bx_litbench_read_random_ascii_input(&args, &input)) {
            bx_litbench_free_args(&args);
            return bx_litbench_finish(2);
        }
    }
    else if (args.have_source_like) {
        if (!bx_litbench_read_source_like_input(&args, &input)) {
            bx_litbench_free_args(&args);
            return bx_litbench_finish(2);
        }
    }
    else if (!bx_litbench_read_input(&args, &input)) {
        bx_litbench_free_args(&args);
        return bx_litbench_finish(2);
    }
    if (!bx_litbench_run_file(&args, &input, &run)) {
        bx_litbench_free_input(&input);
        bx_litbench_free_args(&args);
        return bx_litbench_finish(2);
    }

    printf(
        "bx-litbench: mode=%s input_files=%zu input_bytes=%zu tree_roots=%zu "
        "tree_roots_walked=%zu tree_files_seen=%zu tree_errors=%zu "
        "tree_sample_limit=%zu random_ascii_bytes=%zu random_ascii_seed=%" PRIu64 " source_like_bytes=%zu source_like_seed=%" PRIu64
        " needle_len=%zu literal_backend_requested=%s literal_backend_resolved=%s "
        "warmup=%zu iterations=%zu matches=%zu last_result=%s "
        "last_match_file_index=",
        bx_litbench_mode_name(&args, &input), input.count, input.bytes, args.tree_root_count, input.tree_roots_walked, input.tree_files_seen, input.tree_errors, args.sample_files,
        input.random_ascii_bytes, (uint64_t)BX_LITBENCH_RANDOM_ASCII_SEED, input.source_like_bytes, (uint64_t)BX_LITBENCH_SOURCE_LIKE_SEED, strlen(args.needle),
        args.literal_backend ? args.literal_backend : "default", run.have_resolved_backend ? bx_litbench_backend_name(run.resolved_backend) : "unknown", args.warmup, args.iterations, run.matches,
        run.last_match_found ? "found" : "not_found");
    if (run.last_match_found)
        printf("%zu", run.last_match_file_index);
    else
        printf("none");
    printf(" last_match_offset=");
    if (run.last_match_found)
        printf("%zu", run.last_match_offset);
    else
        printf("none");
    printf(" median_seconds=%.9f min_seconds=%.9f max_seconds=%.9f median_bytes_per_sec=%.3f min_bytes_per_sec=%.3f max_bytes_per_sec=%.3f", run.median_seconds, run.min_seconds, run.max_seconds,
           run.median_bytes_per_sec, run.min_bytes_per_sec, run.max_bytes_per_sec);
    printf(" perf_cycles=");
    if (args.have_perf_cycles)
        printf("%" PRIu64, args.perf_cycles);
    else
        printf("none");
    printf(" cycles_per_byte=");
    if (run.have_cycles_per_byte)
        printf("%.6f", run.cycles_per_byte);
    else
        printf("unavailable");
    printf("\n");

    bx_litbench_free_input(&input);
    bx_litbench_free_args(&args);
    return bx_litbench_finish(0);
}
