#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "applets.h"
#include "diag.h"
#include "libbx.h"

struct bx_getty_options {
    const char* progname;
    bool show_help;
    bool show_version;
    const char* tty_operand;
    const char* term_name;
    const char* baud_rate;
    int command_index;
};

struct bx_getty_baud_entry {
    const char* text;
    speed_t speed;
};

static const struct bx_getty_baud_entry bx_getty_baud_table[] = {
    {"0", B0},           {"50", B50},     {"75", B75},     {"110", B110},   {"134", B134},   {"150", B150},   {"200", B200},     {"300", B300},
    {"600", B600},       {"1200", B1200}, {"1800", B1800}, {"2400", B2400}, {"4800", B4800}, {"9600", B9600}, {"19200", B19200}, {"38400", B38400},
#ifdef B57600
    {"57600", B57600},
#endif
#ifdef B115200
    {"115200", B115200},
#endif
#ifdef B230400
    {"230400", B230400},
#endif
};

static const char* bx_getty_progname(const char* argv0) {
    if (argv0 == NULL || argv0[0] == '\0') {
        return "getty";
    }

    const char* base = strrchr(argv0, '/');
    if (base != NULL && base[1] != '\0') {
        return base + 1;
    }

    return argv0;
}

static void bx_getty_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... TTY PROGRAM [ARG]...\n", progname);
    fprintf(stream, "Open and initialize TTY, then exec PROGRAM directly.\n");
    fprintf(stream, "\n");
    fprintf(stream, "This phase does not authenticate users. Use an external login program\n");
    fprintf(stream, "or run a direct shell command explicitly for rescue use.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -s, --baud=BAUD   set terminal speed (for serial consoles)\n");
    fprintf(stream, "  -t, --term=TERM   set TERM for the exec'd program\n");
    fprintf(stream, "  -h, --help        display this help and exit\n");
    fprintf(stream, "  -V, --version     output version information and exit\n");
}

static void bx_getty_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

static void bx_getty_print_try_help(const char* progname) {
    fprintf(stderr, "Try '%s --help' for more information.\n", progname);
}

static bool bx_getty_parse_options(int argc, char** argv, struct bx_getty_options* options, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"baud", required_argument, NULL, 's'}, {"term", required_argument, NULL, 't'}, {"help", no_argument, NULL, 'h'}, {"version", no_argument, NULL, 'V'}, {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_getty_progname((argc > 0) ? argv[0] : NULL);
    diag->progname = options->progname;

    opterr = 0;
    optind = 1;

    while (true) {
        int c = getopt_long(argc, argv, "+s:t:hV", long_options, NULL);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 's':
                options->baud_rate = optarg;
                break;
            case 't':
                options->term_name = optarg;
                break;
            case 'h':
                options->show_help = true;
                return true;
            case 'V':
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

    if (optind >= argc) {
        bx_diag(diag, "missing operand: TTY");
        return false;
    }
    options->tty_operand = argv[optind++];

    if (optind >= argc) {
        bx_diag(diag, "missing operand: PROGRAM");
        return false;
    }
    options->command_index = optind;

    return true;
}

static bool bx_getty_parse_baud_rate(const char* text, speed_t* speed_out) {
    for (size_t i = 0; i < sizeof(bx_getty_baud_table) / sizeof(bx_getty_baud_table[0]); i++) {
        if (strcmp(text, bx_getty_baud_table[i].text) == 0) {
            *speed_out = bx_getty_baud_table[i].speed;
            return true;
        }
    }

    return false;
}

static char* bx_getty_resolve_tty_path(const char* tty_operand) {
    if (tty_operand[0] == '/') {
        return xstrdup(tty_operand);
    }

    size_t tty_len = strlen(tty_operand);
    size_t out_len = sizeof("/dev/") - 1 + tty_len;
    char* path = xmalloc(out_len + 1);

    memcpy(path, "/dev/", sizeof("/dev/") - 1);
    memcpy(path + (sizeof("/dev/") - 1), tty_operand, tty_len);
    path[out_len] = '\0';
    return path;
}

static void bx_getty_set_sane_control_chars(struct termios* tio) {
    for (size_t i = 0; i < NCCS; i++) {
        tio->c_cc[i] = _POSIX_VDISABLE;
    }

#ifdef VINTR
    tio->c_cc[VINTR] = (cc_t)003;
#endif
#ifdef VQUIT
    tio->c_cc[VQUIT] = (cc_t)034;
#endif
#ifdef VERASE
    tio->c_cc[VERASE] = (cc_t)0177;
#endif
#ifdef VKILL
    tio->c_cc[VKILL] = (cc_t)025;
#endif
#ifdef VEOF
    tio->c_cc[VEOF] = (cc_t)004;
#endif
#ifdef VSTART
    tio->c_cc[VSTART] = (cc_t)021;
#endif
#ifdef VSTOP
    tio->c_cc[VSTOP] = (cc_t)023;
#endif
#ifdef VSUSP
    tio->c_cc[VSUSP] = (cc_t)032;
#endif
#ifdef VMIN
    tio->c_cc[VMIN] = (cc_t)1;
#endif
#ifdef VTIME
    tio->c_cc[VTIME] = (cc_t)0;
#endif
}

static void bx_getty_set_sane_termios(struct termios* tio) {
    tio->c_iflag = BRKINT | ICRNL | IXON;
#ifdef IMAXBEL
    tio->c_iflag |= IMAXBEL;
#endif
#ifdef IUTF8
    tio->c_iflag |= IUTF8;
#endif

    tio->c_oflag = OPOST;
#ifdef ONLCR
    tio->c_oflag |= ONLCR;
#endif

    tio->c_cflag = CREAD | CS8 | HUPCL;

    tio->c_lflag = ISIG | ICANON | IEXTEN | ECHO | ECHOE | ECHOK;
#ifdef ECHOCTL
    tio->c_lflag |= ECHOCTL;
#endif
#ifdef ECHOKE
    tio->c_lflag |= ECHOKE;
#endif

    bx_getty_set_sane_control_chars(tio);
}

static bool bx_getty_prepare_tty(int tty_fd, const struct bx_getty_options* options, struct bx_diag_ctx* diag) {
    if (setsid() == -1) {
        bx_diag(diag, "cannot create a new session: %s", strerror(errno));
        return false;
    }

    if (ioctl(tty_fd, TIOCSCTTY, 0) != 0) {
        bx_diag(diag, "cannot set controlling terminal: %s", strerror(errno));
        return false;
    }

    struct termios tio;
    if (tcgetattr(tty_fd, &tio) != 0) {
        bx_diag(diag, "cannot read terminal attributes: %s", strerror(errno));
        return false;
    }

    speed_t input_speed = cfgetispeed(&tio);
    speed_t output_speed = cfgetospeed(&tio);

    bx_getty_set_sane_termios(&tio);

    if (options->baud_rate != NULL) {
        speed_t speed = 0;
        if (!bx_getty_parse_baud_rate(options->baud_rate, &speed)) {
            bx_diag(diag, "unsupported baud rate '%s'", options->baud_rate);
            return false;
        }

        if (cfsetispeed(&tio, speed) != 0 || cfsetospeed(&tio, speed) != 0) {
            bx_diag(diag, "cannot apply baud rate '%s': %s", options->baud_rate, strerror(errno));
            return false;
        }
    }
    else {
        if (cfsetispeed(&tio, input_speed) != 0 || cfsetospeed(&tio, output_speed) != 0) {
            bx_diag(diag, "cannot preserve terminal speed: %s", strerror(errno));
            return false;
        }
    }

    if (tcsetattr(tty_fd, TCSANOW, &tio) != 0) {
        bx_diag(diag, "cannot apply terminal attributes: %s", strerror(errno));
        return false;
    }

    if (tcsetpgrp(tty_fd, getpgrp()) != 0) {
        bx_diag(diag, "cannot place tty in foreground for this process group: %s", strerror(errno));
        return false;
    }

    return true;
}

static bool bx_getty_attach_stdio(int tty_fd, struct bx_diag_ctx* diag) {
    if (dup2(tty_fd, STDIN_FILENO) < 0) {
        bx_diag(diag, "cannot bind tty to stdin: %s", strerror(errno));
        return false;
    }
    if (dup2(tty_fd, STDOUT_FILENO) < 0) {
        bx_diag(diag, "cannot bind tty to stdout: %s", strerror(errno));
        return false;
    }
    if (dup2(tty_fd, STDERR_FILENO) < 0) {
        bx_diag(diag, "cannot bind tty to stderr: %s", strerror(errno));
        return false;
    }

    if (tty_fd > STDERR_FILENO) {
        close(tty_fd);
    }
    return true;
}

static int bx_getty_exec_program(const struct bx_getty_options* options, int argc, char** argv, struct bx_diag_ctx* diag) {
    (void)argc;

    char* tty_path = bx_getty_resolve_tty_path(options->tty_operand);
    int tty_fd = open(tty_path, O_RDWR | O_NOCTTY);
    if (tty_fd < 0) {
        bx_diag(diag, "cannot open tty '%s': %s", tty_path, strerror(errno));
        free(tty_path);
        return 1;
    }

    if (!bx_getty_prepare_tty(tty_fd, options, diag)) {
        close(tty_fd);
        free(tty_path);
        return 1;
    }

    if (!bx_getty_attach_stdio(tty_fd, diag)) {
        close(tty_fd);
        free(tty_path);
        return 1;
    }

    if (options->term_name != NULL && setenv("TERM", options->term_name, 1) != 0) {
        bx_diag(diag, "cannot set TERM='%s': %s", options->term_name, strerror(errno));
        free(tty_path);
        return 1;
    }

    execvp(argv[options->command_index], &argv[options->command_index]);
    bx_diag(diag, "cannot execute '%s': %s", argv[options->command_index], strerror(errno));
    free(tty_path);
    return 1;
}

int bx_getty_main(int argc, char** argv) {
    struct bx_getty_options options;
    struct bx_diag_ctx diag = {
        .progname = "getty",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };

    if (!bx_getty_parse_options(argc, argv, &options, &diag)) {
        bx_getty_print_try_help(options.progname);
        return 2;
    }

    if (options.show_help) {
        bx_getty_print_help(stdout, options.progname);
        return 0;
    }

    if (options.show_version) {
        bx_getty_print_version(options.progname);
        return 0;
    }

    return bx_getty_exec_program(&options, argc, argv, &diag);
}
