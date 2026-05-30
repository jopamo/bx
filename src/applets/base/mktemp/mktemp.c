#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"
#include "bx/libbx.h"
#include "lib/cli_common.h"
#include "lib/args_common.h"
#include "lib/fd_ops.h"
#include "lib/path_ops.h"

enum {
    BX_MKTEMP_OPT_TMPDIR = 256,
    BX_MKTEMP_OPT_SUFFIX,
};

struct bx_mktemp_options {
    const char* progname;
    bool create_directory;
    bool dry_run;
    bool quiet;
    bool template_component_mode;
    bool tmpdir_specified;
    const char* tmpdir_arg;
    const char* suffix;
    bool show_help;
    bool show_version;
};

static void bx_mktemp_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... [TEMPLATE]\n", progname);
    fprintf(stream, "Create a temporary file or directory, safely, and print its name.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -d, --directory     create a directory, not a file\n");
    fprintf(stream, "  -u, --dry-run       do not create anything; merely print a name (unsafe)\n");
    fprintf(stream, "  -q, --quiet         suppress diagnostics for creation failures\n");
    fprintf(stream, "      --suffix=SUFF   append SUFF to TEMPLATE; SUFF must not contain '/'\n");
    fprintf(stream, "  -p DIR, --tmpdir[=DIR]\n");
    fprintf(stream, "                     interpret TEMPLATE relative to DIR; if DIR is omitted,\n");
    fprintf(stream, "                     use $TMPDIR if set, else /tmp\n");
    fprintf(stream, "  -t                 interpret TEMPLATE as a name component under $TMPDIR,\n");
    fprintf(stream, "                     else -p DIR, else /tmp (deprecated)\n");
    fprintf(stream, "      --help          display this help and exit\n");
    fprintf(stream, "      --version       output version information and exit\n");
    fprintf(stream, "\n");
    fprintf(stream, "If TEMPLATE is omitted, mktemp uses tmp.XXXXXXXXXX with --tmpdir implied.\n");
    fprintf(stream, "TEMPLATE must contain at least 3 consecutive 'X' in the last component.\n");
}

static bool bx_mktemp_parse_options(int argc, char** argv, struct bx_mktemp_options* options, int* first_operand, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"directory", no_argument, NULL, 'd'},
        {"dry-run", no_argument, NULL, 'u'},
        {"quiet", no_argument, NULL, 'q'},
        {"tmpdir", optional_argument, NULL, BX_MKTEMP_OPT_TMPDIR},
        {"suffix", required_argument, NULL, BX_MKTEMP_OPT_SUFFIX},
        {"help", no_argument, NULL, 1},
        {"version", no_argument, NULL, 2},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "mktemp");
    diag->progname = options->progname;

    bx_args_getopt_reset();

    while (true) {
        int option_index = 0;
        int c = bx_args_getopt_long(argc, argv, "+duqtp:", long_options, &option_index);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 'd':
                options->create_directory = true;
                break;
            case 'u':
                options->dry_run = true;
                break;
            case 'q':
                options->quiet = true;
                break;
            case 't':
                options->template_component_mode = true;
                break;
            case 'p':
                options->tmpdir_specified = true;
                options->tmpdir_arg = optarg;
                break;
            case BX_MKTEMP_OPT_TMPDIR:
                options->tmpdir_specified = true;
                options->tmpdir_arg = optarg;
                break;
            case BX_MKTEMP_OPT_SUFFIX:
                options->suffix = optarg;
                break;
            case 1:
                options->show_help = true;
                return true;
            case 2:
                options->show_version = true;
                return true;
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

static const char* bx_mktemp_tmpdir_env(void) {
    const char* tmpdir_env = getenv("TMPDIR");
    if (tmpdir_env != NULL && tmpdir_env[0] != '\0') {
        return tmpdir_env;
    }

    return NULL;
}

static const char* bx_mktemp_default_tmpdir(void) {
    const char* tmpdir_env = bx_mktemp_tmpdir_env();
    return (tmpdir_env != NULL) ? tmpdir_env : "/tmp";
}

static const char* bx_mktemp_resolve_tmpdir_option(const struct bx_mktemp_options* options) {
    if (options->tmpdir_arg != NULL && options->tmpdir_arg[0] != '\0') {
        return options->tmpdir_arg;
    }
    return bx_mktemp_default_tmpdir();
}

static const char* bx_mktemp_resolve_t_tmpdir(const struct bx_mktemp_options* options) {
    const char* tmpdir_env = bx_mktemp_tmpdir_env();
    if (tmpdir_env != NULL) {
        return tmpdir_env;
    }

    if (options->tmpdir_specified && options->tmpdir_arg != NULL && options->tmpdir_arg[0] != '\0') {
        return options->tmpdir_arg;
    }

    return "/tmp";
}

static void bx_mktemp_seed_rng(void) {
    static bool seeded = false;
    if (seeded) {
        return;
    }

    unsigned int seed = (unsigned int)time(NULL) ^ (unsigned int)getpid();
    srand(seed);
    seeded = true;
}

static void bx_mktemp_fill_random(char* out, size_t count) {
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    const size_t alphabet_len = sizeof(alphabet) - 1u;

    bx_mktemp_seed_rng();
    for (size_t i = 0; i < count; i++) {
        out[i] = alphabet[(size_t)(rand() % (int)alphabet_len)];
    }
}

static bool bx_mktemp_prepare_template(const char* full_template,
                                       const char* original_template,
                                       const struct bx_mktemp_options* options,
                                       char** prepared_template_out,
                                       size_t* x_start_out,
                                       size_t* x_len_out,
                                       struct bx_diag_ctx* diag) {
    size_t template_len = strlen(full_template);
    size_t component_start = 0;
    const char* slash = strrchr(full_template, '/');
    if (slash != NULL) {
        component_start = (size_t)(slash - full_template) + 1u;
    }

    if (options->suffix != NULL) {
        if (template_len == 0 || full_template[template_len - 1u] != 'X') {
            bx_diag(diag, "with --suffix, template must end in 'X': '%s'", original_template);
            return false;
        }

        size_t run_start = template_len;
        while (run_start > component_start && full_template[run_start - 1u] == 'X') {
            run_start--;
        }

        size_t run_len = template_len - run_start;
        if (run_len < 3u) {
            bx_diag(diag, "template must contain at least 3 consecutive 'X' characters in the last component: '%s'", original_template);
            return false;
        }

        if (strchr(options->suffix, '/') != NULL) {
            bx_diag(diag, "invalid suffix '%s': contains directory separator", options->suffix);
            return false;
        }

        size_t suffix_len = strlen(options->suffix);
        char* prepared = xmalloc(template_len + suffix_len + 1u);
        memcpy(prepared, full_template, template_len);
        memcpy(prepared + template_len, options->suffix, suffix_len + 1u);

        *prepared_template_out = prepared;
        *x_start_out = run_start;
        *x_len_out = run_len;
        return true;
    }

    size_t search = template_len;
    while (search > component_start && full_template[search - 1u] != 'X') {
        search--;
    }
    if (search == component_start) {
        bx_diag(diag, "template must contain at least 3 consecutive 'X' characters in the last component: '%s'", original_template);
        return false;
    }

    size_t run_end = search;
    size_t run_start = run_end;
    while (run_start > component_start && full_template[run_start - 1u] == 'X') {
        run_start--;
    }

    size_t run_len = run_end - run_start;
    if (run_len < 3u) {
        bx_diag(diag, "template must contain at least 3 consecutive 'X' characters in the last component: '%s'", original_template);
        return false;
    }

    *prepared_template_out = xstrdup(full_template);
    *x_start_out = run_start;
    *x_len_out = run_len;
    return true;
}

static bool bx_mktemp_try_create(const struct bx_mktemp_options* options, const char* path) {
    if (options->dry_run) {
        struct stat st;
        if (lstat(path, &st) == 0) {
            errno = EEXIST;
            return false;
        }

        if (errno == ENOENT) {
            return true;
        }

        return false;
    }

    if (options->create_directory) {
        return mkdir(path, S_IRWXU) == 0;
    }

    int fd = bx_fd_open_cloexec(path, O_CREAT | O_EXCL | O_RDWR, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        return false;
    }

    if (close(fd) != 0) {
        int saved_errno = errno;
        (void)unlink(path);
        errno = saved_errno;
        return false;
    }

    return true;
}

static bool bx_mktemp_create_path(const struct bx_mktemp_options* options, char* path_out, size_t x_start, size_t x_len) {
    enum { BX_MKTEMP_MAX_ATTEMPTS = 16384 };

    for (int attempt = 0; attempt < BX_MKTEMP_MAX_ATTEMPTS; attempt++) {
        bx_mktemp_fill_random(path_out + x_start, x_len);
        if (bx_mktemp_try_create(options, path_out)) {
            return true;
        }

        if (errno != EEXIST) {
            return false;
        }
    }

    errno = EEXIST;
    return false;
}

int bx_mktemp_main(int argc, char** argv) {
    struct bx_mktemp_options options;
    struct bx_diag_ctx diag = {
        .progname = "mktemp",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };
    int first_operand = 0;

    if (!bx_mktemp_parse_options(argc, argv, &options, &first_operand, &diag)) {
        return diag.exit_status != 0 ? diag.exit_status : 1;
    }

    if (options.show_help) {
        bx_mktemp_print_help(stdout, options.progname);
        return 0;
    }

    if (options.show_version) {
        bx_cli_print_version(options.progname);
        return 0;
    }

    int operand_count = argc - first_operand;
    if (operand_count > 1) {
        bx_diag(&diag, "extra operand '%s'", argv[first_operand + 1]);
        return diag.exit_status;
    }

    const char* template_text = (operand_count == 1) ? argv[first_operand] : "tmp.XXXXXXXXXX";
    bool use_tmpdir = options.template_component_mode || options.tmpdir_specified || operand_count == 0;
    const char* tmpdir = NULL;
    if (use_tmpdir) {
        if (options.template_component_mode) {
            tmpdir = bx_mktemp_resolve_t_tmpdir(&options);
        }
        else {
            tmpdir = bx_mktemp_resolve_tmpdir_option(&options);
        }
    }

    if (options.template_component_mode && strchr(template_text, '/') != NULL) {
        bx_diag(&diag, "invalid template, '%s', contains directory separator", template_text);
        return diag.exit_status;
    }

    if (use_tmpdir && !options.template_component_mode && template_text[0] == '/') {
        bx_diag(&diag, "template must not be absolute with --tmpdir: '%s'", template_text);
        return diag.exit_status;
    }

    char* joined_template = (use_tmpdir && tmpdir != NULL && tmpdir[0] != '\0')
        ? bx_path_join(tmpdir, template_text)
        : xstrdup(template_text);
    char* full_template = NULL;
    size_t x_start = 0;
    size_t x_len = 0;
    if (!bx_mktemp_prepare_template(joined_template, template_text, &options, &full_template, &x_start, &x_len, &diag)) {
        free(joined_template);
        return diag.exit_status;
    }
    free(joined_template);

    if (!bx_mktemp_create_path(&options, full_template, x_start, x_len)) {
        if (!options.quiet) {
            bx_perror_path(&diag, full_template);
        }
        else {
            diag.exit_status = 1;
        }
        free(full_template);
        return diag.exit_status;
    }

    printf("%s\n", full_template);
    free(full_template);
    return 0;
}
