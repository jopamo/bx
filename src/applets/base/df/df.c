#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <mntent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"
#include "bx/libbx.h"
#include "lib/cli_common.h"
#include "lib/args_common.h"
#include "lib/line_writer.h"
#include "lib/size_parse.h"

enum bx_df_size_mode {
    BX_DF_SIZE_1K = 0,
    BX_DF_SIZE_HUMAN_1024,
    BX_DF_SIZE_HUMAN_1000,
};

enum bx_df_output_field {
    BX_DF_FIELD_SOURCE = 0,
    BX_DF_FIELD_FSTYPE,
    BX_DF_FIELD_ITOTAL,
    BX_DF_FIELD_IUSED,
    BX_DF_FIELD_IAVAIL,
    BX_DF_FIELD_IPCENT,
    BX_DF_FIELD_SIZE,
    BX_DF_FIELD_USED,
    BX_DF_FIELD_AVAIL,
    BX_DF_FIELD_PCENT,
    BX_DF_FIELD_FILE,
    BX_DF_FIELD_TARGET,
    BX_DF_FIELD_COUNT,
};

struct bx_df_options {
    const char* progname;
    bool show_help;
    bool show_version;
    bool posix_format;
    bool print_type;
    bool show_total;
    bool use_output;
    enum bx_df_size_mode size_mode;
    enum bx_df_output_field output_fields[BX_DF_FIELD_COUNT];
    size_t output_field_count;
    char** include_types;
    size_t include_type_count;
    char** exclude_types;
    size_t exclude_type_count;
};

struct bx_df_mount_entry {
    char* source;
    char* target;
    char* fstype;
};

struct bx_df_mount_table {
    struct bx_df_mount_entry* entries;
    size_t count;
};

struct bx_df_row {
    const char* operand;
    const char* source;
    const char* target;
    const char* fstype;
    uintmax_t block_size;
    uintmax_t total_blocks;
    uintmax_t used_blocks;
    uintmax_t avail_blocks;
    uintmax_t inode_total;
    uintmax_t inode_used;
    uintmax_t inode_avail;
    unsigned block_usage_percent;
    unsigned inode_usage_percent;
};

struct bx_df_column_set {
    enum bx_df_output_field fields[BX_DF_FIELD_COUNT];
    size_t count;
    bool custom_output;
};

struct bx_df_totals {
    uintmax_t total_bytes;
    uintmax_t used_bytes;
    uintmax_t avail_bytes;
    uintmax_t inode_total;
    uintmax_t inode_used;
    uintmax_t inode_avail;
    bool has_rows;
};

enum {
    BX_DF_OPT_HELP = 1,
    BX_DF_OPT_VERSION = 2,
    BX_DF_OPT_OUTPUT = 3,
    BX_DF_OPT_TOTAL = 4,
};

static void bx_df_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... [FILE]...\n", progname);
    fprintf(stream, "Show file system space usage for each FILE.\n");
    fprintf(stream, "With no FILE, use '.'.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -H             print sizes in powers of 1000\n");
    fprintf(stream, "  -h             print sizes in powers of 1024\n");
    fprintf(stream, "  -k             use 1K blocks (default)\n");
    fprintf(stream, "  -P             use POSIX output format\n");
    fprintf(stream, "  -T             print file system type\n");
    fprintf(stream, "  -t, --type=TYPE        limit listing to file systems of type TYPE\n");
    fprintf(stream, "  -x, --exclude-type=TYPE  limit listing to file systems not of type TYPE\n");
    fprintf(stream, "      --total            produce a grand total\n");
    fprintf(stream, "      --output[=FIELD_LIST]  use the output format defined by FIELD_LIST\n");
    fprintf(stream, "      --help     display this help and exit\n");
    fprintf(stream, "      --version  output version information and exit\n");
}

static bool bx_df_parse_output_field_name(const char* name, enum bx_df_output_field* field_out) {
    if (strcmp(name, "source") == 0) {
        *field_out = BX_DF_FIELD_SOURCE;
        return true;
    }
    if (strcmp(name, "fstype") == 0) {
        *field_out = BX_DF_FIELD_FSTYPE;
        return true;
    }
    if (strcmp(name, "itotal") == 0) {
        *field_out = BX_DF_FIELD_ITOTAL;
        return true;
    }
    if (strcmp(name, "iused") == 0) {
        *field_out = BX_DF_FIELD_IUSED;
        return true;
    }
    if (strcmp(name, "iavail") == 0) {
        *field_out = BX_DF_FIELD_IAVAIL;
        return true;
    }
    if (strcmp(name, "ipcent") == 0) {
        *field_out = BX_DF_FIELD_IPCENT;
        return true;
    }
    if (strcmp(name, "size") == 0) {
        *field_out = BX_DF_FIELD_SIZE;
        return true;
    }
    if (strcmp(name, "used") == 0) {
        *field_out = BX_DF_FIELD_USED;
        return true;
    }
    if (strcmp(name, "avail") == 0) {
        *field_out = BX_DF_FIELD_AVAIL;
        return true;
    }
    if (strcmp(name, "pcent") == 0) {
        *field_out = BX_DF_FIELD_PCENT;
        return true;
    }
    if (strcmp(name, "file") == 0) {
        *field_out = BX_DF_FIELD_FILE;
        return true;
    }
    if (strcmp(name, "target") == 0) {
        *field_out = BX_DF_FIELD_TARGET;
        return true;
    }
    return false;
}

static bool bx_df_output_field_is_selected(const struct bx_df_options* options, enum bx_df_output_field field) {
    for (size_t i = 0; i < options->output_field_count; i++) {
        if (options->output_fields[i] == field) {
            return true;
        }
    }
    return false;
}

static void bx_df_set_default_output_fields(struct bx_df_options* options) {
    static const enum bx_df_output_field defaults[] = {
        BX_DF_FIELD_SOURCE, BX_DF_FIELD_FSTYPE, BX_DF_FIELD_ITOTAL, BX_DF_FIELD_IUSED, BX_DF_FIELD_IAVAIL, BX_DF_FIELD_IPCENT,
        BX_DF_FIELD_SIZE,   BX_DF_FIELD_USED,   BX_DF_FIELD_AVAIL,  BX_DF_FIELD_PCENT, BX_DF_FIELD_FILE,   BX_DF_FIELD_TARGET,
    };

    options->output_field_count = 0;
    for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++) {
        options->output_fields[options->output_field_count++] = defaults[i];
    }
}

static bool bx_df_append_output_fields(const char* text, struct bx_df_options* options, struct bx_diag_ctx* diag) {
    char* copy = xstrdup(text);
    char* cursor = copy;

    while (true) {
        char* token = cursor;
        char* comma = strchr(cursor, ',');
        if (comma != NULL) {
            *comma = '\0';
            cursor = comma + 1;
        }

        if (token[0] == '\0') {
            bx_diag(diag, "option --output: field '%s' unknown", token);
            free(copy);
            return false;
        }

        enum bx_df_output_field field;
        if (!bx_df_parse_output_field_name(token, &field)) {
            bx_diag(diag, "option --output: field '%s' unknown", token);
            free(copy);
            return false;
        }
        if (bx_df_output_field_is_selected(options, field)) {
            bx_diag(diag, "option --output: field '%s' used more than once", token);
            free(copy);
            return false;
        }

        options->output_fields[options->output_field_count++] = field;

        if (comma == NULL) {
            break;
        }
    }

    free(copy);
    return true;
}

static void bx_df_free_type_list(char** list, size_t count) {
    for (size_t i = 0; i < count; i++) {
        free(list[i]);
    }
    free(list);
}

static bool bx_df_append_type(char*** list_out, size_t* count_out, const char* type_name, struct bx_diag_ctx* diag) {
    if (type_name == NULL || type_name[0] == '\0') {
        bx_diag(diag, "invalid file system type '%s'", type_name != NULL ? type_name : "");
        return false;
    }

    char** resized = xrealloc(*list_out, (*count_out + 1u) * sizeof(**list_out));
    *list_out = resized;
    (*list_out)[*count_out] = xstrdup(type_name);
    (*count_out)++;
    return true;
}

static void bx_df_free_options(struct bx_df_options* options) {
    if (options == NULL) {
        return;
    }

    bx_df_free_type_list(options->include_types, options->include_type_count);
    bx_df_free_type_list(options->exclude_types, options->exclude_type_count);
    options->include_types = NULL;
    options->include_type_count = 0;
    options->exclude_types = NULL;
    options->exclude_type_count = 0;
}

static bool bx_df_parse_options(int argc, char** argv, struct bx_df_options* options, int* first_operand, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"help", no_argument, NULL, BX_DF_OPT_HELP},
        {"version", no_argument, NULL, BX_DF_OPT_VERSION},
        {"output", optional_argument, NULL, BX_DF_OPT_OUTPUT},
        {"type", required_argument, NULL, 't'},
        {"exclude-type", required_argument, NULL, 'x'},
        {"total", no_argument, NULL, BX_DF_OPT_TOTAL},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "df");
    options->size_mode = BX_DF_SIZE_1K;
    diag->progname = options->progname;

    bx_args_getopt_reset();

    while (true) {
        int option_index = 0;
        int c = bx_args_getopt_long(argc, argv, "+hHkPTt:x:", long_options, &option_index);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 'h':
                options->size_mode = BX_DF_SIZE_HUMAN_1024;
                break;
            case 'H':
                options->size_mode = BX_DF_SIZE_HUMAN_1000;
                break;
            case 'k':
                options->size_mode = BX_DF_SIZE_1K;
                break;
            case 'P':
                options->posix_format = true;
                break;
            case 'T':
                options->print_type = true;
                break;
            case 't':
                if (!bx_df_append_type(&options->include_types, &options->include_type_count, optarg, diag)) {
                    return false;
                }
                break;
            case 'x':
                if (!bx_df_append_type(&options->exclude_types, &options->exclude_type_count, optarg, diag)) {
                    return false;
                }
                break;
            case BX_DF_OPT_OUTPUT:
                options->use_output = true;
                if (optarg == NULL) {
                    bx_df_set_default_output_fields(options);
                }
                else if (!bx_df_append_output_fields(optarg, options, diag)) {
                    return false;
                }
                break;
            case BX_DF_OPT_TOTAL:
                options->show_total = true;
                break;
            case BX_DF_OPT_HELP:
                options->show_help = true;
                return true;
            case BX_DF_OPT_VERSION:
                options->show_version = true;
                return true;
            case ':':
                bx_cli_diag_option_requires_arg(diag, optopt, optind, argc, argv);
                return false;
            case '?':
                bx_cli_diag_unrecognized_option(diag, optopt, optind, argc, argv);
                return false;
            default:
                return false;
        }
    }

    if (options->use_output && options->output_field_count == 0u) {
        bx_df_set_default_output_fields(options);
    }
    if (options->use_output && options->posix_format) {
        bx_diag(diag, "options -P and --output are mutually exclusive");
        return false;
    }
    if (options->use_output && options->print_type) {
        bx_diag(diag, "options -T and --output are mutually exclusive");
        return false;
    }
    if (options->include_type_count > 0u && options->exclude_type_count > 0u) {
        bx_diag(diag, "options -t and -x are mutually exclusive");
        return false;
    }

    *first_operand = optind;
    return true;
}

static uintmax_t bx_df_scale_blocks(uintmax_t blocks, uintmax_t block_size, uintmax_t divisor) {
    if (blocks == 0u || block_size == 0u || divisor == 0u) {
        return 0u;
    }

    if (blocks > (UINTMAX_MAX / block_size)) {
        return UINTMAX_MAX / divisor;
    }

    return (blocks * block_size) / divisor;
}

static uintmax_t bx_df_saturating_add(uintmax_t left, uintmax_t right) {
    if (left > UINTMAX_MAX - right) {
        return UINTMAX_MAX;
    }
    return left + right;
}

static uintmax_t bx_df_saturating_mul(uintmax_t left, uintmax_t right) {
    if (left == 0u || right == 0u) {
        return 0u;
    }
    if (left > UINTMAX_MAX / right) {
        return UINTMAX_MAX;
    }
    return left * right;
}

static unsigned bx_df_usage_percent(uintmax_t used, uintmax_t available) {
    uintmax_t denominator = used + available;
    if (denominator < used) {
        denominator = UINTMAX_MAX;
    }
    if (denominator == 0u) {
        return 0u;
    }

    if (used > (UINTMAX_MAX / 100u)) {
        return 100u;
    }

    uintmax_t scaled = used * 100u;
    uintmax_t percent = scaled / denominator;
    if ((scaled % denominator) != 0u && percent < UINTMAX_MAX) {
        percent++;
    }
    if (percent > 100u) {
        percent = 100u;
    }

    return (unsigned)percent;
}

static void bx_df_format_blocks(uintmax_t blocks, uintmax_t block_size, enum bx_df_size_mode mode, char* buffer, size_t buffer_size) {
    if (mode == BX_DF_SIZE_1K) {
        uintmax_t value = bx_df_scale_blocks(blocks, block_size, 1024u);
        (void)snprintf(buffer, buffer_size, "%" PRIuMAX, value);
        return;
    }

    uintmax_t bytes = bx_df_scale_blocks(blocks, block_size, 1u);
    enum bx_size_unit_label_style style = (mode == BX_DF_SIZE_HUMAN_1000) ? BX_SIZE_UNIT_LABEL_SI_UPPER_K : BX_SIZE_UNIT_LABEL_IEC_PREFIX;
    uintmax_t base = 0;
    if (!bx_size_unit_label_base_uintmax(style, &base)) {
        (void)snprintf(buffer, buffer_size, "%" PRIuMAX, bytes);
        return;
    }
    bx_size_format_human_round(bytes, base, "BKMGTPEZY", false, buffer, buffer_size);
}

static int bx_df_octal_digit(char c) {
    if (c >= '0' && c <= '7') {
        return c - '0';
    }
    return -1;
}

static char* bx_df_unescape_mount_field(const char* text) {
    size_t len = text != NULL ? strlen(text) : 0u;
    char* out = xmalloc(len + 1u);
    size_t out_pos = 0;

    for (size_t i = 0; i < len;) {
        if (text[i] == '\\' && i + 3u < len) {
            int d1 = bx_df_octal_digit(text[i + 1u]);
            int d2 = bx_df_octal_digit(text[i + 2u]);
            int d3 = bx_df_octal_digit(text[i + 3u]);
            if (d1 >= 0 && d2 >= 0 && d3 >= 0) {
                out[out_pos++] = (char)((d1 << 6) | (d2 << 3) | d3);
                i += 4u;
                continue;
            }
        }

        out[out_pos++] = text[i++];
    }

    out[out_pos] = '\0';
    return out;
}

static void bx_df_strip_trailing_slashes(char* path) {
    size_t len = strlen(path);
    while (len > 1u && path[len - 1u] == '/') {
        path[len - 1u] = '\0';
        len--;
    }
}

static void bx_df_free_mount_table(struct bx_df_mount_table* table) {
    for (size_t i = 0; i < table->count; i++) {
        free(table->entries[i].source);
        free(table->entries[i].target);
        free(table->entries[i].fstype);
    }
    free(table->entries);
    table->entries = NULL;
    table->count = 0;
}

static void bx_df_load_mount_table(struct bx_df_mount_table* table) {
    memset(table, 0, sizeof(*table));

    FILE* fp = setmntent("/proc/self/mounts", "r");
    if (fp == NULL) {
        fp = setmntent("/etc/mtab", "r");
    }
    if (fp == NULL) {
        return;
    }

    struct mntent* ent;
    while ((ent = getmntent(fp)) != NULL) {
        char* source = bx_df_unescape_mount_field(ent->mnt_fsname);
        char* target = bx_df_unescape_mount_field(ent->mnt_dir);
        char* fstype = bx_df_unescape_mount_field(ent->mnt_type);
        bx_df_strip_trailing_slashes(target);

        struct bx_df_mount_entry* resized = xrealloc(table->entries, (table->count + 1u) * sizeof(*table->entries));
        table->entries = resized;
        table->entries[table->count].source = source;
        table->entries[table->count].target = target;
        table->entries[table->count].fstype = fstype;
        table->count++;
    }

    (void)endmntent(fp);
}

static bool bx_df_mount_matches_path(const char* mount_target, const char* path) {
    if (mount_target == NULL || path == NULL || mount_target[0] == '\0' || path[0] == '\0') {
        return false;
    }

    if (strcmp(mount_target, "/") == 0) {
        return path[0] == '/';
    }

    size_t mount_len = strlen(mount_target);
    if (strncmp(path, mount_target, mount_len) != 0) {
        return false;
    }

    return path[mount_len] == '\0' || path[mount_len] == '/';
}

static const struct bx_df_mount_entry* bx_df_find_mount_for_path(const struct bx_df_mount_table* table, const char* path) {
    const struct bx_df_mount_entry* best = NULL;
    size_t best_len = 0;

    for (size_t i = 0; i < table->count; i++) {
        const struct bx_df_mount_entry* entry = &table->entries[i];
        if (!bx_df_mount_matches_path(entry->target, path)) {
            continue;
        }

        size_t target_len = strlen(entry->target);
        if (best == NULL || target_len > best_len) {
            best = entry;
            best_len = target_len;
        }
    }

    return best;
}

static char* bx_df_path_for_mount_lookup(const char* path) {
    if (path == NULL || path[0] == '\0') {
        return xstrdup("");
    }
    if (path[0] == '/') {
        return xstrdup(path);
    }

    size_t cwd_size = 128u;
    while (true) {
        char* cwd = xmalloc(cwd_size);
        if (getcwd(cwd, cwd_size) != NULL) {
            size_t cwd_len = strlen(cwd);
            size_t path_len = strlen(path);
            bool root = (cwd_len == 1u && cwd[0] == '/');
            size_t out_len = cwd_len + (root ? 0u : 1u) + path_len;
            char* out = xmalloc(out_len + 1u);

            if (root) {
                (void)snprintf(out, out_len + 1u, "/%s", path);
            }
            else {
                (void)snprintf(out, out_len + 1u, "%s/%s", cwd, path);
            }
            free(cwd);
            return out;
        }

        free(cwd);
        if (errno != ERANGE) {
            break;
        }

        if (cwd_size > (SIZE_MAX / 2u)) {
            break;
        }
        cwd_size *= 2u;
    }

    return xstrdup(path);
}

static bool bx_df_populate_row(const char* path, const struct bx_df_mount_table* mount_table, struct bx_df_row* row, struct bx_diag_ctx* diag) {
    struct statvfs fs;
    if (statvfs(path, &fs) != 0) {
        bx_perror_path(diag, path);
        return false;
    }

    memset(row, 0, sizeof(*row));
    row->operand = path;
    row->source = path;
    row->target = path;
    row->fstype = "-";

    row->block_size = (fs.f_frsize != 0u) ? (uintmax_t)fs.f_frsize : (uintmax_t)fs.f_bsize;
    if (row->block_size == 0u) {
        row->block_size = 1024u;
    }

    row->total_blocks = (uintmax_t)fs.f_blocks;
    uintmax_t free_blocks = (uintmax_t)fs.f_bfree;
    row->avail_blocks = (uintmax_t)fs.f_bavail;
    row->used_blocks = (row->total_blocks > free_blocks) ? (row->total_blocks - free_blocks) : 0u;

    uintmax_t used_1k = bx_df_scale_blocks(row->used_blocks, row->block_size, 1024u);
    uintmax_t avail_1k = bx_df_scale_blocks(row->avail_blocks, row->block_size, 1024u);
    row->block_usage_percent = bx_df_usage_percent(used_1k, avail_1k);

    row->inode_total = (uintmax_t)fs.f_files;
    uintmax_t inode_free = (uintmax_t)fs.f_ffree;
    row->inode_avail = (uintmax_t)fs.f_favail;
    row->inode_used = (row->inode_total > inode_free) ? (row->inode_total - inode_free) : 0u;
    row->inode_usage_percent = bx_df_usage_percent(row->inode_used, row->inode_avail);

    char* lookup_path = bx_df_path_for_mount_lookup(path);
    const struct bx_df_mount_entry* mount = bx_df_find_mount_for_path(mount_table, lookup_path);
    if (mount != NULL) {
        row->source = mount->source;
        row->target = mount->target;
        row->fstype = mount->fstype;
    }
    free(lookup_path);

    return true;
}

static void bx_df_build_columns(const struct bx_df_options* options, struct bx_df_column_set* columns) {
    memset(columns, 0, sizeof(*columns));

    if (options->use_output) {
        columns->custom_output = true;
        columns->count = options->output_field_count;
        for (size_t i = 0; i < options->output_field_count; i++) {
            columns->fields[i] = options->output_fields[i];
        }
        return;
    }

    columns->custom_output = false;
    columns->fields[columns->count++] = BX_DF_FIELD_SOURCE;
    if (options->print_type) {
        columns->fields[columns->count++] = BX_DF_FIELD_FSTYPE;
    }
    columns->fields[columns->count++] = BX_DF_FIELD_SIZE;
    columns->fields[columns->count++] = BX_DF_FIELD_USED;
    columns->fields[columns->count++] = BX_DF_FIELD_AVAIL;
    columns->fields[columns->count++] = BX_DF_FIELD_PCENT;
    columns->fields[columns->count++] = BX_DF_FIELD_TARGET;
}

static const char* bx_df_column_label(enum bx_df_output_field field, const struct bx_df_options* options, bool custom_output) {
    switch (field) {
        case BX_DF_FIELD_SOURCE:
            return "Filesystem";
        case BX_DF_FIELD_FSTYPE:
            return "Type";
        case BX_DF_FIELD_ITOTAL:
            return "Inodes";
        case BX_DF_FIELD_IUSED:
            return "IUsed";
        case BX_DF_FIELD_IAVAIL:
            return "IFree";
        case BX_DF_FIELD_IPCENT:
            return "IUse%";
        case BX_DF_FIELD_SIZE:
            if (options->size_mode == BX_DF_SIZE_1K) {
                if (!custom_output && options->posix_format) {
                    return "1024-blocks";
                }
                return "1K-blocks";
            }
            return "Size";
        case BX_DF_FIELD_USED:
            return "Used";
        case BX_DF_FIELD_AVAIL:
            return custom_output ? "Avail" : "Available";
        case BX_DF_FIELD_PCENT:
            if (!custom_output && options->posix_format && options->size_mode == BX_DF_SIZE_1K) {
                return "Capacity";
            }
            return "Use%";
        case BX_DF_FIELD_FILE:
            return "File";
        case BX_DF_FIELD_TARGET:
            return "Mounted on";
        case BX_DF_FIELD_COUNT:
            break;
    }

    return "";
}

static bool bx_df_field_left_aligned(enum bx_df_output_field field) {
    switch (field) {
        case BX_DF_FIELD_SOURCE:
        case BX_DF_FIELD_FSTYPE:
        case BX_DF_FIELD_FILE:
        case BX_DF_FIELD_TARGET:
            return true;
        case BX_DF_FIELD_ITOTAL:
        case BX_DF_FIELD_IUSED:
        case BX_DF_FIELD_IAVAIL:
        case BX_DF_FIELD_IPCENT:
        case BX_DF_FIELD_SIZE:
        case BX_DF_FIELD_USED:
        case BX_DF_FIELD_AVAIL:
        case BX_DF_FIELD_PCENT:
        case BX_DF_FIELD_COUNT:
            break;
    }
    return false;
}

static int bx_df_field_base_width(enum bx_df_output_field field, bool custom_output) {
    switch (field) {
        case BX_DF_FIELD_SOURCE:
            return 20;
        case BX_DF_FIELD_FSTYPE:
            return 6;
        case BX_DF_FIELD_ITOTAL:
        case BX_DF_FIELD_IUSED:
        case BX_DF_FIELD_IAVAIL:
        case BX_DF_FIELD_USED:
        case BX_DF_FIELD_AVAIL:
            return 10;
        case BX_DF_FIELD_SIZE:
            return 11;
        case BX_DF_FIELD_IPCENT:
            return 5;
        case BX_DF_FIELD_PCENT:
            return custom_output ? 5 : 8;
        case BX_DF_FIELD_FILE:
            return custom_output ? 4 : 0;
        case BX_DF_FIELD_TARGET:
        case BX_DF_FIELD_COUNT:
            break;
    }
    return 0;
}

static bool bx_df_write_error(struct bx_diag_ctx* diag) {
    int saved_errno = errno != 0 ? errno : EIO;
    bx_diag(diag, "write error: %s", strerror(saved_errno));
    errno = saved_errno;
    return false;
}

static bool bx_df_write(struct bx_line_writer* writer, const void* data, size_t length, struct bx_diag_ctx* diag) {
    if (!bx_line_writer_write(writer, data, length)) {
        return bx_df_write_error(diag);
    }
    return true;
}

static bool bx_df_putc(struct bx_line_writer* writer, char ch, struct bx_diag_ctx* diag) {
    return bx_df_write(writer, &ch, 1u, diag);
}

static bool bx_df_puts(struct bx_line_writer* writer, const char* text, struct bx_diag_ctx* diag) {
    return bx_df_write(writer, text, strlen(text), diag);
}

static bool bx_df_write_spaces(struct bx_line_writer* writer, size_t count, struct bx_diag_ctx* diag) {
    static const char spaces[64] = {
        ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
        ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
        ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
        ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
    };

    while (count > 0u) {
        size_t chunk = count > sizeof(spaces) ? sizeof(spaces) : count;
        if (!bx_df_write(writer, spaces, chunk, diag)) {
            return false;
        }
        count -= chunk;
    }
    return true;
}

static bool bx_df_emit_cell(struct bx_line_writer* writer, const char* text, enum bx_df_output_field field, bool custom_output, bool first_column, struct bx_diag_ctx* diag) {
    const char* value = text != NULL ? text : "";
    int width = bx_df_field_base_width(field, custom_output);
    int text_width = (int)strlen(value);
    if (text_width > width) {
        width = text_width;
    }

    bool left = bx_df_field_left_aligned(field);
    if (!first_column && !bx_df_putc(writer, ' ', diag)) {
        return false;
    }

    if (width > 0) {
        size_t pad = (size_t)(width - text_width);
        if (!left && !bx_df_write_spaces(writer, pad, diag)) {
            return false;
        }
        if (!bx_df_puts(writer, value, diag)) {
            return false;
        }
        if (left && !bx_df_write_spaces(writer, pad, diag)) {
            return false;
        }
        return true;
    }

    if (!bx_df_puts(writer, value, diag)) {
        return false;
    }

    return true;
}

static bool bx_df_emit_header(struct bx_line_writer* writer, const struct bx_df_column_set* columns, const struct bx_df_options* options, struct bx_diag_ctx* diag) {
    bool first = true;
    for (size_t i = 0; i < columns->count; i++) {
        enum bx_df_output_field field = columns->fields[i];
        const char* label = bx_df_column_label(field, options, columns->custom_output);
        if (!bx_df_emit_cell(writer, label, field, columns->custom_output, first, diag)) {
            return false;
        }
        first = false;
    }

    return bx_df_putc(writer, '\n', diag);
}

static const char* bx_df_row_value(enum bx_df_output_field field, const struct bx_df_row* row, const struct bx_df_options* options, char* buffer, size_t buffer_size) {
    switch (field) {
        case BX_DF_FIELD_SOURCE:
            return row->source;
        case BX_DF_FIELD_FSTYPE:
            return row->fstype;
        case BX_DF_FIELD_ITOTAL:
            (void)snprintf(buffer, buffer_size, "%" PRIuMAX, row->inode_total);
            return buffer;
        case BX_DF_FIELD_IUSED:
            (void)snprintf(buffer, buffer_size, "%" PRIuMAX, row->inode_used);
            return buffer;
        case BX_DF_FIELD_IAVAIL:
            (void)snprintf(buffer, buffer_size, "%" PRIuMAX, row->inode_avail);
            return buffer;
        case BX_DF_FIELD_IPCENT:
            (void)snprintf(buffer, buffer_size, "%u%%", row->inode_usage_percent);
            return buffer;
        case BX_DF_FIELD_SIZE:
            bx_df_format_blocks(row->total_blocks, row->block_size, options->size_mode, buffer, buffer_size);
            return buffer;
        case BX_DF_FIELD_USED:
            bx_df_format_blocks(row->used_blocks, row->block_size, options->size_mode, buffer, buffer_size);
            return buffer;
        case BX_DF_FIELD_AVAIL:
            bx_df_format_blocks(row->avail_blocks, row->block_size, options->size_mode, buffer, buffer_size);
            return buffer;
        case BX_DF_FIELD_PCENT:
            (void)snprintf(buffer, buffer_size, "%u%%", row->block_usage_percent);
            return buffer;
        case BX_DF_FIELD_FILE:
            return row->operand;
        case BX_DF_FIELD_TARGET:
            return row->target;
        case BX_DF_FIELD_COUNT:
            break;
    }

    return "";
}

static bool bx_df_emit_row(struct bx_line_writer* writer, const struct bx_df_row* row, const struct bx_df_column_set* columns, const struct bx_df_options* options, struct bx_diag_ctx* diag) {
    bool first = true;
    for (size_t i = 0; i < columns->count; i++) {
        enum bx_df_output_field field = columns->fields[i];
        char buffer[64];
        const char* value = bx_df_row_value(field, row, options, buffer, sizeof(buffer));
        if (!bx_df_emit_cell(writer, value, field, columns->custom_output, first, diag)) {
            return false;
        }
        first = false;
    }

    return bx_df_putc(writer, '\n', diag);
}

static bool bx_df_type_list_contains(char* const* list, size_t count, const char* type_name) {
    const char* candidate = (type_name != NULL) ? type_name : "";
    for (size_t i = 0; i < count; i++) {
        if (strcmp(list[i], candidate) == 0) {
            return true;
        }
    }
    return false;
}

static bool bx_df_row_selected(const struct bx_df_options* options, const struct bx_df_row* row) {
    if (options->include_type_count > 0u && !bx_df_type_list_contains(options->include_types, options->include_type_count, row->fstype)) {
        return false;
    }
    if (options->exclude_type_count > 0u && bx_df_type_list_contains(options->exclude_types, options->exclude_type_count, row->fstype)) {
        return false;
    }
    return true;
}

static void bx_df_add_row_to_totals(struct bx_df_totals* totals, const struct bx_df_row* row) {
    uintmax_t total_bytes = bx_df_saturating_mul(row->total_blocks, row->block_size);
    uintmax_t used_bytes = bx_df_saturating_mul(row->used_blocks, row->block_size);
    uintmax_t avail_bytes = bx_df_saturating_mul(row->avail_blocks, row->block_size);

    totals->total_bytes = bx_df_saturating_add(totals->total_bytes, total_bytes);
    totals->used_bytes = bx_df_saturating_add(totals->used_bytes, used_bytes);
    totals->avail_bytes = bx_df_saturating_add(totals->avail_bytes, avail_bytes);
    totals->inode_total = bx_df_saturating_add(totals->inode_total, row->inode_total);
    totals->inode_used = bx_df_saturating_add(totals->inode_used, row->inode_used);
    totals->inode_avail = bx_df_saturating_add(totals->inode_avail, row->inode_avail);
    totals->has_rows = true;
}

static void bx_df_totals_to_row(const struct bx_df_totals* totals, struct bx_df_row* row) {
    memset(row, 0, sizeof(*row));
    row->operand = "total";
    row->source = "total";
    row->target = "-";
    row->fstype = "-";
    row->block_size = 1u;
    row->total_blocks = totals->total_bytes;
    row->used_blocks = totals->used_bytes;
    row->avail_blocks = totals->avail_bytes;
    row->inode_total = totals->inode_total;
    row->inode_used = totals->inode_used;
    row->inode_avail = totals->inode_avail;
    row->block_usage_percent = bx_df_usage_percent(totals->used_bytes, totals->avail_bytes);
    row->inode_usage_percent = bx_df_usage_percent(totals->inode_used, totals->inode_avail);
}

static bool bx_df_process_operand(const char* path,
                                  const struct bx_df_options* options,
                                  const struct bx_df_mount_table* mount_table,
                                  const struct bx_df_column_set* columns,
                                  struct bx_df_totals* totals,
                                  struct bx_line_writer* writer,
                                  struct bx_diag_ctx* diag) {
    struct bx_df_row row;
    if (!bx_df_populate_row(path, mount_table, &row, diag)) {
        return true;
    }

    if (!bx_df_row_selected(options, &row)) {
        return true;
    }

    if (totals != NULL) {
        bx_df_add_row_to_totals(totals, &row);
    }

    return bx_df_emit_row(writer, &row, columns, options, diag);
}

int bx_df_main(int argc, char** argv) {
    struct bx_df_options options;
    struct bx_df_mount_table mount_table;
    struct bx_df_column_set columns;
    struct bx_df_totals totals = {0};
    struct bx_diag_ctx diag = {
        .progname = "df",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };
    int first_operand = 0;
    int rc = 0;

    if (!bx_df_parse_options(argc, argv, &options, &first_operand, &diag)) {
        bx_df_free_options(&options);
        return diag.exit_status != 0 ? diag.exit_status : 1;
    }

    if (options.show_help) {
        bx_df_print_help(stdout, options.progname);
        bx_df_free_options(&options);
        return 0;
    }

    if (options.show_version) {
        bx_cli_print_version(options.progname);
        bx_df_free_options(&options);
        return 0;
    }

    bx_df_load_mount_table(&mount_table);
    bx_df_build_columns(&options, &columns);

    char output_buffer[8192];
    struct bx_line_writer writer;
    bx_line_writer_init(&writer, STDOUT_FILENO, output_buffer, sizeof(output_buffer));

    if (!bx_df_emit_header(&writer, &columns, &options, &diag)) {
        rc = diag.exit_status;
        goto out;
    }

    int operand_count = argc - first_operand;
    if (operand_count <= 0) {
        if (!bx_df_process_operand(".", &options, &mount_table, &columns, &totals, &writer, &diag)) {
            rc = diag.exit_status;
            goto out;
        }
    }
    else {
        for (int i = first_operand; i < argc; i++) {
            if (!bx_df_process_operand(argv[i], &options, &mount_table, &columns, &totals, &writer, &diag)) {
                rc = diag.exit_status;
                goto out;
            }
        }
    }

    if (!totals.has_rows && diag.exit_status == 0) {
        bx_diag(&diag, "no file systems processed");
    }

    if (options.show_total && totals.has_rows) {
        struct bx_df_row total_row;
        bx_df_totals_to_row(&totals, &total_row);
        if (!bx_df_emit_row(&writer, &total_row, &columns, &options, &diag)) {
            rc = diag.exit_status;
            goto out;
        }
    }

    if (bx_line_writer_error(&writer) == 0 && !bx_line_writer_flush(&writer)) {
        bx_df_write_error(&diag);
    }
    rc = diag.exit_status;

out:
    bx_df_free_mount_table(&mount_table);
    bx_df_free_options(&options);
    return rc;
}
