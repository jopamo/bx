#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "applets.h"
#include "bx/diag.h"
#include "bx/libbx.h"
#include "lib/cli_common.h"

enum bx_du_symlink_mode {
    BX_DU_SYMLINK_NEVER = 0,
    BX_DU_SYMLINK_COMMAND_LINE,
    BX_DU_SYMLINK_ALWAYS,
};

enum bx_du_output_mode {
    BX_DU_OUTPUT_BLOCKS = 0,
    BX_DU_OUTPUT_HUMAN_1024,
    BX_DU_OUTPUT_HUMAN_1000,
};

struct bx_du_options {
    const char* progname;
    bool all;
    bool summarize;
    bool total;
    bool one_file_system;
    bool apparent_size;
    bool count_links;
    bool null_terminate;
    bool limit_depth;
    uintmax_t max_depth;
    uintmax_t output_block_size;
    enum bx_du_output_mode output_mode;
    enum bx_du_symlink_mode symlink_mode;
    bool show_help;
    bool show_version;
};

enum {
    BX_DU_OPT_HELP = 1,
    BX_DU_OPT_VERSION = 2,
    BX_DU_OPT_MAX_DEPTH = 3,
    BX_DU_OPT_APPARENT_SIZE = 4,
    BX_DU_OPT_SI = 5,
};

struct bx_du_inode_slot {
    dev_t dev;
    ino_t ino;
    bool occupied;
};

struct bx_du_inode_set {
    struct bx_du_inode_slot* slots;
    size_t capacity;
    size_t count;
};

struct bx_du_context {
    const struct bx_du_options* options;
    struct bx_diag_ctx* diag;
    struct bx_du_inode_set seen;
};

struct bx_du_dir_stack_entry {
    dev_t dev;
    ino_t ino;
    const struct bx_du_dir_stack_entry* next;
};

static void bx_du_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... [FILE]...\n", progname);
    fprintf(stream, "Summarize device usage of the set of FILEs, recursively for directories.\n");
    fprintf(stream, "With no FILE, use '.'.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -0, --null            end each output line with NUL, not newline\n");
    fprintf(stream, "  -a, --all             write counts for all files, not just directories\n");
    fprintf(stream, "  -b, --bytes           equivalent to --apparent-size --block-size=1\n");
    fprintf(stream, "  -c, --total           produce a grand total\n");
    fprintf(stream, "  -h, --human-readable  print sizes in human readable format (e.g., 1.0K)\n");
    fprintf(stream, "  -k                    use 1K blocks (default)\n");
    fprintf(stream, "  -l, --count-links     count sizes many times if hard linked\n");
    fprintf(stream, "      --si              like -h, but use powers of 1000 not 1024\n");
    fprintf(stream, "  -x, --one-file-system skip directories on different file systems\n");
    fprintf(stream, "  -B, --block-size=SIZE scale sizes by SIZE before printing\n");
    fprintf(stream, "      --apparent-size   print apparent sizes, rather than disk usage\n");
    fprintf(stream, "  -D                    dereference only command line symlinks (same as -H)\n");
    fprintf(stream, "  -H                    dereference command line symlinks\n");
    fprintf(stream, "  -L, --dereference     dereference all symbolic links\n");
    fprintf(stream, "  -P, --no-dereference  do not dereference symbolic links (default)\n");
    fprintf(stream, "  -d, --max-depth=N     print the total for a directory only if it is N or fewer levels below arguments\n");
    fprintf(stream, "  -s, --summarize       display only a total for each argument\n");
    fprintf(stream, "      --help            display this help and exit\n");
    fprintf(stream, "      --version         output version information and exit\n");
}

static bool bx_du_parse_max_depth(const char* text, uintmax_t* depth_out, struct bx_diag_ctx* diag) {
    if (text == NULL || text[0] == '\0' || text[0] == '-') {
        bx_diag(diag, "invalid maximum depth '%s'", text != NULL ? text : "");
        return false;
    }

    errno = 0;
    char* end = NULL;
    uintmax_t depth = strtoumax(text, &end, 10);
    if (errno == ERANGE || end == text || (end != NULL && *end != '\0')) {
        bx_diag(diag, "invalid maximum depth '%s'", text);
        return false;
    }

    *depth_out = depth;
    return true;
}

static bool bx_du_parse_size_suffix(const char* suffix, uintmax_t* multiplier_out) {
    if (suffix == NULL || multiplier_out == NULL) {
        return false;
    }

    if (suffix[0] == '\0') {
        *multiplier_out = 1u;
        return true;
    }

    if (suffix[1] == '\0') {
        if (suffix[0] == 'B') {
            *multiplier_out = 1u;
            return true;
        }
        if (suffix[0] == 'b') {
            *multiplier_out = 512u;
            return true;
        }
    }

    static const char scale_letters[] = "KMGTPEZYRQ";
    char normalized = (char)toupper((unsigned char)suffix[0]);
    const char* letter_pos = strchr(scale_letters, normalized);
    if (letter_pos == NULL) {
        return false;
    }

    uintmax_t base = 0u;
    size_t suffix_len = strlen(suffix);
    if (suffix_len == 1u) {
        base = 1024u;
    }
    else if (suffix_len == 2u && (suffix[1] == 'B' || suffix[1] == 'b')) {
        base = 1000u;
    }
    else if (suffix_len == 3u && (suffix[1] == 'i' || suffix[1] == 'I') && (suffix[2] == 'B' || suffix[2] == 'b')) {
        base = 1024u;
    }
    else {
        return false;
    }

    unsigned int power = (unsigned int)(letter_pos - scale_letters) + 1u;
    uintmax_t multiplier = 1u;
    for (unsigned int i = 0u; i < power; i++) {
        if (multiplier > UINTMAX_MAX / base) {
            return false;
        }
        multiplier *= base;
    }

    *multiplier_out = multiplier;
    return true;
}

static bool bx_du_parse_block_size(const char* text, uintmax_t* size_out, struct bx_diag_ctx* diag) {
    if (text == NULL || text[0] == '\0' || !isdigit((unsigned char)text[0])) {
        bx_diag(diag, "invalid --block-size argument '%s'", text != NULL ? text : "");
        return false;
    }

    errno = 0;
    char* end = NULL;
    uintmax_t value = strtoumax(text, &end, 10);
    if (errno == ERANGE || end == text || end == NULL) {
        bx_diag(diag, "invalid --block-size argument '%s'", text);
        return false;
    }

    uintmax_t multiplier = 1u;
    if (!bx_du_parse_size_suffix(end, &multiplier)) {
        bx_diag(diag, "invalid --block-size argument '%s'", text);
        return false;
    }

    if (value == 0u || value > UINTMAX_MAX / multiplier) {
        bx_diag(diag, "invalid --block-size argument '%s'", text);
        return false;
    }

    *size_out = value * multiplier;
    return true;
}

static bool bx_du_parse_options(int argc, char** argv, struct bx_du_options* options, int* first_operand, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"null", no_argument, NULL, '0'},
        {"all", no_argument, NULL, 'a'},
        {"bytes", no_argument, NULL, 'b'},
        {"summarize", no_argument, NULL, 's'},
        {"total", no_argument, NULL, 'c'},
        {"human-readable", no_argument, NULL, 'h'},
        {"si", no_argument, NULL, BX_DU_OPT_SI},
        {"count-links", no_argument, NULL, 'l'},
        {"one-file-system", no_argument, NULL, 'x'},
        {"block-size", required_argument, NULL, 'B'},
        {"apparent-size", no_argument, NULL, BX_DU_OPT_APPARENT_SIZE},
        {"dereference-args", no_argument, NULL, 'D'},
        {"dereference", no_argument, NULL, 'L'},
        {"no-dereference", no_argument, NULL, 'P'},
        {"max-depth", required_argument, NULL, BX_DU_OPT_MAX_DEPTH},
        {"help", no_argument, NULL, BX_DU_OPT_HELP},
        {"version", no_argument, NULL, BX_DU_OPT_VERSION},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "du");
    options->output_block_size = 1024u;
    options->output_mode = BX_DU_OUTPUT_BLOCKS;
    options->symlink_mode = BX_DU_SYMLINK_NEVER;
    diag->progname = options->progname;

    opterr = 0;
    optind = 1;

    while (true) {
        int option_index = 0;
        int c = getopt_long(argc, argv, "+0abclhksxd:B:DHLP", long_options, &option_index);
        if (c == -1) {
            break;
        }

        switch (c) {
            case '0':
                options->null_terminate = true;
                break;
            case 'a':
                options->all = true;
                break;
            case 'b':
                options->apparent_size = true;
                options->output_block_size = 1u;
                options->output_mode = BX_DU_OUTPUT_BLOCKS;
                break;
            case 'c':
                options->total = true;
                break;
            case 'h':
                options->output_mode = BX_DU_OUTPUT_HUMAN_1024;
                break;
            case 'k':
                options->output_block_size = 1024u;
                options->output_mode = BX_DU_OUTPUT_BLOCKS;
                break;
            case 'l':
                options->count_links = true;
                break;
            case 'x':
                options->one_file_system = true;
                break;
            case 'B':
                if (!bx_du_parse_block_size(optarg, &options->output_block_size, diag)) {
                    return false;
                }
                options->output_mode = BX_DU_OUTPUT_BLOCKS;
                break;
            case BX_DU_OPT_APPARENT_SIZE:
                options->apparent_size = true;
                break;
            case BX_DU_OPT_SI:
                options->output_mode = BX_DU_OUTPUT_HUMAN_1000;
                break;
            case 'D':
            case 'H':
                options->symlink_mode = BX_DU_SYMLINK_COMMAND_LINE;
                break;
            case 'L':
                options->symlink_mode = BX_DU_SYMLINK_ALWAYS;
                break;
            case 'P':
                options->symlink_mode = BX_DU_SYMLINK_NEVER;
                break;
            case 's':
                options->summarize = true;
                break;
            case 'd':
            case BX_DU_OPT_MAX_DEPTH:
                options->limit_depth = true;
                if (!bx_du_parse_max_depth(optarg, &options->max_depth, diag)) {
                    return false;
                }
                break;
            case BX_DU_OPT_HELP:
                options->show_help = true;
                return true;
            case BX_DU_OPT_VERSION:
                options->show_version = true;
                return true;
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

    if (options->all && options->summarize) {
        bx_diag(diag, "cannot both summarize and show all entries");
        bx_diag(diag, "Try '%s --help' for more information.", options->progname);
        return false;
    }
    if (options->summarize && options->limit_depth) {
        if (options->max_depth == 0u) {
            (void)fprintf(stderr, "%s: warning: summarizing is the same as using --max-depth=0\n", options->progname);
        }
        else {
            bx_diag(diag, "summarizing conflicts with --max-depth=%" PRIuMAX, options->max_depth);
            bx_diag(diag, "Try '%s --help' for more information.", options->progname);
            return false;
        }
    }

    *first_operand = optind;
    return true;
}

static uintmax_t bx_du_saturating_add(uintmax_t lhs, uintmax_t rhs) {
    if (UINTMAX_MAX - lhs < rhs) {
        return UINTMAX_MAX;
    }
    return lhs + rhs;
}

static uintmax_t bx_du_saturating_mul(uintmax_t lhs, uintmax_t rhs) {
    if (lhs == 0u || rhs == 0u) {
        return 0u;
    }
    if (lhs > UINTMAX_MAX / rhs) {
        return UINTMAX_MAX;
    }
    return lhs * rhs;
}

static uintmax_t bx_du_ceil_div(uintmax_t value, uintmax_t divisor) {
    if (divisor == 0u) {
        return 0u;
    }
    return (value / divisor) + ((value % divisor) != 0u ? 1u : 0u);
}

static uint64_t bx_du_mix_u64(uint64_t value) {
    value ^= value >> 30u;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27u;
    value *= UINT64_C(0x94d049bb133111eb);
    value ^= value >> 31u;
    return value;
}

static uint64_t bx_du_inode_hash(dev_t dev, ino_t ino) {
    uint64_t d = (uint64_t)(uintmax_t)dev;
    uint64_t i = (uint64_t)(uintmax_t)ino;
    return bx_du_mix_u64(d) ^ bx_du_mix_u64(i + UINT64_C(0x9e3779b97f4a7c15));
}

static void bx_du_inode_set_init(struct bx_du_inode_set* set) {
    set->slots = NULL;
    set->capacity = 0u;
    set->count = 0u;
}

static void bx_du_inode_set_free(struct bx_du_inode_set* set) {
    free(set->slots);
    set->slots = NULL;
    set->capacity = 0u;
    set->count = 0u;
}

static void bx_du_inode_set_resize(struct bx_du_inode_set* set, size_t new_capacity) {
    struct bx_du_inode_slot* new_slots = xmalloc(new_capacity * sizeof(*new_slots));
    memset(new_slots, 0, new_capacity * sizeof(*new_slots));

    for (size_t i = 0u; i < set->capacity; i++) {
        const struct bx_du_inode_slot* old_slot = &set->slots[i];
        if (!old_slot->occupied) {
            continue;
        }

        size_t index = (size_t)(bx_du_inode_hash(old_slot->dev, old_slot->ino) & (uint64_t)(new_capacity - 1u));
        while (new_slots[index].occupied) {
            index = (index + 1u) & (new_capacity - 1u);
        }
        new_slots[index] = *old_slot;
    }

    free(set->slots);
    set->slots = new_slots;
    set->capacity = new_capacity;
}

static void bx_du_inode_set_grow_if_needed(struct bx_du_inode_set* set) {
    if (set->capacity != 0u && set->count < ((set->capacity * 3u) / 4u)) {
        return;
    }

    size_t new_capacity = (set->capacity == 0u) ? 256u : set->capacity * 2u;
    if (new_capacity <= set->capacity || (new_capacity & (new_capacity - 1u)) != 0u) {
        bx_fatal(3, "du inode set overflow");
    }

    bx_du_inode_set_resize(set, new_capacity);
}

static bool bx_du_inode_set_mark_seen(struct bx_du_inode_set* set, dev_t dev, ino_t ino) {
    bx_du_inode_set_grow_if_needed(set);

    size_t index = (size_t)(bx_du_inode_hash(dev, ino) & (uint64_t)(set->capacity - 1u));
    while (set->slots[index].occupied) {
        if (set->slots[index].dev == dev && set->slots[index].ino == ino) {
            return true;
        }
        index = (index + 1u) & (set->capacity - 1u);
    }

    set->slots[index].occupied = true;
    set->slots[index].dev = dev;
    set->slots[index].ino = ino;
    set->count++;
    return false;
}

static char* bx_du_join_path(const char* parent, const char* child) {
    size_t parent_len = strlen(parent);
    size_t child_len = strlen(child);
    bool need_slash = parent_len > 0u && parent[parent_len - 1u] != '/';

    size_t len = parent_len + (need_slash ? 1u : 0u) + child_len;
    char* path = xmalloc(len + 1u);
    memcpy(path, parent, parent_len);
    if (need_slash) {
        path[parent_len] = '/';
        memcpy(path + parent_len + 1u, child, child_len);
    }
    else {
        memcpy(path + parent_len, child, child_len);
    }
    path[len] = '\0';
    return path;
}

static void bx_du_format_human_size(uintmax_t size_bytes, uintmax_t base, const char* suffixes, char* buffer, size_t buffer_size) {
    if (size_bytes == 0u) {
        (void)snprintf(buffer, buffer_size, "0");
        return;
    }

    size_t suffix_index = 0u;
    size_t suffix_count = strlen(suffixes);
    uintmax_t unit = 1u;

    while (suffix_index < suffix_count) {
        if (unit > UINTMAX_MAX / base) {
            break;
        }
        uintmax_t next_unit = unit * base;
        if (size_bytes < next_unit) {
            break;
        }
        unit = next_unit;
        suffix_index++;
    }

    if (suffix_index == 0u) {
        (void)snprintf(buffer, buffer_size, "%" PRIuMAX, size_bytes);
        return;
    }

    uintmax_t tenths = bx_du_ceil_div(bx_du_saturating_mul(size_bytes, 10u), unit);
    if (tenths < 100u) {
        uintmax_t whole = tenths / 10u;
        uintmax_t fractional = tenths % 10u;
        (void)snprintf(buffer, buffer_size, "%" PRIuMAX ".%" PRIuMAX "%c", whole, fractional, suffixes[suffix_index - 1u]);
    }
    else {
        uintmax_t whole = bx_du_ceil_div(size_bytes, unit);
        (void)snprintf(buffer, buffer_size, "%" PRIuMAX "%c", whole, suffixes[suffix_index - 1u]);
    }
}

static void bx_du_format_size(uintmax_t size_bytes, const struct bx_du_options* options, char* buffer, size_t buffer_size) {
    switch (options->output_mode) {
        case BX_DU_OUTPUT_HUMAN_1024:
            bx_du_format_human_size(size_bytes, 1024u, "KMGTPEZYRQ", buffer, buffer_size);
            return;
        case BX_DU_OUTPUT_HUMAN_1000:
            bx_du_format_human_size(size_bytes, 1000u, "kMGTPEZYRQ", buffer, buffer_size);
            return;
        case BX_DU_OUTPUT_BLOCKS:
            break;
    }

    uintmax_t scaled_size = bx_du_ceil_div(size_bytes, options->output_block_size);
    (void)snprintf(buffer, buffer_size, "%" PRIuMAX, scaled_size);
}

static bool bx_du_emit_line(uintmax_t size_bytes, const char* path, const struct bx_du_options* options, struct bx_diag_ctx* diag) {
    char size_text[64];
    bx_du_format_size(size_bytes, options, size_text, sizeof(size_text));

    if (fprintf(stdout, "%s\t%s", size_text, path) < 0) {
        bx_diag(diag, "write error: %s", strerror(errno));
        return false;
    }
    if (fputc(options->null_terminate ? '\0' : '\n', stdout) == EOF) {
        bx_diag(diag, "write error: %s", strerror(errno));
        return false;
    }

    return true;
}

static bool bx_du_is_dot_or_dotdot(const char* name) {
    return (name[0] == '.' && name[1] == '\0') || (name[0] == '.' && name[1] == '.' && name[2] == '\0');
}

static uintmax_t bx_du_disk_usage_bytes(const struct stat* st) {
    if (st->st_blocks <= 0) {
        return 0u;
    }
    return bx_du_saturating_mul((uintmax_t)st->st_blocks, 512u);
}

static uintmax_t bx_du_apparent_usage_bytes(const struct stat* st) {
    if (st->st_size <= 0) {
        return 0u;
    }
    return (uintmax_t)st->st_size;
}

static uintmax_t bx_du_path_usage_bytes(const struct stat* st, const struct bx_du_options* options) {
    if (options->apparent_size) {
        return bx_du_apparent_usage_bytes(st);
    }
    return bx_du_disk_usage_bytes(st);
}

static bool bx_du_should_follow_symlink(const struct bx_du_options* options, bool top_level) {
    if (options->symlink_mode == BX_DU_SYMLINK_ALWAYS) {
        return true;
    }
    if (options->symlink_mode == BX_DU_SYMLINK_COMMAND_LINE) {
        return top_level;
    }
    return false;
}

static bool bx_du_dir_stack_contains(const struct bx_du_dir_stack_entry* stack, dev_t dev, ino_t ino) {
    for (const struct bx_du_dir_stack_entry* curr = stack; curr != NULL; curr = curr->next) {
        if (curr->dev == dev && curr->ino == ino) {
            return true;
        }
    }
    return false;
}

static uintmax_t bx_du_walk_path(struct bx_du_context* ctx,
                                 const char* path,
                                 bool top_level,
                                 uintmax_t depth,
                                 dev_t root_dev,
                                 bool root_dev_set,
                                 const struct bx_du_dir_stack_entry* dir_stack,
                                 bool* ok_out) {
    const struct bx_du_options* options = ctx->options;

    struct stat lst;
    if (lstat(path, &lst) != 0) {
        bx_perror_path(ctx->diag, path);
        *ok_out = false;
        return 0u;
    }

    struct stat st = lst;
    if (S_ISLNK(lst.st_mode) && bx_du_should_follow_symlink(options, top_level)) {
        if (stat(path, &st) != 0) {
            bx_perror_path(ctx->diag, path);
            *ok_out = false;
            return 0u;
        }
    }

    if (!root_dev_set) {
        root_dev = st.st_dev;
        root_dev_set = true;
    }

    if (!top_level && options->one_file_system && S_ISDIR(st.st_mode) && st.st_dev != root_dev) {
        *ok_out = true;
        return 0u;
    }

    if (S_ISDIR(st.st_mode) && bx_du_dir_stack_contains(dir_stack, st.st_dev, st.st_ino)) {
        *ok_out = true;
        return 0u;
    }

    if (!options->count_links && bx_du_inode_set_mark_seen(&ctx->seen, st.st_dev, st.st_ino)) {
        *ok_out = true;
        return 0u;
    }

    bool ok = true;
    uintmax_t total_bytes = bx_du_path_usage_bytes(&st, options);

    if (S_ISDIR(st.st_mode)) {
        struct bx_du_dir_stack_entry stack_entry = {
            .dev = st.st_dev,
            .ino = st.st_ino,
            .next = dir_stack,
        };

        DIR* dir = opendir(path);
        if (dir == NULL) {
            bx_perror_path(ctx->diag, path);
            ok = false;
        }
        else {
            while (true) {
                errno = 0;
                struct dirent* entry = readdir(dir);
                if (entry == NULL) {
                    if (errno != 0) {
                        bx_perror_path(ctx->diag, path);
                        ok = false;
                    }
                    break;
                }

                if (bx_du_is_dot_or_dotdot(entry->d_name)) {
                    continue;
                }

                char* child_path = bx_du_join_path(path, entry->d_name);
                bool child_ok = true;
                uintmax_t child_depth = depth == UINTMAX_MAX ? UINTMAX_MAX : depth + 1u;
                uintmax_t child_total = bx_du_walk_path(ctx, child_path, false, child_depth, root_dev, root_dev_set, &stack_entry, &child_ok);
                total_bytes = bx_du_saturating_add(total_bytes, child_total);
                if (!child_ok) {
                    ok = false;
                }
                free(child_path);
            }

            if (closedir(dir) != 0) {
                bx_perror_path(ctx->diag, path);
                ok = false;
            }
        }
    }

    bool should_print = false;
    if (S_ISDIR(st.st_mode)) {
        should_print = !options->summarize || top_level;
    }
    else if (top_level || (!options->summarize && options->all)) {
        should_print = true;
    }

    bool within_max_depth = !options->limit_depth || depth <= options->max_depth;
    if (should_print && within_max_depth && !bx_du_emit_line(total_bytes, path, options, ctx->diag)) {
        ok = false;
    }

    *ok_out = ok;
    return total_bytes;
}

int bx_du_main(int argc, char** argv) {
    struct bx_du_options options;
    struct bx_diag_ctx diag = {
        .progname = "du",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };
    int first_operand = 0;

    if (!bx_du_parse_options(argc, argv, &options, &first_operand, &diag)) {
        return diag.exit_status != 0 ? diag.exit_status : 1;
    }

    if (options.show_help) {
        bx_du_print_help(stdout, options.progname);
        return 0;
    }

    if (options.show_version) {
        bx_cli_print_version(options.progname);
        return 0;
    }

    struct bx_du_context ctx = {
        .options = &options,
        .diag = &diag,
    };
    bx_du_inode_set_init(&ctx.seen);

    uintmax_t grand_total_bytes = 0u;

    if (first_operand >= argc) {
        bool operand_ok = true;
        uintmax_t total_bytes = bx_du_walk_path(&ctx, ".", true, 0u, 0, false, NULL, &operand_ok);
        grand_total_bytes = bx_du_saturating_add(grand_total_bytes, total_bytes);
    }
    else {
        for (int i = first_operand; i < argc; i++) {
            bool operand_ok = true;
            uintmax_t total_bytes = bx_du_walk_path(&ctx, argv[i], true, 0u, 0, false, NULL, &operand_ok);
            grand_total_bytes = bx_du_saturating_add(grand_total_bytes, total_bytes);
        }
    }

    if (options.total && !bx_du_emit_line(grand_total_bytes, "total", &options, &diag)) {
        bx_du_inode_set_free(&ctx.seen);
        return diag.exit_status != 0 ? diag.exit_status : 1;
    }

    if (fflush(stdout) == EOF) {
        bx_diag(&diag, "write error: %s", strerror(errno));
    }

    bx_du_inode_set_free(&ctx.seen);
    return diag.exit_status;
}
