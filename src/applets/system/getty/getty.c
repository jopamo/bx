#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"
#include "bx/libbx.h"

enum bx_getty_clocal_mode {
    BX_GETTY_CLOCAL_AUTO = 0,
    BX_GETTY_CLOCAL_ALWAYS,
    BX_GETTY_CLOCAL_NEVER,
};

struct bx_getty_options {
    const char* progname;
    bool show_help;
    bool show_version;
    bool list_speeds;
    bool show_issue_only;
    bool eight_bits;
    bool flow_control;
    bool noissue;
    bool noclear;
    bool skip_login;
    enum bx_getty_clocal_mode clocal_mode;
    const char* autologin_user;
    const char* login_program;
    const char* login_options;
    unsigned timeout_seconds;
    const char* tty_operand;
    const char* baud_list;
    const char* termtype;
};

struct bx_getty_baud_entry {
    const char* text;
    speed_t speed;
};

static const struct bx_getty_baud_entry bx_getty_baud_table[] = {
    {"0", B0},           {"50", B50},       {"75", B75},        {"110", B110},     {"134", B134},     {"150", B150},     {"200", B200},
    {"300", B300},       {"600", B600},     {"1200", B1200},    {"1800", B1800},   {"2400", B2400},   {"4800", B4800},   {"9600", B9600},
    {"19200", B19200},   {"38400", B38400},
#ifdef B57600
    {"57600", B57600},
#endif
#ifdef B115200
    {"115200", B115200},
#endif
#ifdef B230400
    {"230400", B230400},
#endif
#ifdef B460800
    {"460800", B460800},
#endif
#ifdef B500000
    {"500000", B500000},
#endif
#ifdef B576000
    {"576000", B576000},
#endif
#ifdef B921600
    {"921600", B921600},
#endif
#ifdef B1000000
    {"1000000", B1000000},
#endif
#ifdef B1152000
    {"1152000", B1152000},
#endif
#ifdef B1500000
    {"1500000", B1500000},
#endif
#ifdef B2000000
    {"2000000", B2000000},
#endif
#ifdef B2500000
    {"2500000", B2500000},
#endif
#ifdef B3000000
    {"3000000", B3000000},
#endif
#ifdef B3500000
    {"3500000", B3500000},
#endif
#ifdef B4000000
    {"4000000", B4000000},
#endif
};

static const char* bx_getty_progname(const char* argv0) {
    if (argv0 == NULL || argv0[0] == '\0') {
        return "getty";
    }

    const char* base = strrchr(argv0, '/');
    return (base != NULL && base[1] != '\0') ? base + 1 : argv0;
}

static void bx_getty_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage:\n");
    fprintf(stream, " %s [options] <line> [<baud_rate>,...] [<termtype>]\n", progname);
    fprintf(stream, " %s [options] <baud_rate>,... <line> [<termtype>]\n", progname);
    fprintf(stream, "\n");
    fprintf(stream, "Open a terminal and run a login program.\n");
    fprintf(stream, "\n");
    fprintf(stream, "Options:\n");
    fprintf(stream, " -8, --8bits                assume 8-bit tty\n");
    fprintf(stream, " -a, --autologin <user>     login the specified user automatically\n");
    fprintf(stream, " -h, --flow-control         enable hardware flow control\n");
    fprintf(stream, " -i, --noissue              do not display /etc/issue\n");
    fprintf(stream, " -J, --noclear              do not clear the screen before prompt\n");
    fprintf(stream, " -l, --login-program <file> specify login program\n");
    fprintf(stream, " -L, --local-line[=<mode>]  control the local line flag\n");
    fprintf(stream, " -n, --skip-login           do not prompt for login\n");
    fprintf(stream, " -o, --login-options <opts> options passed to the login program\n");
    fprintf(stream, " -t, --timeout <number>     login process timeout in seconds\n");
    fprintf(stream, "     --show-issue           display /etc/issue and exit\n");
    fprintf(stream, "     --list-speeds          display supported baud rates\n");
    fprintf(stream, "     --help                 display this help\n");
    fprintf(stream, "     --version              display version\n");
}

static void bx_getty_print_try_help(const char* progname) {
    fprintf(stderr, "Try '%s --help' for more information.\n", progname);
}

static void bx_getty_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
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

static bool bx_getty_looks_like_baud_list(const char* text) {
    if (text == NULL || text[0] == '\0') {
        return false;
    }

    bool saw_digit = false;
    for (const char* p = text; *p != '\0'; p++) {
        if (*p == ',') {
            continue;
        }
        if (!isdigit((unsigned char)*p)) {
            return false;
        }
        saw_digit = true;
    }
    return saw_digit;
}

static bool bx_getty_parse_baud_list(const char* text, speed_t* speed_out) {
    if (text == NULL) {
        return false;
    }

    char* copy = xstrdup(text);
    char* saveptr = NULL;
    char* tok = strtok_r(copy, ",", &saveptr);
    if (tok == NULL) {
        free(copy);
        return false;
    }

    speed_t speed = 0;
    bool ok = bx_getty_parse_baud_rate(tok, &speed);
    free(copy);
    if (!ok) {
        return false;
    }

    *speed_out = speed;
    return true;
}

static bool bx_getty_parse_timeout(const char* text, unsigned* timeout_out) {
    if (text == NULL || text[0] == '\0') {
        return false;
    }

    errno = 0;
    char* end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value > UINT_MAX) {
        return false;
    }

    *timeout_out = (unsigned)value;
    return true;
}

static bool bx_getty_parse_local_line(const char* text, enum bx_getty_clocal_mode* mode_out) {
    if (text == NULL || strcmp(text, "always") == 0) {
        *mode_out = BX_GETTY_CLOCAL_ALWAYS;
        return true;
    }
    if (strcmp(text, "auto") == 0) {
        *mode_out = BX_GETTY_CLOCAL_AUTO;
        return true;
    }
    if (strcmp(text, "never") == 0) {
        *mode_out = BX_GETTY_CLOCAL_NEVER;
        return true;
    }
    return false;
}

static bool bx_getty_parse_operands(int argc, char** argv, int start, struct bx_getty_options* options, struct bx_diag_ctx* diag) {
    int remaining = argc - start;
    if (remaining <= 0) {
        bx_diag(diag, "not enough arguments");
        return false;
    }
    if (remaining > 3) {
        bx_diag(diag, "too many arguments");
        return false;
    }

    const char* op1 = argv[start];
    const char* op2 = remaining >= 2 ? argv[start + 1] : NULL;
    const char* op3 = remaining >= 3 ? argv[start + 2] : NULL;

    if (remaining == 1) {
        options->tty_operand = op1;
        return true;
    }

    if (remaining == 2) {
        if (bx_getty_looks_like_baud_list(op1) && !bx_getty_looks_like_baud_list(op2)) {
            options->baud_list = op1;
            options->tty_operand = op2;
        }
        else if (bx_getty_looks_like_baud_list(op2)) {
            options->tty_operand = op1;
            options->baud_list = op2;
        }
        else {
            options->tty_operand = op1;
            options->termtype = op2;
        }
        return true;
    }

    if (bx_getty_looks_like_baud_list(op1) && !bx_getty_looks_like_baud_list(op2)) {
        options->baud_list = op1;
        options->tty_operand = op2;
        options->termtype = op3;
    }
    else {
        options->tty_operand = op1;
        options->baud_list = op2;
        options->termtype = op3;
    }

    return true;
}

static bool bx_getty_parse_options(int argc, char** argv, struct bx_getty_options* options, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"8bits", no_argument, NULL, '8'},
        {"autologin", required_argument, NULL, 'a'},
        {"flow-control", no_argument, NULL, 'h'},
        {"noissue", no_argument, NULL, 'i'},
        {"noclear", no_argument, NULL, 'J'},
        {"login-program", required_argument, NULL, 'l'},
        {"local-line", optional_argument, NULL, 'L'},
        {"skip-login", no_argument, NULL, 'n'},
        {"login-options", required_argument, NULL, 'o'},
        {"timeout", required_argument, NULL, 't'},
        {"show-issue", no_argument, NULL, 1000},
        {"list-speeds", no_argument, NULL, 1001},
        {"help", no_argument, NULL, 1002},
        {"version", no_argument, NULL, 1003},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_getty_progname((argc > 0) ? argv[0] : NULL);
    options->login_program = "/bin/login";
    options->clocal_mode = BX_GETTY_CLOCAL_AUTO;
    diag->progname = options->progname;

    opterr = 0;
    optind = 1;

    while (true) {
        int c = getopt_long(argc, argv, "8a:hiJl:L::no:t:", long_options, NULL);
        if (c == -1) {
            break;
        }

        switch (c) {
            case '8':
                options->eight_bits = true;
                break;
            case 'a':
                if (optarg == NULL || optarg[0] == '\0') {
                    bx_diag(diag, "autologin user may not be empty");
                    return false;
                }
                options->autologin_user = optarg;
                break;
            case 'h':
                options->flow_control = true;
                break;
            case 'i':
                options->noissue = true;
                break;
            case 'J':
                options->noclear = true;
                break;
            case 'l':
                options->login_program = optarg;
                break;
            case 'L':
                if (!bx_getty_parse_local_line(optarg, &options->clocal_mode)) {
                    bx_diag(diag, "invalid local-line mode '%s'", optarg);
                    return false;
                }
                break;
            case 'n':
                options->skip_login = true;
                break;
            case 'o':
                options->login_options = optarg;
                break;
            case 't':
                if (!bx_getty_parse_timeout(optarg, &options->timeout_seconds)) {
                    bx_diag(diag, "invalid timeout '%s'", optarg);
                    return false;
                }
                break;
            case 1000:
                options->show_issue_only = true;
                return true;
            case 1001:
                options->list_speeds = true;
                return true;
            case 1002:
                options->show_help = true;
                return true;
            case 1003:
                options->show_version = true;
                return true;
            case '?':
                if (optind > 0 && optind <= argc && argv[optind - 1] != NULL) {
                    bx_diag(diag, "unrecognized option '%s'", argv[optind - 1]);
                }
                else {
                    bx_diag(diag, "unrecognized option");
                }
                return false;
            case ':':
                if (optind > 0 && optind <= argc && argv[optind - 1] != NULL) {
                    bx_diag(diag, "option requires an argument -- '%s'", argv[optind - 1]);
                }
                else {
                    bx_diag(diag, "option requires an argument");
                }
                return false;
            default:
                return false;
        }
    }

    return bx_getty_parse_operands(argc, argv, optind, options, diag);
}

static char* bx_getty_resolve_tty_path(const char* tty_operand) {
    if (tty_operand == NULL) {
        return NULL;
    }
    if (tty_operand[0] == '/') {
        return xstrdup(tty_operand);
    }

    size_t tty_len = strlen(tty_operand);
    size_t out_len = sizeof("/dev/") - 1 + tty_len;
    char* path = xmalloc(out_len + 1);
    memcpy(path, "/dev/", sizeof("/dev/") - 1);
    memcpy(path + sizeof("/dev/") - 1, tty_operand, tty_len);
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
#ifdef VMIN
    tio->c_cc[VMIN] = (cc_t)1;
#endif
#ifdef VTIME
    tio->c_cc[VTIME] = (cc_t)0;
#endif
}

static void bx_getty_make_termios(struct termios* tio, const struct bx_getty_options* options) {
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

    tio->c_cflag = CREAD | HUPCL | (options->eight_bits ? CS8 : CS8);
#ifdef CRTSCTS
    if (options->flow_control) {
        tio->c_cflag |= CRTSCTS;
    }
#endif

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
    bool clocal_preserved = (tio.c_cflag & CLOCAL) != 0;

    bx_getty_make_termios(&tio, options);

    if (options->clocal_mode == BX_GETTY_CLOCAL_ALWAYS) {
        tio.c_cflag |= CLOCAL;
    }
    else if (options->clocal_mode == BX_GETTY_CLOCAL_NEVER) {
        tio.c_cflag &= (tcflag_t)~CLOCAL;
    }
    else if (clocal_preserved) {
        tio.c_cflag |= CLOCAL;
    }

    if (options->baud_list != NULL) {
        speed_t speed = 0;
        if (!bx_getty_parse_baud_list(options->baud_list, &speed)) {
            bx_diag(diag, "unsupported baud rate '%s'", options->baud_list);
            return false;
        }
        if (cfsetispeed(&tio, speed) != 0 || cfsetospeed(&tio, speed) != 0) {
            bx_diag(diag, "cannot apply baud rate '%s': %s", options->baud_list, strerror(errno));
            return false;
        }
    }
    else if (cfsetispeed(&tio, input_speed) != 0 || cfsetospeed(&tio, output_speed) != 0) {
        bx_diag(diag, "cannot preserve terminal speed: %s", strerror(errno));
        return false;
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
    if (dup2(tty_fd, STDIN_FILENO) < 0 || dup2(tty_fd, STDOUT_FILENO) < 0 || dup2(tty_fd, STDERR_FILENO) < 0) {
        bx_diag(diag, "cannot attach tty to stdio: %s", strerror(errno));
        return false;
    }
    if (tty_fd > STDERR_FILENO) {
        close(tty_fd);
    }
    return true;
}

static bool bx_getty_copy_file_to_stdout(const char* path, struct bx_diag_ctx* diag) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return errno == ENOENT;
    }

    char buf[4096];
    while (true) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n == 0) {
            close(fd);
            return true;
        }
        if (n < 0) {
            bx_diag(diag, "cannot read '%s': %s", path, strerror(errno));
            close(fd);
            return false;
        }
        if (write(STDOUT_FILENO, buf, (size_t)n) != n) {
            bx_diag(diag, "cannot write issue text: %s", strerror(errno));
            close(fd);
            return false;
        }
    }
}

static bool bx_getty_should_clear(const char* tty_path, const struct bx_getty_options* options) {
    if (options->noclear || tty_path == NULL) {
        return false;
    }

    const char* base = strrchr(tty_path, '/');
    base = base != NULL ? base + 1 : tty_path;
    return strncmp(base, "tty", 3) == 0 && isdigit((unsigned char)base[3]);
}

static bool bx_getty_prompt_login_name(char* buffer, size_t buffer_size, struct bx_diag_ctx* diag) {
    if (fputs("login: ", stdout) == EOF || fflush(stdout) == EOF) {
        bx_diag(diag, "cannot write login prompt: %s", strerror(errno));
        return false;
    }

    if (fgets(buffer, (int)buffer_size, stdin) == NULL) {
        if (ferror(stdin)) {
            bx_diag(diag, "cannot read login name: %s", strerror(errno));
        }
        else {
            bx_diag(diag, "no login name received");
        }
        return false;
    }

    size_t len = strlen(buffer);
    while (len > 0 && (buffer[len - 1] == '\n' || buffer[len - 1] == '\r')) {
        buffer[--len] = '\0';
    }
    if (buffer[0] == '\0') {
        bx_diag(diag, "empty login name");
        return false;
    }
    return true;
}

static char* bx_getty_replace_username_token(const char* text, const char* username) {
    const char* user = username != NULL ? username : "";
    size_t user_len = strlen(user);
    size_t text_len = strlen(text);
    size_t out_len = 0;

    for (size_t i = 0; i < text_len; i++) {
        if (text[i] == '\\' && text[i + 1] == 'u') {
            out_len += user_len;
            i++;
        }
        else {
            out_len++;
        }
    }

    char* out = xmalloc(out_len + 1);
    char* p = out;
    for (size_t i = 0; i < text_len; i++) {
        if (text[i] == '\\' && text[i + 1] == 'u') {
            memcpy(p, user, user_len);
            p += user_len;
            i++;
        }
        else {
            *p++ = text[i];
        }
    }
    *p = '\0';
    return out;
}

static char** bx_getty_split_login_options(const char* text, const char* username, int* argc_out) {
    char* copy = xstrdup(text);
    size_t cap = 8;
    size_t argc = 0;
    char** argv = xmalloc((cap + 1) * sizeof(*argv));

    char* saveptr = NULL;
    for (char* tok = strtok_r(copy, " \t\r\n", &saveptr); tok != NULL; tok = strtok_r(NULL, " \t\r\n", &saveptr)) {
        if (argc == cap) {
            cap *= 2;
            argv = xrealloc(argv, (cap + 1) * sizeof(*argv));
        }
        argv[argc++] = bx_getty_replace_username_token(tok, username);
    }

    argv[argc] = NULL;
    *argc_out = (int)argc;
    free(copy);
    return argv;
}

static char** bx_getty_build_login_argv(const struct bx_getty_options* options, const char* username, int* argc_out) {
    if (options->login_options != NULL) {
        int extra_argc = 0;
        char** extra_argv = bx_getty_split_login_options(options->login_options, username, &extra_argc);
        char** argv = xmalloc(((size_t)extra_argc + 2u) * sizeof(*argv));
        argv[0] = xstrdup(options->login_program);
        for (int i = 0; i < extra_argc; i++) {
            argv[i + 1] = extra_argv[i];
        }
        argv[extra_argc + 1] = NULL;
        free(extra_argv);
        *argc_out = extra_argc + 1;
        return argv;
    }

    size_t argc = 1;
    if (username != NULL) {
        argc += options->autologin_user != NULL ? 3 : 2;
    }

    char** argv = xmalloc((argc + 1) * sizeof(*argv));
    size_t i = 0;
    argv[i++] = xstrdup(options->login_program);
    if (username != NULL) {
        if (options->autologin_user != NULL) {
            argv[i++] = xstrdup("-f");
        }
        argv[i++] = xstrdup("--");
        argv[i++] = xstrdup(username);
    }
    argv[i] = NULL;
    *argc_out = (int)i;
    return argv;
}

static void bx_getty_free_login_argv(char** argv, int argc) {
    if (argv == NULL) {
        return;
    }
    for (int i = 0; i < argc; i++) {
        free(argv[i]);
    }
    free(argv);
}

static int bx_getty_exec_login(const struct bx_getty_options* options, struct bx_diag_ctx* diag) {
    char* tty_path = bx_getty_resolve_tty_path(options->tty_operand);
    int tty_fd = open(tty_path, O_RDWR | O_NOCTTY);
    if (tty_fd < 0) {
        bx_diag(diag, "cannot open tty '%s': %s", tty_path, strerror(errno));
        free(tty_path);
        return 1;
    }

    if (!bx_getty_prepare_tty(tty_fd, options, diag) || !bx_getty_attach_stdio(tty_fd, diag)) {
        close(tty_fd);
        free(tty_path);
        return 1;
    }

    if (options->timeout_seconds > 0) {
        alarm(options->timeout_seconds);
    }

    if (options->termtype != NULL && setenv("TERM", options->termtype, 1) != 0) {
        bx_diag(diag, "cannot set TERM='%s': %s", options->termtype, strerror(errno));
        free(tty_path);
        return 1;
    }

    if (bx_getty_should_clear(tty_path, options)) {
        write(STDOUT_FILENO, "\033[H\033[J", 6);
    }

    if (!options->noissue && !bx_getty_copy_file_to_stdout("/etc/issue", diag)) {
        free(tty_path);
        return 1;
    }

    char username_buf[256];
    const char* username = NULL;
    if (options->autologin_user != NULL) {
        username = options->autologin_user;
    }
    else if (!options->skip_login) {
        if (!bx_getty_prompt_login_name(username_buf, sizeof(username_buf), diag)) {
            free(tty_path);
            return 1;
        }
        username = username_buf;
    }

    int login_argc = 0;
    char** login_argv = bx_getty_build_login_argv(options, username, &login_argc);

    if (strchr(options->login_program, '/') != NULL) {
        execv(options->login_program, login_argv);
    }
    else {
        execvp(options->login_program, login_argv);
    }

    bx_diag(diag, "cannot execute '%s': %s", options->login_program, strerror(errno));
    bx_getty_free_login_argv(login_argv, login_argc);
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
        bx_getty_print_try_help(options.progname != NULL ? options.progname : "getty");
        return 1;
    }

    if (options.show_help) {
        bx_getty_print_help(stdout, options.progname);
        return 0;
    }
    if (options.show_version) {
        bx_getty_print_version(options.progname);
        return 0;
    }
    if (options.list_speeds) {
        for (size_t i = 0; i < sizeof(bx_getty_baud_table) / sizeof(bx_getty_baud_table[0]); i++) {
            printf("%10s\n", bx_getty_baud_table[i].text);
        }
        return 0;
    }
    if (options.show_issue_only) {
        return bx_getty_copy_file_to_stdout("/etc/issue", &diag) ? 0 : 1;
    }

    return bx_getty_exec_login(&options, &diag);
}
