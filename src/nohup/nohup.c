#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "applets.h"
#include "diag.h"
#include "libbx.h"

struct bx_nohup_options {
    const char* progname;
    bool show_help;
    bool show_version;
    int first_operand;
};

static const char* bx_nohup_progname(const char* argv0) {
    if (argv0 == NULL || argv0[0] == '\0') {
        return "nohup";
    }

    const char* base = strrchr(argv0, '/');
    if (base != NULL && base[1] != '\0') {
        return base + 1;
    }
    return argv0;
}

static void bx_nohup_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s COMMAND [ARG]...\n", progname);
    fprintf(stream, "  or:  %s OPTION\n", progname);
    fprintf(stream, "Run COMMAND, ignoring hangup signals.\n");
    fprintf(stream, "\n");
    fprintf(stream, "      --help     display this help and exit\n");
    fprintf(stream, "      --version  output version information and exit\n");
}

static void bx_nohup_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

static void bx_nohup_print_try_help(const char* progname) {
    fprintf(stderr, "Try '%s --help' for more information.\n", progname);
}

static void bx_nohup_notice(const char* progname, const char* fmt, ...) {
    va_list ap;
    fprintf(stderr, "%s: ", progname);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static bool bx_nohup_parse_options(int argc, char** argv, struct bx_nohup_options* options, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"help", no_argument, NULL, 1},
        {"version", no_argument, NULL, 2},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_nohup_progname((argc > 0) ? argv[0] : NULL);
    diag->progname = options->progname;

    opterr = 0;
    optind = 1;

    while (true) {
        int c = getopt_long(argc, argv, "+", long_options, NULL);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 1:
                options->show_help = true;
                return true;
            case 2:
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

    options->first_operand = optind;
    return true;
}

static char* bx_nohup_home_output_path(const char* home) {
    size_t home_len = strlen(home);
    bool need_slash = (home_len > 0 && home[home_len - 1] != '/');
    size_t total = home_len + (need_slash ? 1u : 0u) + sizeof("nohup.out");

    char* path = xmalloc(total);
    memcpy(path, home, home_len);

    size_t pos = home_len;
    if (need_slash) {
        path[pos++] = '/';
    }
    memcpy(path + pos, "nohup.out", sizeof("nohup.out"));
    return path;
}

static bool bx_nohup_open_output_file(int* output_fd, char** output_path, struct bx_diag_ctx* diag) {
    const char default_output[] = "nohup.out";
    int fd = open(default_output, O_WRONLY | O_CREAT | O_APPEND, S_IRUSR | S_IWUSR);
    if (fd >= 0) {
        *output_fd = fd;
        *output_path = xstrdup(default_output);
        return true;
    }

    const char* home = getenv("HOME");
    if (home == NULL || home[0] == '\0') {
        bx_diag(diag, "failed to open '%s': %s", default_output, strerror(errno));
        return false;
    }

    char* home_output = bx_nohup_home_output_path(home);
    fd = open(home_output, O_WRONLY | O_CREAT | O_APPEND, S_IRUSR | S_IWUSR);
    if (fd >= 0) {
        *output_fd = fd;
        *output_path = home_output;
        return true;
    }

    bx_diag(diag, "failed to open '%s': %s", home_output, strerror(errno));
    free(home_output);
    return false;
}

static int bx_nohup_setup_standard_streams(const struct bx_nohup_options* options, struct bx_diag_ctx* diag) {
    if (isatty(STDIN_FILENO)) {
        bx_nohup_notice(options->progname, "ignoring input");

        int null_fd = open("/dev/null", O_RDONLY);
        if (null_fd < 0) {
            bx_diag(diag, "failed to open /dev/null: %s", strerror(errno));
            return 125;
        }

        if (dup2(null_fd, STDIN_FILENO) < 0) {
            bx_diag(diag, "failed to redirect standard input: %s", strerror(errno));
            close(null_fd);
            return 125;
        }

        if (close(null_fd) != 0) {
            bx_diag(diag, "failed to close /dev/null: %s", strerror(errno));
            return 125;
        }
    }

    bool stderr_is_tty = (isatty(STDERR_FILENO) != 0);

    if (isatty(STDOUT_FILENO)) {
        int output_fd = -1;
        char* output_path = NULL;
        if (!bx_nohup_open_output_file(&output_fd, &output_path, diag)) {
            return 125;
        }

        bx_nohup_notice(options->progname, "appending output to '%s'", output_path);

        if (dup2(output_fd, STDOUT_FILENO) < 0) {
            bx_diag(diag, "failed to redirect standard output: %s", strerror(errno));
            close(output_fd);
            free(output_path);
            return 125;
        }

        if (close(output_fd) != 0) {
            bx_diag(diag, "failed to close output file '%s': %s", output_path, strerror(errno));
            free(output_path);
            return 125;
        }

        free(output_path);
    }

    if (stderr_is_tty) {
        if (dup2(STDOUT_FILENO, STDERR_FILENO) < 0) {
            bx_diag(diag, "failed to redirect standard error: %s", strerror(errno));
            return 125;
        }
    }

    return 0;
}

static int bx_nohup_execute_command(char** command_argv, struct bx_diag_ctx* diag) {
    execvp(command_argv[0], command_argv);

    int exec_error = errno;
    bx_diag(diag, "failed to run command '%s': %s", command_argv[0], strerror(exec_error));
    if (exec_error == ENOENT) {
        return 127;
    }
    return 126;
}

int bx_nohup_main(int argc, char** argv) {
    struct bx_nohup_options options;
    struct bx_diag_ctx diag = {
        .progname = "nohup",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };

    if (!bx_nohup_parse_options(argc, argv, &options, &diag)) {
        bx_nohup_print_try_help(options.progname);
        return 125;
    }

    if (options.show_help) {
        bx_nohup_print_help(stdout, options.progname);
        return 0;
    }

    if (options.show_version) {
        bx_nohup_print_version(options.progname);
        return 0;
    }

    if (options.first_operand >= argc) {
        bx_diag(&diag, "missing operand");
        bx_nohup_print_try_help(options.progname);
        return 125;
    }

    if (bx_nohup_setup_standard_streams(&options, &diag) != 0) {
        return 125;
    }

    if (signal(SIGHUP, SIG_IGN) == SIG_ERR) {
        bx_diag(&diag, "failed to ignore SIGHUP: %s", strerror(errno));
        return 125;
    }

    return bx_nohup_execute_command(argv + options.first_operand, &diag);
}
