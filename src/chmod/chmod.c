#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "applets.h"
#include "common/path_ops.h"
#include "diag.h"
#include "libbx.h"

#ifdef S_ISVTX
#define BX_CHMOD_STICKY_BIT S_ISVTX
#else
#define BX_CHMOD_STICKY_BIT 01000
#endif

enum bx_chmod_mode_kind {
    BX_CHMOD_MODE_NUMERIC = 0,
    BX_CHMOD_MODE_SYMBOLIC = 1,
};

enum bx_chmod_report_mode {
    BX_CHMOD_REPORT_NONE = 0,
    BX_CHMOD_REPORT_CHANGES,
    BX_CHMOD_REPORT_VERBOSE,
};

struct bx_chmod_mode_spec {
    enum bx_chmod_mode_kind kind;
    mode_t numeric_mode;
    const char* symbolic_mode;
    mode_t umask_value;
};

struct bx_chmod_options {
    const char* progname;
    bool recursive;
    bool quiet;
    enum bx_chmod_report_mode report_mode;
    const char* reference_path;
    bool show_help;
    bool show_version;
};

static const char* bx_chmod_progname(const char* argv0) {
    if (argv0 == NULL || argv0[0] == '\0') {
        return "chmod";
    }

    const char* base = strrchr(argv0, '/');
    if (base != NULL && base[1] != '\0') {
        return base + 1;
    }
    return argv0;
}

static void bx_chmod_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... MODE[,MODE]... FILE...\n", progname);
    fprintf(stream, "  or:  %s [OPTION]... OCTAL-MODE FILE...\n", progname);
    fprintf(stream, "  or:  %s [OPTION]... --reference=RFILE FILE...\n", progname);
    fprintf(stream, "Change the mode of each FILE to MODE.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -c, --changes    like verbose but report only when a change is made\n");
    fprintf(stream, "  -f, --silent, --quiet  suppress most error messages\n");
    fprintf(stream, "  -R, --recursive  change files and directories recursively\n");
    fprintf(stream, "      --reference=RFILE  use RFILE's mode instead of MODE values\n");
    fprintf(stream, "  -v, --verbose    output a diagnostic for every file processed\n");
    fprintf(stream, "      --help     display this help and exit\n");
    fprintf(stream, "      --version  output version information and exit\n");
}

static void bx_chmod_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

static bool bx_chmod_short_group_is_options(const char* arg) {
    for (const char* p = arg + 1; *p != '\0'; p++) {
        if (*p != 'R' && *p != 'c' && *p != 'f' && *p != 'v') {
            return false;
        }
    }
    return true;
}

static bool bx_chmod_parse_options(int argc, char** argv, struct bx_chmod_options* options, int* first_operand, struct bx_diag_ctx* diag) {
    memset(options, 0, sizeof(*options));
    options->progname = bx_chmod_progname((argc > 0) ? argv[0] : NULL);
    diag->progname = options->progname;

    int i = 1;
    while (i < argc) {
        const char* arg = argv[i];

        if (strcmp(arg, "--") == 0) {
            i++;
            break;
        }
        if (strcmp(arg, "--help") == 0) {
            options->show_help = true;
            *first_operand = i + 1;
            return true;
        }
        if (strcmp(arg, "--version") == 0) {
            options->show_version = true;
            *first_operand = i + 1;
            return true;
        }
        if (strcmp(arg, "--changes") == 0) {
            options->report_mode = BX_CHMOD_REPORT_CHANGES;
            i++;
            continue;
        }
        if (strcmp(arg, "--recursive") == 0) {
            options->recursive = true;
            i++;
            continue;
        }
        if (strcmp(arg, "--verbose") == 0) {
            options->report_mode = BX_CHMOD_REPORT_VERBOSE;
            i++;
            continue;
        }
        if (strcmp(arg, "--quiet") == 0 || strcmp(arg, "--silent") == 0) {
            options->quiet = true;
            i++;
            continue;
        }
        if (strcmp(arg, "--reference") == 0) {
            if (i + 1 >= argc) {
                bx_diag(diag, "option '--reference' requires an argument");
                return false;
            }
            options->reference_path = argv[i + 1];
            i += 2;
            continue;
        }
        if (strncmp(arg, "--reference=", 12) == 0) {
            options->reference_path = arg + 12;
            i++;
            continue;
        }
        if (arg[0] == '-' && arg[1] == '-') {
            bx_diag(diag, "unrecognized option '%s'", arg);
            return false;
        }
        if (arg[0] == '-' && arg[1] != '\0') {
            if (!bx_chmod_short_group_is_options(arg)) {
                break;
            }

            for (const char* p = arg + 1; *p != '\0'; p++) {
                if (*p == 'R') {
                    options->recursive = true;
                }
                else if (*p == 'c') {
                    options->report_mode = BX_CHMOD_REPORT_CHANGES;
                }
                else if (*p == 'f') {
                    options->quiet = true;
                }
                else if (*p == 'v') {
                    options->report_mode = BX_CHMOD_REPORT_VERBOSE;
                }
            }
            i++;
            continue;
        }

        break;
    }

    *first_operand = i;
    return true;
}

enum {
    BX_CHMOD_WHO_U = 1u << 0,
    BX_CHMOD_WHO_G = 1u << 1,
    BX_CHMOD_WHO_O = 1u << 2,
    BX_CHMOD_WHO_ALL = BX_CHMOD_WHO_U | BX_CHMOD_WHO_G | BX_CHMOD_WHO_O,
};

static mode_t bx_chmod_rwx_mask_from_who(unsigned int who_flags) {
    mode_t mask = 0u;

    if ((who_flags & BX_CHMOD_WHO_U) != 0u) {
        mask |= S_IRWXU;
    }
    if ((who_flags & BX_CHMOD_WHO_G) != 0u) {
        mask |= S_IRWXG;
    }
    if ((who_flags & BX_CHMOD_WHO_O) != 0u) {
        mask |= S_IRWXO;
    }

    return mask;
}

static mode_t bx_chmod_special_mask_from_who(unsigned int who_flags) {
    mode_t mask = 0u;

    if ((who_flags & BX_CHMOD_WHO_U) != 0u) {
        mask |= S_ISUID;
    }
    if ((who_flags & BX_CHMOD_WHO_G) != 0u) {
        mask |= S_ISGID;
    }
    if ((who_flags & BX_CHMOD_WHO_O) != 0u) {
        mask |= BX_CHMOD_STICKY_BIT;
    }

    return mask;
}

static mode_t bx_chmod_perm_bits_for_who(unsigned int who_flags, char perm) {
    mode_t bits = 0u;

    if (perm == 'r') {
        if ((who_flags & BX_CHMOD_WHO_U) != 0u) {
            bits |= S_IRUSR;
        }
        if ((who_flags & BX_CHMOD_WHO_G) != 0u) {
            bits |= S_IRGRP;
        }
        if ((who_flags & BX_CHMOD_WHO_O) != 0u) {
            bits |= S_IROTH;
        }
    }
    else if (perm == 'w') {
        if ((who_flags & BX_CHMOD_WHO_U) != 0u) {
            bits |= S_IWUSR;
        }
        if ((who_flags & BX_CHMOD_WHO_G) != 0u) {
            bits |= S_IWGRP;
        }
        if ((who_flags & BX_CHMOD_WHO_O) != 0u) {
            bits |= S_IWOTH;
        }
    }
    else if (perm == 'x') {
        if ((who_flags & BX_CHMOD_WHO_U) != 0u) {
            bits |= S_IXUSR;
        }
        if ((who_flags & BX_CHMOD_WHO_G) != 0u) {
            bits |= S_IXGRP;
        }
        if ((who_flags & BX_CHMOD_WHO_O) != 0u) {
            bits |= S_IXOTH;
        }
    }

    return bits;
}

static mode_t bx_chmod_copy_perm_bits(mode_t mode, unsigned int who_flags, char source_class) {
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
    if ((who_flags & BX_CHMOD_WHO_U) != 0u) {
        bits |= source << 6;
    }
    if ((who_flags & BX_CHMOD_WHO_G) != 0u) {
        bits |= source << 3;
    }
    if ((who_flags & BX_CHMOD_WHO_O) != 0u) {
        bits |= source;
    }

    return bits;
}

static bool bx_chmod_mode_is_octal(const char* text) {
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

static bool bx_chmod_parse_numeric_mode(const char* text, mode_t* mode_out) {
    errno = 0;
    char* end = NULL;
    unsigned long value = strtoul(text, &end, 8);
    if (errno == ERANGE || end == text || end == NULL || end[0] != '\0' || value > 07777ul) {
        return false;
    }

    *mode_out = (mode_t)value;
    return true;
}

static bool bx_chmod_apply_symbolic_mode(const char* text, mode_t start_mode, bool is_directory, mode_t umask_value, mode_t* mode_out) {
    if (text == NULL || text[0] == '\0') {
        return false;
    }

    mode_t mode = start_mode & 07777u;
    const char* p = text;

    while (*p != '\0') {
        unsigned int who_flags = 0u;
        bool who_specified = false;

        while (*p == 'u' || *p == 'g' || *p == 'o' || *p == 'a') {
            who_specified = true;
            if (*p == 'u') {
                who_flags |= BX_CHMOD_WHO_U;
            }
            else if (*p == 'g') {
                who_flags |= BX_CHMOD_WHO_G;
            }
            else if (*p == 'o') {
                who_flags |= BX_CHMOD_WHO_O;
            }
            else {
                who_flags |= BX_CHMOD_WHO_ALL;
            }
            p++;
        }

        if (!who_specified) {
            who_flags = BX_CHMOD_WHO_ALL;
        }

        if (*p != '+' && *p != '-' && *p != '=') {
            return false;
        }

        while (*p == '+' || *p == '-' || *p == '=') {
            char op = *p;
            p++;

            mode_t op_rwx_bits = 0u;
            mode_t op_special_bits = 0u;
            mode_t source_mode = mode;

            while (*p != '\0' && *p != ',' && *p != '+' && *p != '-' && *p != '=') {
                switch (*p) {
                    case 'r':
                    case 'w':
                    case 'x':
                        op_rwx_bits |= bx_chmod_perm_bits_for_who(who_flags, *p);
                        break;
                    case 'X':
                        if (is_directory || (source_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0u) {
                            op_rwx_bits |= bx_chmod_perm_bits_for_who(who_flags, 'x');
                        }
                        break;
                    case 's':
                        if ((who_flags & BX_CHMOD_WHO_U) != 0u) {
                            op_special_bits |= S_ISUID;
                        }
                        if ((who_flags & BX_CHMOD_WHO_G) != 0u) {
                            op_special_bits |= S_ISGID;
                        }
                        break;
                    case 't':
                        if ((who_flags & BX_CHMOD_WHO_O) != 0u) {
                            op_special_bits |= BX_CHMOD_STICKY_BIT;
                        }
                        break;
                    case 'u':
                    case 'g':
                    case 'o':
                        op_rwx_bits |= bx_chmod_copy_perm_bits(source_mode, who_flags, *p);
                        break;
                    default:
                        return false;
                }
                p++;
            }

            mode_t affected_rwx_mask = bx_chmod_rwx_mask_from_who(who_flags);
            if (!who_specified) {
                affected_rwx_mask &= (mode_t)(~umask_value) & 0777u;
            }

            mode_t clear_rwx_mask = affected_rwx_mask;
            if (op == '=' && !who_specified) {
                clear_rwx_mask = S_IRWXU | S_IRWXG | S_IRWXO;
            }

            mode_t affected_special_mask = who_specified ? bx_chmod_special_mask_from_who(who_flags) : (S_ISUID | S_ISGID | BX_CHMOD_STICKY_BIT);
            mode_t applied_rwx_bits = op_rwx_bits & affected_rwx_mask;

            if (op == '+') {
                mode |= applied_rwx_bits;
                mode |= op_special_bits;
            }
            else if (op == '-') {
                mode &= ~applied_rwx_bits;
                mode &= ~op_special_bits;
            }
            else {
                mode &= ~clear_rwx_mask;
                mode &= ~affected_special_mask;
                mode |= applied_rwx_bits;
                mode |= op_special_bits;
            }
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

static bool bx_chmod_parse_mode_spec(const char* text, struct bx_chmod_mode_spec* mode_spec, struct bx_diag_ctx* diag) {
    if (text == NULL || text[0] == '\0') {
        bx_diag(diag, "invalid mode '%s'", (text != NULL) ? text : "");
        return false;
    }

    memset(mode_spec, 0, sizeof(*mode_spec));

    if (bx_chmod_mode_is_octal(text)) {
        mode_spec->kind = BX_CHMOD_MODE_NUMERIC;
        if (bx_chmod_parse_numeric_mode(text, &mode_spec->numeric_mode)) {
            return true;
        }
    }
    else {
        mode_t umask_value = umask(0u);
        umask(umask_value);

        mode_t dummy_mode = 0u;
        if (bx_chmod_apply_symbolic_mode(text, 0u, false, umask_value, &dummy_mode)) {
            mode_spec->kind = BX_CHMOD_MODE_SYMBOLIC;
            mode_spec->symbolic_mode = text;
            mode_spec->umask_value = umask_value;
            return true;
        }
    }

    bx_diag(diag, "invalid mode '%s'", text);
    return false;
}

static bool bx_chmod_parse_reference_mode(const char* reference_path, struct bx_chmod_mode_spec* mode_spec, struct bx_diag_ctx* diag) {
    struct stat st;
    if (stat(reference_path, &st) != 0) {
        bx_perror_path(diag, reference_path);
        return false;
    }

    memset(mode_spec, 0, sizeof(*mode_spec));
    mode_spec->kind = BX_CHMOD_MODE_NUMERIC;
    mode_spec->numeric_mode = st.st_mode & 07777u;
    return true;
}

static void bx_chmod_perror_path(const struct bx_chmod_options* options, struct bx_diag_ctx* diag, const char* path) {
    if (options->quiet) {
        diag->exit_status = 1;
        return;
    }
    bx_perror_path(diag, path);
}

static bool bx_chmod_emit_report(const struct bx_chmod_options* options, const char* path, mode_t old_mode, mode_t new_mode, bool changed, struct bx_diag_ctx* diag) {
    if (options->report_mode == BX_CHMOD_REPORT_NONE) {
        return true;
    }
    if (options->report_mode == BX_CHMOD_REPORT_CHANGES && !changed) {
        return true;
    }

    int wrote = 0;
    if (changed) {
        wrote = fprintf(stdout, "mode of '%s' changed from %04o to %04o\n", path, (unsigned int)(old_mode & 07777u), (unsigned int)(new_mode & 07777u));
    }
    else {
        wrote = fprintf(stdout, "mode of '%s' retained as %04o\n", path, (unsigned int)(new_mode & 07777u));
    }
    if (wrote < 0) {
        bx_diag(diag, "write error: %s", strerror(errno));
        return false;
    }
    return true;
}

static bool bx_chmod_apply_existing(const char* path, const struct stat* st, const struct bx_chmod_mode_spec* mode_spec, const struct bx_chmod_options* options, struct bx_diag_ctx* diag) {
    mode_t old_mode = st->st_mode & 07777u;
    mode_t mode_value = old_mode;

    if (mode_spec->kind == BX_CHMOD_MODE_NUMERIC) {
        mode_value = mode_spec->numeric_mode;
    }
    else if (!bx_chmod_apply_symbolic_mode(mode_spec->symbolic_mode, old_mode, S_ISDIR(st->st_mode), mode_spec->umask_value, &mode_value)) {
        bx_diag(diag, "invalid mode '%s'", mode_spec->symbolic_mode);
        return false;
    }

    if (chmod(path, mode_value) != 0) {
        bx_chmod_perror_path(options, diag, path);
        return false;
    }

    bool changed = (old_mode != mode_value);
    return bx_chmod_emit_report(options, path, old_mode, mode_value, changed, diag);
}

static bool bx_chmod_apply_path_recursive(const char* path, bool top_level, const struct bx_chmod_mode_spec* mode_spec, const struct bx_chmod_options* options, struct bx_diag_ctx* diag) {
    struct stat st;

    if (top_level) {
        if (stat(path, &st) != 0) {
            bx_chmod_perror_path(options, diag, path);
            return false;
        }
    }
    else {
        if (lstat(path, &st) != 0) {
            bx_chmod_perror_path(options, diag, path);
            return false;
        }
        if (S_ISLNK(st.st_mode)) {
            return true;
        }
    }

    bool ok = bx_chmod_apply_existing(path, &st, mode_spec, options, diag);

    if (!options->recursive || !S_ISDIR(st.st_mode)) {
        return ok;
    }

    DIR* dir = opendir(path);
    if (dir == NULL) {
        bx_chmod_perror_path(options, diag, path);
        return false;
    }

    bool recurse_ok = true;
    for (;;) {
        errno = 0;
        struct dirent* entry = readdir(dir);
        if (entry == NULL) {
            if (errno != 0) {
                bx_chmod_perror_path(options, diag, path);
                recurse_ok = false;
            }
            break;
        }
        if (bx_path_is_dot_or_dotdot(entry->d_name)) {
            continue;
        }

        char* child_path = bx_path_join(path, entry->d_name);
        if (!bx_chmod_apply_path_recursive(child_path, false, mode_spec, options, diag)) {
            recurse_ok = false;
        }
        free(child_path);
    }

    if (closedir(dir) != 0) {
        bx_chmod_perror_path(options, diag, path);
        recurse_ok = false;
    }

    return ok && recurse_ok;
}

static void bx_chmod_apply_one(const char* path, const struct bx_chmod_mode_spec* mode_spec, const struct bx_chmod_options* options, struct bx_diag_ctx* diag) {
    (void)bx_chmod_apply_path_recursive(path, true, mode_spec, options, diag);
}

static int bx_chmod_report_missing_mode_operand(struct bx_diag_ctx* diag, const char* mode_operand) {
    if (mode_operand == NULL) {
        bx_diag(diag, "missing operand");
    }
    else {
        bx_diag(diag, "missing operand after '%s'", mode_operand);
    }
    return diag->exit_status;
}

int bx_chmod_main(int argc, char** argv) {
    struct bx_chmod_options options;
    struct bx_chmod_mode_spec mode_spec;
    struct bx_diag_ctx diag = {
        .progname = "chmod",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };
    int first_operand = 0;

    if (!bx_chmod_parse_options(argc, argv, &options, &first_operand, &diag)) {
        return diag.exit_status != 0 ? diag.exit_status : 1;
    }

    if (options.show_help) {
        bx_chmod_print_help(stdout, options.progname);
        return 0;
    }

    if (options.show_version) {
        bx_chmod_print_version(options.progname);
        return 0;
    }

    int operand_count = argc - first_operand;
    if (operand_count <= 0) {
        return bx_chmod_report_missing_mode_operand(&diag, NULL);
    }

    int first_file = first_operand;

    if (options.reference_path != NULL) {
        if (!bx_chmod_parse_reference_mode(options.reference_path, &mode_spec, &diag)) {
            return diag.exit_status;
        }
    }
    else {
        if (operand_count <= 1) {
            return bx_chmod_report_missing_mode_operand(&diag, argv[first_operand]);
        }
        if (!bx_chmod_parse_mode_spec(argv[first_operand], &mode_spec, &diag)) {
            return diag.exit_status;
        }

        first_file = first_operand + 1;
        if (first_file < argc && strcmp(argv[first_file], "--") == 0) {
            first_file++;
        }
        if (first_file >= argc) {
            return bx_chmod_report_missing_mode_operand(&diag, argv[first_operand]);
        }
    }

    for (int i = first_file; i < argc; i++) {
        bx_chmod_apply_one(argv[i], &mode_spec, &options, &diag);
    }

    if (options.report_mode != BX_CHMOD_REPORT_NONE && fflush(stdout) == EOF) {
        bx_diag(&diag, "write error: %s", strerror(errno));
    }

    return diag.exit_status;
}
