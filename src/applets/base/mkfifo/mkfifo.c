#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "applets.h"
#include "bx/diag.h"
#include "lib/cli_common.h"

struct bx_mkfifo_options {
    const char* progname;
    bool mode_set;
    mode_t mode;
    bool show_help;
    bool show_version;
};

static void bx_mkfifo_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... NAME...\n", progname);
    fprintf(stream, "Create the FIFO special files NAMEs.\n");
    fprintf(stream, "\n");
    fprintf(stream, "Mandatory arguments to long options are mandatory for short options too.\n");
    fprintf(stream, "  -m, --mode=MODE  set file permission bits to MODE, not a=rw - umask\n");
    fprintf(stream, "      --help     display this help and exit\n");
    fprintf(stream, "      --version  output version information and exit\n");
}

enum {
    BX_MKFIFO_WHO_U = 1u << 0,
    BX_MKFIFO_WHO_G = 1u << 1,
    BX_MKFIFO_WHO_O = 1u << 2,
    BX_MKFIFO_WHO_ALL = BX_MKFIFO_WHO_U | BX_MKFIFO_WHO_G | BX_MKFIFO_WHO_O,
};

static mode_t bx_mkfifo_rwx_mask_from_who(unsigned int who_flags) {
    mode_t mask = 0u;
    if ((who_flags & BX_MKFIFO_WHO_U) != 0u) {
        mask |= S_IRWXU;
    }
    if ((who_flags & BX_MKFIFO_WHO_G) != 0u) {
        mask |= S_IRWXG;
    }
    if ((who_flags & BX_MKFIFO_WHO_O) != 0u) {
        mask |= S_IRWXO;
    }
    return mask;
}

static mode_t bx_mkfifo_perm_bits_for_who(unsigned int who_flags, char perm) {
    mode_t bits = 0u;

    if (perm == 'r') {
        if ((who_flags & BX_MKFIFO_WHO_U) != 0u) {
            bits |= S_IRUSR;
        }
        if ((who_flags & BX_MKFIFO_WHO_G) != 0u) {
            bits |= S_IRGRP;
        }
        if ((who_flags & BX_MKFIFO_WHO_O) != 0u) {
            bits |= S_IROTH;
        }
    }
    else if (perm == 'w') {
        if ((who_flags & BX_MKFIFO_WHO_U) != 0u) {
            bits |= S_IWUSR;
        }
        if ((who_flags & BX_MKFIFO_WHO_G) != 0u) {
            bits |= S_IWGRP;
        }
        if ((who_flags & BX_MKFIFO_WHO_O) != 0u) {
            bits |= S_IWOTH;
        }
    }
    else if (perm == 'x') {
        if ((who_flags & BX_MKFIFO_WHO_U) != 0u) {
            bits |= S_IXUSR;
        }
        if ((who_flags & BX_MKFIFO_WHO_G) != 0u) {
            bits |= S_IXGRP;
        }
        if ((who_flags & BX_MKFIFO_WHO_O) != 0u) {
            bits |= S_IXOTH;
        }
    }

    return bits;
}

static mode_t bx_mkfifo_copy_perm_bits(mode_t mode, unsigned int who_flags, char source_class) {
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
    if ((who_flags & BX_MKFIFO_WHO_U) != 0u) {
        bits |= source << 6;
    }
    if ((who_flags & BX_MKFIFO_WHO_G) != 0u) {
        bits |= source << 3;
    }
    if ((who_flags & BX_MKFIFO_WHO_O) != 0u) {
        bits |= source;
    }
    return bits;
}

static bool bx_mkfifo_mode_is_octal(const char* text) {
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

static bool bx_mkfifo_parse_numeric_mode(const char* text, mode_t* mode_out) {
    errno = 0;
    char* end = NULL;
    unsigned long value = strtoul(text, &end, 8);
    if (errno == ERANGE || end == text || end == NULL || end[0] != '\0' || value > 0777ul) {
        return false;
    }

    *mode_out = (mode_t)value;
    return true;
}

static bool bx_mkfifo_parse_symbolic_mode(const char* text, mode_t* mode_out) {
    mode_t mode = 0666u;
    const mode_t current_umask = umask(0u);
    umask(current_umask);

    const char* p = text;
    while (*p != '\0') {
        unsigned int who_flags = 0u;
        bool who_specified = false;

        while (*p == 'u' || *p == 'g' || *p == 'o' || *p == 'a') {
            who_specified = true;
            if (*p == 'u') {
                who_flags |= BX_MKFIFO_WHO_U;
            }
            else if (*p == 'g') {
                who_flags |= BX_MKFIFO_WHO_G;
            }
            else if (*p == 'o') {
                who_flags |= BX_MKFIFO_WHO_O;
            }
            else {
                who_flags |= BX_MKFIFO_WHO_ALL;
            }
            p++;
        }

        if (!who_specified) {
            who_flags = BX_MKFIFO_WHO_ALL;
        }

        char op = *p;
        if (op != '+' && op != '-' && op != '=') {
            return false;
        }
        p++;

        mode_t clause_rwx_bits = 0u;
        mode_t source_mode = mode;
        while (*p != '\0' && *p != ',') {
            switch (*p) {
                case 'r':
                case 'w':
                case 'x':
                    clause_rwx_bits |= bx_mkfifo_perm_bits_for_who(who_flags, *p);
                    break;
                case 'X':
                    if ((source_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0u) {
                        clause_rwx_bits |= bx_mkfifo_perm_bits_for_who(who_flags, 'x');
                    }
                    break;
                case 'u':
                case 'g':
                case 'o':
                    clause_rwx_bits |= bx_mkfifo_copy_perm_bits(source_mode, who_flags, *p);
                    break;
                default:
                    return false;
            }
            p++;
        }

        mode_t affected_rwx_mask = bx_mkfifo_rwx_mask_from_who(who_flags);
        if (!who_specified) {
            affected_rwx_mask &= (mode_t)(~current_umask) & 0777u;
        }

        mode_t clear_rwx_mask = affected_rwx_mask;
        if (op == '=' && !who_specified) {
            clear_rwx_mask = S_IRWXU | S_IRWXG | S_IRWXO;
        }

        mode_t applied_rwx_bits = clause_rwx_bits & affected_rwx_mask;
        if (op == '+') {
            mode |= applied_rwx_bits;
        }
        else if (op == '-') {
            mode &= ~applied_rwx_bits;
        }
        else {
            mode &= ~clear_rwx_mask;
            mode |= applied_rwx_bits;
        }

        if (*p == ',') {
            p++;
            if (*p == '\0' || *p == ',') {
                return false;
            }
        }
    }

    *mode_out = mode & 0777u;
    return true;
}

static bool bx_mkfifo_parse_mode(const char* text, mode_t* mode_out, struct bx_diag_ctx* diag) {
    if (text == NULL || text[0] == '\0') {
        bx_diag(diag, "invalid mode '%s'", (text != NULL) ? text : "");
        return false;
    }

    if (bx_mkfifo_mode_is_octal(text)) {
        if (bx_mkfifo_parse_numeric_mode(text, mode_out)) {
            return true;
        }
    }
    else if (bx_mkfifo_parse_symbolic_mode(text, mode_out)) {
        return true;
    }

    bx_diag(diag, "invalid mode '%s'", text);
    return false;
}

static bool bx_mkfifo_parse_options(int argc, char** argv, struct bx_mkfifo_options* options, int* first_operand, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"mode", required_argument, NULL, 'm'},
        {"help", no_argument, NULL, 1},
        {"version", no_argument, NULL, 2},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "mkfifo");
    diag->progname = options->progname;

    opterr = 0;
    optind = 1;

    while (true) {
        int option_index = 0;
        int c = getopt_long(argc, argv, "+:m:", long_options, &option_index);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 'm':
                if (!bx_mkfifo_parse_mode(optarg, &options->mode, diag)) {
                    return false;
                }
                options->mode_set = true;
                break;
            case 1:
                options->show_help = true;
                return true;
            case 2:
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

    *first_operand = optind;
    return true;
}

int bx_mkfifo_main(int argc, char** argv) {
    struct bx_mkfifo_options options;
    struct bx_diag_ctx diag = {
        .progname = "mkfifo",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };
    int first_operand = 0;

    if (!bx_mkfifo_parse_options(argc, argv, &options, &first_operand, &diag)) {
        return diag.exit_status != 0 ? diag.exit_status : 1;
    }

    if (options.show_help) {
        bx_mkfifo_print_help(stdout, options.progname);
        return 0;
    }

    if (options.show_version) {
        bx_cli_print_version(options.progname);
        return 0;
    }

    int operand_count = argc - first_operand;
    if (operand_count <= 0) {
        bx_cli_diag_missing_operand(&diag);
        return diag.exit_status;
    }

    for (int i = first_operand; i < argc; i++) {
        mode_t create_mode = options.mode_set ? options.mode : 0666u;
        if (mkfifo(argv[i], create_mode) != 0) {
            bx_perror_path(&diag, argv[i]);
        }
        else if (options.mode_set && chmod(argv[i], options.mode) != 0) {
            bx_perror_path(&diag, argv[i]);
        }
    }

    return diag.exit_status;
}
