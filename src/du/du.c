#include <dirent.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "applets.h"
#include "diag.h"
#include "libbx.h"

struct bx_du_options {
    const char* progname;
    bool all;
    bool summarize;
    bool total;
    bool human_readable;
    bool limit_depth;
    uintmax_t max_depth;
    bool show_help;
    bool show_version;
};

enum {
    BX_DU_OPT_HELP = 1,
    BX_DU_OPT_VERSION = 2,
    BX_DU_OPT_MAX_DEPTH = 3,
};

static const char* bx_du_progname(const char* argv0) {
    if (argv0 == NULL || argv0[0] == '\0') {
        return "du";
    }

    const char* base = strrchr(argv0, '/');
    if (base != NULL && base[1] != '\0') {
        return base + 1;
    }
    return argv0;
}

static void bx_du_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... [FILE]...\n", progname);
    fprintf(stream, "Summarize device usage for each FILE, recursively for directories.\n");
    fprintf(stream, "With no FILE, use '.'.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -a, --all             write counts for all files, not just directories\n");
    fprintf(stream, "  -c, --total           produce a grand total\n");
    fprintf(stream, "  -h, --human-readable  print sizes in human readable format (e.g., 1.0K)\n");
    fprintf(stream, "  -k                    use 1K blocks (default)\n");
    fprintf(stream, "      --max-depth=N     print the total for a directory only if it is N or fewer levels below arguments\n");
    fprintf(stream, "  -s, --summarize       display only a total for each argument\n");
    fprintf(stream, "      --help            display this help and exit\n");
    fprintf(stream, "      --version         output version information and exit\n");
}

static void bx_du_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
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

static bool bx_du_parse_options(int argc, char** argv, struct bx_du_options* options, int* first_operand, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"all", no_argument, NULL, 'a'},
        {"summarize", no_argument, NULL, 's'},
        {"total", no_argument, NULL, 'c'},
        {"human-readable", no_argument, NULL, 'h'},
        {"max-depth", required_argument, NULL, BX_DU_OPT_MAX_DEPTH},
        {"help", no_argument, NULL, BX_DU_OPT_HELP},
        {"version", no_argument, NULL, BX_DU_OPT_VERSION},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_du_progname((argc > 0) ? argv[0] : NULL);
    diag->progname = options->progname;

    opterr = 0;
    optind = 1;

    while (true) {
        int option_index = 0;
        int c = getopt_long(argc, argv, "+acshk", long_options, &option_index);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 'a':
                options->all = true;
                break;
            case 'c':
                options->total = true;
                break;
            case 'h':
                options->human_readable = true;
                break;
            case 'k':
                break;
            case 's':
                options->summarize = true;
                break;
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

    *first_operand = optind;
    return true;
}

static uintmax_t bx_du_blocks_1k(const struct stat* st) {
    if (st->st_blocks <= 0) {
        return 0u;
    }

    uintmax_t blocks_512 = (uintmax_t)st->st_blocks;
    return (blocks_512 / 2u) + ((blocks_512 % 2u) != 0u ? 1u : 0u);
}

static uintmax_t bx_du_saturating_add(uintmax_t lhs, uintmax_t rhs) {
    if (UINTMAX_MAX - lhs < rhs) {
        return UINTMAX_MAX;
    }
    return lhs + rhs;
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

static void bx_du_format_human_size(uintmax_t blocks_1k, char* buffer, size_t buffer_size) {
    static const char suffixes[] = "KMGTPEZY";

    if (blocks_1k == 0u) {
        (void)snprintf(buffer, buffer_size, "0");
        return;
    }

    double value = (double)blocks_1k;
    size_t suffix_index = 0u;
    while (value >= 1024.0 && suffix_index + 1u < (sizeof(suffixes) - 1u)) {
        value /= 1024.0;
        suffix_index++;
    }

    if (value >= 10.0) {
        (void)snprintf(buffer, buffer_size, "%.0f%c", value, suffixes[suffix_index]);
    }
    else {
        (void)snprintf(buffer, buffer_size, "%.1f%c", value, suffixes[suffix_index]);
    }
}

static void bx_du_format_size(uintmax_t blocks_1k, const struct bx_du_options* options, char* buffer, size_t buffer_size) {
    if (options->human_readable) {
        bx_du_format_human_size(blocks_1k, buffer, buffer_size);
        return;
    }

    (void)snprintf(buffer, buffer_size, "%" PRIuMAX, blocks_1k);
}

static bool bx_du_emit_line(uintmax_t blocks_1k, const char* path, const struct bx_du_options* options, struct bx_diag_ctx* diag) {
    char size_text[64];
    bx_du_format_size(blocks_1k, options, size_text, sizeof(size_text));

    if (fprintf(stdout, "%s\t%s\n", size_text, path) < 0) {
        bx_diag(diag, "write error: %s", strerror(errno));
        return false;
    }

    return true;
}

static bool bx_du_is_dot_or_dotdot(const char* name) {
    return (name[0] == '.' && name[1] == '\0') || (name[0] == '.' && name[1] == '.' && name[2] == '\0');
}

static uintmax_t bx_du_walk_path(const char* path, bool top_level, uintmax_t depth, const struct bx_du_options* options, struct bx_diag_ctx* diag, bool* ok_out) {
    struct stat st;
    if (lstat(path, &st) != 0) {
        bx_perror_path(diag, path);
        *ok_out = false;
        return 0u;
    }

    bool ok = true;
    uintmax_t total_1k = bx_du_blocks_1k(&st);

    if (S_ISDIR(st.st_mode)) {
        DIR* dir = opendir(path);
        if (dir == NULL) {
            bx_perror_path(diag, path);
            ok = false;
        }
        else {
            while (true) {
                errno = 0;
                struct dirent* entry = readdir(dir);
                if (entry == NULL) {
                    if (errno != 0) {
                        bx_perror_path(diag, path);
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
                uintmax_t child_total = bx_du_walk_path(child_path, false, child_depth, options, diag, &child_ok);
                total_1k = bx_du_saturating_add(total_1k, child_total);
                if (!child_ok) {
                    ok = false;
                }
                free(child_path);
            }

            if (closedir(dir) != 0) {
                bx_perror_path(diag, path);
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
    if (should_print && within_max_depth && !bx_du_emit_line(total_1k, path, options, diag)) {
        ok = false;
    }

    *ok_out = ok;
    return total_1k;
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
        bx_du_print_version(options.progname);
        return 0;
    }

    uintmax_t grand_total_1k = 0u;

    if (first_operand >= argc) {
        bool operand_ok = true;
        uintmax_t total_1k = bx_du_walk_path(".", true, 0u, &options, &diag, &operand_ok);
        grand_total_1k = bx_du_saturating_add(grand_total_1k, total_1k);
    }
    else {
        for (int i = first_operand; i < argc; i++) {
            bool operand_ok = true;
            uintmax_t total_1k = bx_du_walk_path(argv[i], true, 0u, &options, &diag, &operand_ok);
            grand_total_1k = bx_du_saturating_add(grand_total_1k, total_1k);
        }
    }

    if (options.total && !bx_du_emit_line(grand_total_1k, "total", &options, &diag)) {
        return diag.exit_status != 0 ? diag.exit_status : 1;
    }

    if (fflush(stdout) == EOF) {
        bx_diag(&diag, "write error: %s", strerror(errno));
    }

    return diag.exit_status;
}
