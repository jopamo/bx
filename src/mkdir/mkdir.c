#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "applets.h"
#include "diag.h"
#include "libbx.h"

#ifdef S_ISVTX
#define BX_MKDIR_STICKY_BIT S_ISVTX
#else
#define BX_MKDIR_STICKY_BIT 01000
#endif

struct bx_mkdir_options {
    const char* progname;
    bool mode_set;
    mode_t mode;
    bool parents;
    bool verbose;
    bool show_help;
    bool show_version;
};

static const char* bx_mkdir_progname(const char* argv0) {
    if (argv0 == NULL || argv0[0] == '\0') {
        return "mkdir";
    }

    const char* base = strrchr(argv0, '/');
    if (base != NULL && base[1] != '\0') {
        return base + 1;
    }
    return argv0;
}

static void bx_mkdir_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... DIRECTORY...\n", progname);
    fprintf(stream, "Create the DIRECTORY(ies), if they do not already exist.\n");
    fprintf(stream, "\n");
    fprintf(stream, "Mandatory arguments to long options are mandatory for short options too.\n");
    fprintf(stream, "  -m, --mode=MODE\n");
    fprintf(stream, "         set file mode (as in chmod), not a=rwx - umask\n");
    fprintf(stream, "  -p, --parents\n");
    fprintf(stream, "         no error if existing, make parent directories as needed,\n");
    fprintf(stream, "         with their file modes unaffected by any -m option\n");
    fprintf(stream, "  -v, --verbose\n");
    fprintf(stream, "         print a message for each created directory\n");
    fprintf(stream, "      --help\n");
    fprintf(stream, "         display this help and exit\n");
    fprintf(stream, "      --version\n");
    fprintf(stream, "         output version information and exit\n");
}

static void bx_mkdir_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

enum {
    BX_MKDIR_WHO_U = 1u << 0,
    BX_MKDIR_WHO_G = 1u << 1,
    BX_MKDIR_WHO_O = 1u << 2,
    BX_MKDIR_WHO_ALL = BX_MKDIR_WHO_U | BX_MKDIR_WHO_G | BX_MKDIR_WHO_O,
};

static mode_t bx_mkdir_rwx_mask_from_who(unsigned int who_flags) {
    mode_t mask = 0u;
    if ((who_flags & BX_MKDIR_WHO_U) != 0u) {
        mask |= S_IRWXU;
    }
    if ((who_flags & BX_MKDIR_WHO_G) != 0u) {
        mask |= S_IRWXG;
    }
    if ((who_flags & BX_MKDIR_WHO_O) != 0u) {
        mask |= S_IRWXO;
    }
    return mask;
}

static mode_t bx_mkdir_special_mask_from_who(unsigned int who_flags) {
    mode_t mask = 0u;
    if ((who_flags & BX_MKDIR_WHO_U) != 0u) {
        mask |= S_ISUID;
    }
    if ((who_flags & BX_MKDIR_WHO_G) != 0u) {
        mask |= S_ISGID;
    }
    if ((who_flags & BX_MKDIR_WHO_O) != 0u) {
        mask |= BX_MKDIR_STICKY_BIT;
    }
    return mask;
}

static mode_t bx_mkdir_perm_bits_for_who(unsigned int who_flags, char perm) {
    mode_t bits = 0u;

    if (perm == 'r') {
        if ((who_flags & BX_MKDIR_WHO_U) != 0u) {
            bits |= S_IRUSR;
        }
        if ((who_flags & BX_MKDIR_WHO_G) != 0u) {
            bits |= S_IRGRP;
        }
        if ((who_flags & BX_MKDIR_WHO_O) != 0u) {
            bits |= S_IROTH;
        }
    }
    else if (perm == 'w') {
        if ((who_flags & BX_MKDIR_WHO_U) != 0u) {
            bits |= S_IWUSR;
        }
        if ((who_flags & BX_MKDIR_WHO_G) != 0u) {
            bits |= S_IWGRP;
        }
        if ((who_flags & BX_MKDIR_WHO_O) != 0u) {
            bits |= S_IWOTH;
        }
    }
    else if (perm == 'x') {
        if ((who_flags & BX_MKDIR_WHO_U) != 0u) {
            bits |= S_IXUSR;
        }
        if ((who_flags & BX_MKDIR_WHO_G) != 0u) {
            bits |= S_IXGRP;
        }
        if ((who_flags & BX_MKDIR_WHO_O) != 0u) {
            bits |= S_IXOTH;
        }
    }

    return bits;
}

static mode_t bx_mkdir_copy_perm_bits(mode_t mode, unsigned int who_flags, char source_class) {
    mode_t source = 0u;

    switch (source_class) {
        case 'u':
            source = (mode & S_IRWXU) >> 6;
            break;
        case 'g':
            source = (mode & S_IRWXG) >> 3;
            break;
        case 'o':
            source = mode & S_IRWXO;
            break;
        default:
            return 0u;
    }

    mode_t bits = 0u;
    if ((who_flags & BX_MKDIR_WHO_U) != 0u) {
        bits |= source << 6;
    }
    if ((who_flags & BX_MKDIR_WHO_G) != 0u) {
        bits |= source << 3;
    }
    if ((who_flags & BX_MKDIR_WHO_O) != 0u) {
        bits |= source;
    }
    return bits;
}

static bool bx_mkdir_mode_is_octal(const char* text) {
    if (text == NULL || text[0] == '\0') {
        return false;
    }

    for (const char* p = text; *p != '\0'; p++) {
        if (*p < '0' || *p > '7') {
            return false;
        }
    }
    return true;
}

static bool bx_mkdir_parse_numeric_mode(const char* text, mode_t* mode_out) {
    errno = 0;
    char* end = NULL;
    unsigned long value = strtoul(text, &end, 8);
    if (errno == ERANGE || end == text || end == NULL || end[0] != '\0' || value > 07777ul) {
        return false;
    }

    *mode_out = (mode_t)value;
    return true;
}

static bool bx_mkdir_parse_symbolic_mode(const char* text, mode_t* mode_out) {
    mode_t mode = 0777u;
    const mode_t current_umask = umask(0u);
    umask(current_umask);

    const char* p = text;
    while (*p != '\0') {
        unsigned int who_flags = 0u;
        bool who_specified = false;

        while (*p == 'u' || *p == 'g' || *p == 'o' || *p == 'a') {
            who_specified = true;
            if (*p == 'u') {
                who_flags |= BX_MKDIR_WHO_U;
            }
            else if (*p == 'g') {
                who_flags |= BX_MKDIR_WHO_G;
            }
            else if (*p == 'o') {
                who_flags |= BX_MKDIR_WHO_O;
            }
            else {
                who_flags |= BX_MKDIR_WHO_ALL;
            }
            p++;
        }

        if (!who_specified) {
            who_flags = BX_MKDIR_WHO_ALL;
        }

        char op = *p;
        if (op != '+' && op != '-' && op != '=') {
            return false;
        }
        p++;

        if (*p == '\0' || *p == ',') {
            return false;
        }

        mode_t clause_rwx_bits = 0u;
        mode_t clause_special_bits = 0u;
        mode_t source_mode = mode;
        while (*p != '\0' && *p != ',') {
            switch (*p) {
                case 'r':
                case 'w':
                case 'x':
                    clause_rwx_bits |= bx_mkdir_perm_bits_for_who(who_flags, *p);
                    break;
                case 'X':
                    clause_rwx_bits |= bx_mkdir_perm_bits_for_who(who_flags, 'x');
                    break;
                case 's':
                    if ((who_flags & BX_MKDIR_WHO_U) != 0u) {
                        clause_special_bits |= S_ISUID;
                    }
                    if ((who_flags & BX_MKDIR_WHO_G) != 0u) {
                        clause_special_bits |= S_ISGID;
                    }
                    break;
                case 't':
                    if ((who_flags & BX_MKDIR_WHO_O) != 0u) {
                        clause_special_bits |= BX_MKDIR_STICKY_BIT;
                    }
                    break;
                case 'u':
                case 'g':
                case 'o':
                    clause_rwx_bits |= bx_mkdir_copy_perm_bits(source_mode, who_flags, *p);
                    break;
                default:
                    return false;
            }
            p++;
        }

        mode_t affected_rwx_mask = bx_mkdir_rwx_mask_from_who(who_flags);
        if (!who_specified) {
            affected_rwx_mask &= (mode_t)(~current_umask) & 0777u;
        }

        mode_t clear_rwx_mask = affected_rwx_mask;
        if (op == '=' && !who_specified) {
            clear_rwx_mask = S_IRWXU | S_IRWXG | S_IRWXO;
        }

        mode_t affected_special_mask = who_specified ? bx_mkdir_special_mask_from_who(who_flags) : (S_ISUID | S_ISGID | BX_MKDIR_STICKY_BIT);
        mode_t applied_rwx_bits = clause_rwx_bits & affected_rwx_mask;

        if (op == '+') {
            mode |= applied_rwx_bits;
            mode |= clause_special_bits;
        }
        else if (op == '-') {
            mode &= ~applied_rwx_bits;
            mode &= ~clause_special_bits;
        }
        else {
            mode &= ~clear_rwx_mask;
            mode &= ~affected_special_mask;
            mode |= applied_rwx_bits;
            mode |= clause_special_bits;
        }

        if (*p == ',') {
            p++;
            if (*p == '\0') {
                return false;
            }
        }
    }

    *mode_out = mode & 07777u;
    return true;
}

static bool bx_mkdir_parse_mode(const char* text, mode_t* mode_out, struct bx_diag_ctx* diag) {
    if (text == NULL || text[0] == '\0') {
        bx_diag(diag, "invalid mode '%s'", (text != NULL) ? text : "");
        return false;
    }

    if (bx_mkdir_mode_is_octal(text)) {
        if (bx_mkdir_parse_numeric_mode(text, mode_out)) {
            return true;
        }
    }
    else if (bx_mkdir_parse_symbolic_mode(text, mode_out)) {
        return true;
    }

    bx_diag(diag, "invalid mode '%s'", text);
    return false;
}

static bool bx_mkdir_parse_options(int argc, char** argv, struct bx_mkdir_options* options, int* first_operand, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"mode", required_argument, NULL, 'm'}, {"parents", no_argument, NULL, 'p'}, {"verbose", no_argument, NULL, 'v'},
        {"help", no_argument, NULL, 1},         {"version", no_argument, NULL, 2},   {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_mkdir_progname((argc > 0) ? argv[0] : NULL);
    diag->progname = options->progname;

    opterr = 0;
    optind = 1;

    while (true) {
        int option_index = 0;
        int c = getopt_long(argc, argv, "+:m:pv", long_options, &option_index);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 'm':
                if (!bx_mkdir_parse_mode(optarg, &options->mode, diag)) {
                    return false;
                }
                options->mode_set = true;
                break;
            case 'p':
                options->parents = true;
                break;
            case 'v':
                options->verbose = true;
                break;
            case 1:
                options->show_help = true;
                return true;
            case 2:
                options->show_version = true;
                return true;
            case ':':
                if (optopt != 0) {
                    bx_diag(diag, "option requires an argument -- '%c'", optopt);
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

static bool bx_mkdir_emit_created(const char* path, struct bx_diag_ctx* diag) {
    if (fprintf(stdout, "mkdir: created directory '%s'\n", path) < 0) {
        bx_diag(diag, "write error: %s", strerror(errno));
        return false;
    }
    return true;
}

static bool bx_mkdir_create_component(const char* path, bool final_component, const struct bx_mkdir_options* options, struct bx_diag_ctx* diag) {
    mode_t create_mode = (options->mode_set && final_component) ? options->mode : 0777u;
    if (mkdir(path, create_mode) == 0) {
        if (options->mode_set && final_component && chmod(path, options->mode) != 0) {
            bx_perror_path(diag, path);
            return false;
        }
        if (options->verbose && !bx_mkdir_emit_created(path, diag)) {
            return false;
        }
        return true;
    }

    if (errno == EEXIST) {
        struct stat st;
        if (stat(path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                return true;
            }
            errno = final_component ? EEXIST : ENOTDIR;
        }
    }

    bx_perror_path(diag, path);
    return false;
}

static bool bx_mkdir_parents(const char* path, const struct bx_mkdir_options* options, struct bx_diag_ctx* diag) {
    char* path_copy = xstrdup(path);
    size_t len = strlen(path_copy);
    size_t start = 0;

    if (len == 0) {
        errno = ENOENT;
        bx_perror_path(diag, path);
        free(path_copy);
        return false;
    }

    if (path_copy[0] == '/') {
        start = 1;
    }

    for (size_t i = start; i <= len; i++) {
        if (path_copy[i] != '/' && path_copy[i] != '\0') {
            continue;
        }

        bool final_component = (path_copy[i] == '\0');
        if (final_component && i > start && path_copy[i - 1] == '/') {
            continue;
        }

        char saved = path_copy[i];
        path_copy[i] = '\0';

        if (path_copy[0] != '\0' && !bx_mkdir_create_component(path_copy, final_component, options, diag)) {
            free(path_copy);
            return false;
        }

        path_copy[i] = saved;
    }

    free(path_copy);
    return true;
}

static bool bx_mkdir_one(const char* path, const struct bx_mkdir_options* options, struct bx_diag_ctx* diag) {
    if (options->parents) {
        return bx_mkdir_parents(path, options, diag);
    }

    mode_t create_mode = options->mode_set ? options->mode : 0777u;
    if (mkdir(path, create_mode) != 0) {
        bx_perror_path(diag, path);
        return false;
    }

    if (options->mode_set && chmod(path, options->mode) != 0) {
        bx_perror_path(diag, path);
        return false;
    }

    if (options->verbose && !bx_mkdir_emit_created(path, diag)) {
        return false;
    }

    return true;
}

int bx_mkdir_main(int argc, char** argv) {
    struct bx_mkdir_options options;
    struct bx_diag_ctx diag = {
        .progname = "mkdir",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };
    int first_operand = 0;

    if (!bx_mkdir_parse_options(argc, argv, &options, &first_operand, &diag)) {
        return diag.exit_status != 0 ? diag.exit_status : 1;
    }

    if (options.show_help) {
        bx_mkdir_print_help(stdout, options.progname);
        return 0;
    }

    if (options.show_version) {
        bx_mkdir_print_version(options.progname);
        return 0;
    }

    int operand_count = argc - first_operand;
    if (operand_count <= 0) {
        bx_diag(&diag, "missing operand");
        return diag.exit_status;
    }

    for (int i = first_operand; i < argc; i++) {
        (void)bx_mkdir_one(argv[i], &options, &diag);
    }

    if (options.verbose && fflush(stdout) == EOF) {
        bx_diag(&diag, "write error: %s", strerror(errno));
    }

    return diag.exit_status;
}
