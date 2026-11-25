#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"
#include "bx/libbx.h"
#include "lib/cli_common.h"
#include "lib/line_writer.h"
#include "lib/size_parse.h"
#include "lib/option_scan.h"
#include "lib/dir_cycle.h"
#include "lib/fd_ops.h"
#include "lib/path_ops.h"
#include "lib/same_file.h"

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
    BX_DU_OPT_HELP = 256,
    BX_DU_OPT_VERSION,
    BX_DU_OPT_APPARENT_SIZE,
    BX_DU_OPT_SI,
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
    struct bx_line_writer* writer;
    struct bx_du_inode_set seen;
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
    if (text == NULL || text[0] == '\0') {
        bx_diag(diag, "invalid maximum depth '%s'", text != NULL ? text : "");
        return false;
    }

    const char* digits = text;
    while (isspace((unsigned char)*digits)) {
        digits++;
    }
    if (digits[0] == '+') {
        digits++;
    }
    else if (digits[0] == '-') {
        bx_diag(diag, "invalid maximum depth '%s'", text);
        return false;
    }

    uintmax_t depth = 0;
    if (!bx_size_parse_uint(digits, &depth)) {
        bx_diag(diag, "invalid maximum depth '%s'", text);
        return false;
    }
    *depth_out = depth;
    return true;
}

static bool bx_du_parse_block_size(const char* text, uintmax_t* size_out, struct bx_diag_ctx* diag) {
    if (!bx_size_parse_block_size(text, size_out)) {
        bx_diag(diag, "invalid --block-size argument '%s'", text);
        return false;
    }
    return true;
}

static bool bx_du_parse_options(int argc, char** argv, struct bx_du_options* options,
                                const char** operands, size_t* count, struct bx_diag_ctx* diag) {
    static const struct bx_option_spec specs[] = {
        {'0', '0', "null", BX_OPTION_NO_VALUE},
        {'a', 'a', "all", BX_OPTION_NO_VALUE},
        {'b', 'b', "bytes", BX_OPTION_NO_VALUE},
        {'s', 's', "summarize", BX_OPTION_NO_VALUE},
        {'c', 'c', "total", BX_OPTION_NO_VALUE},
        {'h', 'h', "human-readable", BX_OPTION_NO_VALUE},
        {'k', 'k', NULL, BX_OPTION_NO_VALUE},
        {'l', 'l', "count-links", BX_OPTION_NO_VALUE},
        {'x', 'x', "one-file-system", BX_OPTION_NO_VALUE},
        {'B', 'B', "block-size", BX_OPTION_REQUIRED_VALUE},
        {'d', 'd', "max-depth", BX_OPTION_REQUIRED_VALUE},
        {'D', 'D', "dereference-args", BX_OPTION_NO_VALUE},
        {'D', 'H', NULL, BX_OPTION_NO_VALUE},
        {'L', 'L', "dereference", BX_OPTION_NO_VALUE},
        {'P', 'P', "no-dereference", BX_OPTION_NO_VALUE},
        {BX_DU_OPT_APPARENT_SIZE, 0, "apparent-size", BX_OPTION_NO_VALUE},
        {BX_DU_OPT_SI, 0, "si", BX_OPTION_NO_VALUE},
        {BX_DU_OPT_HELP, 0, "help", BX_OPTION_NO_VALUE},
        {BX_DU_OPT_VERSION, 0, "version", BX_OPTION_NO_VALUE},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "du");
    options->output_block_size = 1024u;
    options->output_mode = BX_DU_OUTPUT_BLOCKS;
    options->symlink_mode = BX_DU_SYMLINK_NEVER;
    diag->progname = options->progname;

    struct bx_option_cursor cursor;
    bx_option_cursor_init(&cursor, argv, argc > 0 ? 1u : 0u, (size_t)argc,
                          BX_OPTION_ORDERED | BX_OPTION_ABBREVIATE, specs, sizeof(specs) / sizeof(specs[0]));
    struct bx_option_result option;
    while ((option = bx_option_next(&cursor)).event != BX_OPTION_END) {
        if (option.event == BX_OPTION_OPERAND) {
            operands[(*count)++] = option.token;
            continue;
        }
        if (option.event != BX_OPTION_MATCH) {
            if (option.event == BX_OPTION_MISSING_VALUE && option.short_name)
                bx_diag(diag, "option requires an argument -- '%c'", option.short_name);
            else if (option.event == BX_OPTION_MISSING_VALUE)
                bx_diag(diag, "option '%s' requires an argument", option.token);
            else if (option.short_name)
                bx_diag(diag, "invalid option -- '%c'", option.short_name);
            else
                bx_diag(diag, "unrecognized option '%s'", option.token);
            return false;
        }

        switch (option.spec->id) {
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
                if (!bx_du_parse_block_size(option.value, &options->output_block_size, diag)) {
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
                options->limit_depth = true;
                if (!bx_du_parse_max_depth(option.value, &options->max_depth, diag)) {
                    return false;
                }
                break;
            case BX_DU_OPT_HELP:
                options->show_help = true;
                return true;
            case BX_DU_OPT_VERSION:
                options->show_version = true;
                return true;
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

static void bx_du_format_size(uintmax_t size_bytes, const struct bx_du_options* options, char* buffer, size_t buffer_size) {
    enum bx_size_unit_label_style style = BX_SIZE_UNIT_LABEL_IEC_PREFIX;
    const char* suffixes = "KMGTPEZYRQ";

    switch (options->output_mode) {
        case BX_DU_OUTPUT_HUMAN_1024:
            break;
        case BX_DU_OUTPUT_HUMAN_1000:
            style = BX_SIZE_UNIT_LABEL_SI_LOWER_K;
            suffixes = "kMGTPEZYRQ";
            break;
        case BX_DU_OUTPUT_BLOCKS:
            {
                uintmax_t scaled_size = bx_du_ceil_div(size_bytes, options->output_block_size);
                (void)snprintf(buffer, buffer_size, "%" PRIuMAX, scaled_size);
                return;
            }
    }

    uintmax_t base = 0;
    if (!bx_size_unit_label_base_uintmax(style, &base)) {
        (void)snprintf(buffer, buffer_size, "%" PRIuMAX, size_bytes);
        return;
    }
    bx_size_format_human_ceil(size_bytes, base, suffixes, buffer, buffer_size);
}

static bool bx_du_emit_line(uintmax_t size_bytes, const char* path, const struct bx_du_options* options, struct bx_diag_ctx* diag, struct bx_line_writer* writer) {
    char size_text[64];
    bx_du_format_size(size_bytes, options, size_text, sizeof(size_text));

    if (!bx_line_writer_puts(writer, size_text)
        || !bx_line_writer_putc(writer, '\t')
        || !bx_line_writer_puts(writer, path)
        || !bx_line_writer_putc(writer, options->null_terminate ? '\0' : '\n')) {
        bx_diag(diag, "write error: %s", strerror(errno));
        return false;
    }

    return true;
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

static DIR* bx_du_open_directory(int parent_fd, const char* name, bool follow,
                                 const struct stat* expected) {
    int flags = O_RDONLY | O_DIRECTORY | (follow ? 0 : O_NOFOLLOW);
    int fd = parent_fd == AT_FDCWD ? bx_fd_open_cloexec(name, flags, 0)
                                  : bx_fd_openat_child(parent_fd, name, flags, 0);
    if (fd < 0)
        return NULL;

    struct stat opened;
    int error;
    if (fstat(fd, &opened) != 0) {
        error = errno;
    } else if (!bx_same_file(expected, &opened)) {
        error = ESTALE;
    } else {
        DIR* dir = fdopendir(fd);
        if (dir)
            return dir;
        error = errno;
    }
    bx_fd_cleanup(&fd);
    errno = error;
    return NULL;
}

static uintmax_t bx_du_walk_path(struct bx_du_context* ctx,
                                 int parent_fd,
                                 const char* name,
                                 const char* path,
                                 uintmax_t depth,
                                 dev_t root_dev,
                                 struct bx_dir_stack* dir_stack) {
    const struct bx_du_options* options = ctx->options;
    bool top_level = dir_stack == NULL;

    struct stat st;
    int status = top_level ? lstat(name, &st) : bx_fd_fstatat_child_nofollow(parent_fd, name, &st);
    if (status != 0) {
        bx_perror_path(ctx->diag, path);
        return 0u;
    }

    bool follow = S_ISLNK(st.st_mode) && bx_du_should_follow_symlink(options, top_level);
    if (follow) {
        status = top_level ? stat(name, &st) : bx_fd_fstatat_child(parent_fd, name, &st, 0);
        if (status != 0) {
            bx_perror_path(ctx->diag, path);
            return 0u;
        }
    }

    if (top_level)
        root_dev = st.st_dev;

    bool is_dir = S_ISDIR(st.st_mode);
    if (is_dir && ((!top_level && options->one_file_system && st.st_dev != root_dev) ||
                   bx_dir_stack_contains(dir_stack, &st)))
        return 0u;

    if (!options->count_links && bx_du_inode_set_mark_seen(&ctx->seen, st.st_dev, st.st_ino))
        return 0u;

    uintmax_t total_bytes = bx_du_path_usage_bytes(&st, options);

    if (is_dir) {
        struct bx_dir_stack stack_entry = {
            .dev = st.st_dev,
            .ino = st.st_ino,
            .parent = dir_stack,
        };

        DIR* dir = bx_du_open_directory(parent_fd, name, follow, &st);
        if (dir == NULL) {
            bx_perror_path(ctx->diag, path);
        }
        else {
            int fd = dirfd(dir);
            if (fd < 0)
                bx_perror_path(ctx->diag, path);
            while (fd >= 0) {
                errno = 0;
                struct dirent* entry = readdir(dir);
                if (entry == NULL) {
                    if (errno != 0)
                        bx_perror_path(ctx->diag, path);
                    break;
                }

                if (bx_path_is_dot_or_dotdot(entry->d_name)) {
                    continue;
                }

                char* child_path = bx_path_join(path, entry->d_name);
                uintmax_t child_depth = depth == UINTMAX_MAX ? UINTMAX_MAX : depth + 1u;
                uintmax_t child_total = bx_du_walk_path(ctx, fd, entry->d_name, child_path, child_depth,
                                                       root_dev, &stack_entry);
                total_bytes = bx_du_saturating_add(total_bytes, child_total);
                free(child_path);
            }

            if (closedir(dir) != 0)
                bx_perror_path(ctx->diag, path);
        }
    }

    bool should_print = top_level || (!options->summarize && (is_dir || options->all));
    bool within_max_depth = !options->limit_depth || depth <= options->max_depth;
    if (should_print && within_max_depth)
        bx_du_emit_line(total_bytes, path, options, ctx->diag, ctx->writer);

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
    const char** operands = argc > 1 ? calloc((size_t)(argc - 1), sizeof(*operands)) : NULL;
    if (argc > 1 && operands == NULL) {
        bx_diag(&diag, "memory exhausted");
        return diag.exit_status;
    }
    size_t operand_count = 0;

    if (!bx_du_parse_options(argc, argv, &options, operands, &operand_count, &diag))
        goto done;

    if (options.show_help) {
        bx_du_print_help(stdout, options.progname);
        goto done;
    }

    if (options.show_version) {
        bx_cli_print_version(options.progname);
        goto done;
    }

    struct bx_du_context ctx = {
        .options = &options,
        .diag = &diag,
    };
    char output_buffer[8192];
    struct bx_line_writer writer;
    bx_line_writer_init(&writer, STDOUT_FILENO, output_buffer, sizeof(output_buffer));
    ctx.writer = &writer;
    bx_du_inode_set_init(&ctx.seen);

    uintmax_t grand_total_bytes = 0u;

    if (operand_count == 0) {
        grand_total_bytes = bx_du_walk_path(&ctx, AT_FDCWD, ".", ".", 0, 0, NULL);
    }
    else {
        for (size_t i = 0; i < operand_count; i++) {
            uintmax_t total_bytes = bx_du_walk_path(&ctx, AT_FDCWD, operands[i], operands[i], 0, 0, NULL);
            grand_total_bytes = bx_du_saturating_add(grand_total_bytes, total_bytes);
        }
    }

    if ((!options.total || bx_du_emit_line(grand_total_bytes, "total", &options, &diag, &writer)) &&
        !bx_line_writer_flush(&writer)) {
        bx_diag(&diag, "write error: %s", strerror(errno));
    }

    bx_du_inode_set_free(&ctx.seen);
done:
    free(operands);
    return diag.exit_status;
}
