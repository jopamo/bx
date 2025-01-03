#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "applets.h"
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

struct bx_chmod_mode_spec {
    enum bx_chmod_mode_kind kind;
    mode_t numeric_mode;
    const char* symbolic_mode;
    mode_t umask_value;
};

struct bx_chmod_options {
    const char* progname;
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
    fprintf(stream, "Change the mode of each FILE to MODE.\n");
    fprintf(stream, "\n");
    fprintf(stream, "      --help     display this help and exit\n");
    fprintf(stream, "      --version  output version information and exit\n");
}

static void bx_chmod_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
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
        if (arg[0] == '-' && arg[1] == '-' && arg[2] != '\0') {
            bx_diag(diag, "unrecognized option '%s'", arg);
            return false;
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
                    clause_rwx_bits |= bx_chmod_perm_bits_for_who(who_flags, *p);
                    break;
                case 'X':
                    if (is_directory || (source_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0u) {
                        clause_rwx_bits |= bx_chmod_perm_bits_for_who(who_flags, 'x');
                    }
                    break;
                case 's':
                    if ((who_flags & BX_CHMOD_WHO_U) != 0u) {
                        clause_special_bits |= S_ISUID;
                    }
                    if ((who_flags & BX_CHMOD_WHO_G) != 0u) {
                        clause_special_bits |= S_ISGID;
                    }
                    break;
                case 't':
                    if ((who_flags & BX_CHMOD_WHO_O) != 0u) {
                        clause_special_bits |= BX_CHMOD_STICKY_BIT;
                    }
                    break;
                case 'u':
                case 'g':
                case 'o':
                    clause_rwx_bits |= bx_chmod_copy_perm_bits(source_mode, who_flags, *p);
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

static void bx_chmod_apply_one(const char* path, const struct bx_chmod_mode_spec* mode_spec, struct bx_diag_ctx* diag) {
    mode_t mode_value = 0u;

    if (mode_spec->kind == BX_CHMOD_MODE_NUMERIC) {
        mode_value = mode_spec->numeric_mode;
    }
    else {
        struct stat st;
        if (stat(path, &st) != 0) {
            bx_perror_path(diag, path);
            return;
        }

        if (!bx_chmod_apply_symbolic_mode(mode_spec->symbolic_mode, st.st_mode & 07777u, S_ISDIR(st.st_mode), mode_spec->umask_value, &mode_value)) {
            bx_diag(diag, "invalid mode '%s'", mode_spec->symbolic_mode);
            return;
        }
    }

    if (chmod(path, mode_value) != 0) {
        bx_perror_path(diag, path);
    }
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
        bx_diag(&diag, "missing operand");
        return diag.exit_status;
    }
    if (operand_count <= 1) {
        bx_diag(&diag, "missing operand after '%s'", argv[first_operand]);
        return diag.exit_status;
    }

    if (!bx_chmod_parse_mode_spec(argv[first_operand], &mode_spec, &diag)) {
        return diag.exit_status;
    }

    int first_file = first_operand + 1;
    if (first_file < argc && strcmp(argv[first_file], "--") == 0) {
        first_file++;
    }
    if (first_file >= argc) {
        bx_diag(&diag, "missing operand after '%s'", argv[first_operand]);
        return diag.exit_status;
    }

    for (int i = first_file; i < argc; i++) {
        bx_chmod_apply_one(argv[i], &mode_spec, &diag);
    }

    return diag.exit_status;
}
