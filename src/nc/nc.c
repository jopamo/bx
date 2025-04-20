#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "applets.h"
#include "common/xreadwrite.h"
#include "diag.h"

#ifndef NI_MAXHOST
#define NI_MAXHOST 1025
#endif

#ifndef NI_MAXSERV
#define NI_MAXSERV 32
#endif

struct bx_nc_options {
    const char* progname;
    bool show_help;
    bool show_version;
    bool listen_mode;
    bool udp_mode;
    bool verbose;
    bool zero_io;
    bool shutdown_on_eof;
    bool numeric_only;
    bool keep_open;
    bool detach_stdin;
    int timeout_seconds;
    const char* source_addr;
    const char* source_port;
    const char* host;
    const char* port;
};

static const char* bx_nc_progname(const char* argv0) {
    if (argv0 == NULL || argv0[0] == '\0') {
        return "nc";
    }

    const char* base = strrchr(argv0, '/');
    if (base != NULL && base[1] != '\0') {
        return base + 1;
    }
    return argv0;
}

static void bx_nc_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTIONS] HOST PORT\n", progname);
    fprintf(stream, "       %s [OPTIONS] -l [HOST] PORT\n", progname);
    fprintf(stream, "\n");
    fprintf(stream, "IPv4-only netcat applet merged from the bx sibling netcat project.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -4               force IPv4 (default)\n");
    fprintf(stream, "  -6               unsupported in this build\n");
    fprintf(stream, "  -d               detach from stdin\n");
    fprintf(stream, "  -h, --help       display this help and exit\n");
    fprintf(stream, "  -k               keep accepting connections in listen mode (TCP only)\n");
    fprintf(stream, "  -l               listen mode\n");
    fprintf(stream, "  -N               shutdown socket write side after stdin EOF\n");
    fprintf(stream, "  -n               numeric-only host/port resolution\n");
    fprintf(stream, "  -p PORT          source port (connect mode only)\n");
    fprintf(stream, "  -s ADDR          source address (connect mode) or bind address (listen mode)\n");
    fprintf(stream, "  -u               UDP mode\n");
    fprintf(stream, "  -v               verbose diagnostics\n");
    fprintf(stream, "  -w SECONDS       connect/read timeout\n");
    fprintf(stream, "  -z               zero-I/O mode (connect/scan only)\n");
    fprintf(stream, "      --version    output version information and exit\n");
}

static void bx_nc_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

static void bx_nc_print_try_help(const char* progname) {
    fprintf(stderr, "Try '%s --help' for more information.\n", progname);
}

static bool bx_nc_parse_int_range(const char* text, int min_value, int max_value, int* value_out) {
    if (text == NULL || text[0] == '\0') {
        return false;
    }

    errno = 0;
    char* end = NULL;
    long value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        return false;
    }
    if (value < min_value || value > max_value) {
        return false;
    }

    *value_out = (int)value;
    return true;
}

static bool bx_nc_parse_options(int argc, char** argv, struct bx_nc_options* options, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"help", no_argument, NULL, 1},
        {"version", no_argument, NULL, 2},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_nc_progname((argc > 0) ? argv[0] : NULL);
    options->timeout_seconds = -1;
    diag->progname = options->progname;

    opterr = 0;
    optind = 1;

    while (true) {
        int c = getopt_long(argc, argv, "+46dhklNnuvzw:p:s:", long_options, NULL);
        if (c == -1) {
            break;
        }

        switch (c) {
            case '4':
                break;
            case '6':
                bx_diag(diag, "IPv6 is not supported in this IPv4-only build");
                return false;
            case 'd':
                options->detach_stdin = true;
                break;
            case 'h':
                options->show_help = true;
                return true;
            case 'k':
                options->keep_open = true;
                break;
            case 'l':
                options->listen_mode = true;
                break;
            case 'N':
                options->shutdown_on_eof = true;
                break;
            case 'n':
                options->numeric_only = true;
                break;
            case 'u':
                options->udp_mode = true;
                break;
            case 'v':
                options->verbose = true;
                break;
            case 'w':
                if (!bx_nc_parse_int_range(optarg, 0, INT_MAX, &options->timeout_seconds)) {
                    bx_diag(diag, "invalid timeout '%s'", optarg != NULL ? optarg : "");
                    return false;
                }
                break;
            case 'z':
                options->zero_io = true;
                break;
            case 'p':
                options->source_port = optarg;
                break;
            case 's':
                options->source_addr = optarg;
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
                else if (optind > 0 && optind <= argc && argv[optind - 1] != NULL) {
                    bx_diag(diag, "option requires an argument -- '%s'", argv[optind - 1]);
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

    int remaining = argc - optind;

    if (options->listen_mode) {
        if (remaining == 0) {
            bx_diag(diag, "missing port in listen mode");
            return false;
        }

        if (remaining > 2) {
            bx_diag(diag, "extra operand '%s'", argv[optind + 2]);
            return false;
        }

        if (remaining == 2) {
            if (options->source_addr != NULL) {
                bx_diag(diag, "cannot combine listen HOST operand with -s");
                return false;
            }
            options->host = argv[optind];
            options->port = argv[optind + 1];
        }
        else {
            options->host = options->source_addr;
            options->port = argv[optind];
        }

        if (options->source_port != NULL) {
            bx_diag(diag, "-p is not valid with -l; specify listen port as operand");
            return false;
        }

        if (options->zero_io) {
            bx_diag(diag, "-z cannot be used with -l");
            return false;
        }

        if (options->keep_open && options->udp_mode) {
            bx_diag(diag, "-k is only supported for TCP listen mode");
            return false;
        }
    }
    else {
        if (options->keep_open) {
            bx_diag(diag, "-k requires -l");
            return false;
        }

        if (remaining < 2) {
            bx_diag(diag, "missing host and port operands");
            return false;
        }

        if (remaining > 2) {
            bx_diag(diag, "extra operand '%s'", argv[optind + 2]);
            return false;
        }

        options->host = argv[optind];
        options->port = argv[optind + 1];
    }

    return true;
}

static int bx_nc_timeout_ms(int timeout_seconds) {
    if (timeout_seconds < 0) {
        return -1;
    }
    if (timeout_seconds > INT_MAX / 1000) {
        return INT_MAX;
    }
    return timeout_seconds * 1000;
}

static bool bx_nc_bind_local(int fd, const char* host, const char* port, int socktype, int protocol, bool numeric_only) {
    if (host == NULL && port == NULL) {
        return true;
    }

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = socktype;
    hints.ai_protocol = protocol;
    hints.ai_flags = AI_PASSIVE;

    if (numeric_only) {
        if (host != NULL) {
            hints.ai_flags |= AI_NUMERICHOST;
        }
        if (port != NULL) {
            hints.ai_flags |= AI_NUMERICSERV;
        }
    }

    struct addrinfo* res0 = NULL;
    int gai_rc = getaddrinfo(host, port, &hints, &res0);
    if (gai_rc != 0) {
        errno = EINVAL;
        return false;
    }

    bool bound = false;
    int saved_errno = EADDRNOTAVAIL;

    for (struct addrinfo* res = res0; res != NULL; res = res->ai_next) {
        if (bind(fd, res->ai_addr, res->ai_addrlen) == 0) {
            bound = true;
            break;
        }
        saved_errno = errno;
    }

    freeaddrinfo(res0);

    if (!bound) {
        errno = saved_errno;
    }
    return bound;
}

static bool bx_nc_connect_with_timeout(int fd, const struct sockaddr* addr, socklen_t addr_len, int timeout_seconds) {
    if (timeout_seconds < 0) {
        return connect(fd, addr, addr_len) == 0;
    }

    int original_flags = fcntl(fd, F_GETFL, 0);
    if (original_flags < 0) {
        return false;
    }

    if (fcntl(fd, F_SETFL, original_flags | O_NONBLOCK) != 0) {
        return false;
    }

    bool ok = false;

    if (connect(fd, addr, addr_len) == 0) {
        ok = true;
    }
    else if (errno == EINPROGRESS) {
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLOUT;
        pfd.revents = 0;

        int poll_rc;
        do {
            poll_rc = poll(&pfd, 1, bx_nc_timeout_ms(timeout_seconds));
        } while (poll_rc < 0 && errno == EINTR);

        if (poll_rc > 0) {
            int sock_error = 0;
            socklen_t sock_error_len = sizeof(sock_error);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &sock_error, &sock_error_len) == 0 && sock_error == 0) {
                ok = true;
            }
            else if (sock_error != 0) {
                errno = sock_error;
            }
        }
        else if (poll_rc == 0) {
            errno = ETIMEDOUT;
        }
    }

    if (fcntl(fd, F_SETFL, original_flags) != 0 && ok) {
        return false;
    }

    return ok;
}

static bool bx_nc_format_sockaddr(const struct sockaddr* addr, socklen_t addr_len, char* host, size_t host_len, char* port, size_t port_len) {
    int rc = getnameinfo(addr, addr_len, host, host_len, port, port_len, NI_NUMERICHOST | NI_NUMERICSERV);
    return rc == 0;
}

static void bx_nc_report_local_listen(int fd, const struct bx_nc_options* options) {
    if (!options->verbose) {
        return;
    }

    struct sockaddr_storage ss;
    socklen_t ss_len = sizeof(ss);

    if (getsockname(fd, (struct sockaddr*)&ss, &ss_len) != 0) {
        return;
    }

    char host[NI_MAXHOST];
    char port[NI_MAXSERV];
    if (!bx_nc_format_sockaddr((struct sockaddr*)&ss, ss_len, host, sizeof(host), port, sizeof(port))) {
        return;
    }

    fprintf(stderr, "Listening on %s:%s\n", host, port);
}

static void bx_nc_report_connected(int fd, const struct bx_nc_options* options) {
    if (!options->verbose) {
        return;
    }

    struct sockaddr_storage ss;
    socklen_t ss_len = sizeof(ss);

    if (getpeername(fd, (struct sockaddr*)&ss, &ss_len) != 0) {
        return;
    }

    char host[NI_MAXHOST];
    char port[NI_MAXSERV];
    if (!bx_nc_format_sockaddr((struct sockaddr*)&ss, ss_len, host, sizeof(host), port, sizeof(port))) {
        return;
    }

    fprintf(stderr, "Connected to %s:%s\n", host, port);
}

static void bx_nc_report_peer(const struct sockaddr* addr, socklen_t addr_len, const struct bx_nc_options* options) {
    if (!options->verbose) {
        return;
    }

    char host[NI_MAXHOST];
    char port[NI_MAXSERV];
    if (!bx_nc_format_sockaddr(addr, addr_len, host, sizeof(host), port, sizeof(port))) {
        return;
    }

    fprintf(stderr, "Connection from %s:%s\n", host, port);
}

#ifdef MSG_NOSIGNAL
enum { BX_NC_SEND_FLAGS = MSG_NOSIGNAL };
#else
enum { BX_NC_SEND_FLAGS = 0 };
#endif

static bool bx_nc_send_all(int fd, const unsigned char* data, size_t data_len) {
    size_t written = 0;

    while (written < data_len) {
        ssize_t n = send(fd, data + written, data_len - written, BX_NC_SEND_FLAGS);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            errno = EPIPE;
            return false;
        }
        written += (size_t)n;
    }

    return true;
}

static int bx_nc_relay_connected_socket(int fd, const struct bx_nc_options* options, bool stdin_enabled, struct bx_diag_ctx* diag) {
    unsigned char io_buffer[16384];
    bool stdin_open = stdin_enabled;
    bool socket_open = true;
    int timeout_ms = bx_nc_timeout_ms(options->timeout_seconds);

    while (socket_open) {
        struct pollfd fds[2];
        nfds_t nfds = 0;
        int stdin_index = -1;
        int socket_index = -1;

        if (stdin_open) {
            stdin_index = (int)nfds;
            fds[nfds].fd = STDIN_FILENO;
            fds[nfds].events = POLLIN;
            fds[nfds].revents = 0;
            nfds++;
        }

        socket_index = (int)nfds;
        fds[nfds].fd = fd;
        fds[nfds].events = POLLIN;
        fds[nfds].revents = 0;
        nfds++;

        int poll_rc;
        do {
            poll_rc = poll(fds, nfds, timeout_ms);
        } while (poll_rc < 0 && errno == EINTR);

        if (poll_rc < 0) {
            bx_diag(diag, "poll failed: %s", strerror(errno));
            return 1;
        }

        if (poll_rc == 0) {
            return 0;
        }

        if (stdin_index >= 0) {
            short stdin_events = fds[stdin_index].revents;

            if ((stdin_events & POLLIN) != 0) {
                ssize_t nread = bx_xread(STDIN_FILENO, io_buffer, sizeof(io_buffer));
                if (nread < 0) {
                    bx_diag(diag, "stdin read failed: %s", strerror(errno));
                    return 1;
                }

                if (nread == 0) {
                    stdin_open = false;
                    if (options->shutdown_on_eof && !options->udp_mode) {
                        shutdown(fd, SHUT_WR);
                    }
                }
                else if (!bx_nc_send_all(fd, io_buffer, (size_t)nread)) {
                    if (errno == EPIPE || errno == ECONNRESET) {
                        socket_open = false;
                    }
                    else {
                        bx_diag(diag, "socket write failed: %s", strerror(errno));
                        return 1;
                    }
                }
            }

            if ((stdin_events & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
                stdin_open = false;
                if (options->shutdown_on_eof && !options->udp_mode) {
                    shutdown(fd, SHUT_WR);
                }
            }
        }

        short socket_events = fds[socket_index].revents;

        if ((socket_events & POLLIN) != 0) {
            ssize_t nread = recv(fd, io_buffer, sizeof(io_buffer), 0);
            if (nread < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                    continue;
                }
                bx_diag(diag, "socket read failed: %s", strerror(errno));
                return 1;
            }

            if (nread == 0) {
                socket_open = false;
            }
            else if (!options->zero_io && !bx_xwrite_all(STDOUT_FILENO, io_buffer, (size_t)nread)) {
                bx_diag(diag, "stdout write failed: %s", strerror(errno));
                return 1;
            }
        }

        if ((socket_events & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
            socket_open = false;
        }
    }

    return 0;
}

static int bx_nc_open_client_socket(const struct bx_nc_options* options, struct bx_diag_ctx* diag) {
    int socktype = options->udp_mode ? SOCK_DGRAM : SOCK_STREAM;
    int protocol = options->udp_mode ? IPPROTO_UDP : IPPROTO_TCP;

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = socktype;
    hints.ai_protocol = protocol;

    if (options->numeric_only) {
        hints.ai_flags |= AI_NUMERICHOST | AI_NUMERICSERV;
    }

    struct addrinfo* res0 = NULL;
    int gai_rc = getaddrinfo(options->host, options->port, &hints, &res0);
    if (gai_rc != 0) {
        bx_diag(diag, "getaddrinfo('%s', '%s') failed: %s", options->host, options->port, gai_strerror(gai_rc));
        return -1;
    }

    int fd = -1;
    int saved_errno = ECONNREFUSED;

    for (struct addrinfo* res = res0; res != NULL; res = res->ai_next) {
        fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (fd < 0) {
            saved_errno = errno;
            continue;
        }

        if (!bx_nc_bind_local(fd, options->source_addr, options->source_port, res->ai_socktype, res->ai_protocol, options->numeric_only)) {
            saved_errno = errno;
            close(fd);
            fd = -1;
            continue;
        }

        if (bx_nc_connect_with_timeout(fd, res->ai_addr, res->ai_addrlen, options->timeout_seconds)) {
            break;
        }

        saved_errno = errno;
        close(fd);
        fd = -1;
    }

    freeaddrinfo(res0);

    if (fd < 0) {
        errno = saved_errno;
        bx_diag(diag, "connect failed: %s", strerror(errno));
        return -1;
    }

    return fd;
}

static int bx_nc_open_listener_socket(const struct bx_nc_options* options, struct bx_diag_ctx* diag) {
    int socktype = options->udp_mode ? SOCK_DGRAM : SOCK_STREAM;
    int protocol = options->udp_mode ? IPPROTO_UDP : IPPROTO_TCP;

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = socktype;
    hints.ai_protocol = protocol;
    hints.ai_flags = AI_PASSIVE;

    if (options->numeric_only) {
        if (options->host != NULL) {
            hints.ai_flags |= AI_NUMERICHOST;
        }
        hints.ai_flags |= AI_NUMERICSERV;
    }

    struct addrinfo* res0 = NULL;
    int gai_rc = getaddrinfo(options->host, options->port, &hints, &res0);
    if (gai_rc != 0) {
        bx_diag(diag, "getaddrinfo('%s', '%s') failed: %s", options->host != NULL ? options->host : "*", options->port, gai_strerror(gai_rc));
        return -1;
    }

    int fd = -1;
    int saved_errno = EADDRINUSE;

    for (struct addrinfo* res = res0; res != NULL; res = res->ai_next) {
        fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (fd < 0) {
            saved_errno = errno;
            continue;
        }

        int one = 1;
        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

        if (bind(fd, res->ai_addr, res->ai_addrlen) != 0) {
            saved_errno = errno;
            close(fd);
            fd = -1;
            continue;
        }

        if (!options->udp_mode) {
            int backlog = options->keep_open ? 16 : 1;
            if (listen(fd, backlog) != 0) {
                saved_errno = errno;
                close(fd);
                fd = -1;
                continue;
            }
        }

        break;
    }

    freeaddrinfo(res0);

    if (fd < 0) {
        errno = saved_errno;
        bx_diag(diag, "listen setup failed: %s", strerror(errno));
        return -1;
    }

    bx_nc_report_local_listen(fd, options);

    return fd;
}

static int bx_nc_run_client(const struct bx_nc_options* options, struct bx_diag_ctx* diag) {
    int fd = bx_nc_open_client_socket(options, diag);
    if (fd < 0) {
        return 1;
    }

    bx_nc_report_connected(fd, options);

    int rc = 0;
    if (!options->zero_io) {
        rc = bx_nc_relay_connected_socket(fd, options, !options->detach_stdin, diag);
    }

    close(fd);
    return rc;
}

static int bx_nc_run_udp_listener(int fd, const struct bx_nc_options* options, struct bx_diag_ctx* diag) {
    unsigned char io_buffer[16384];
    struct sockaddr_storage peer;
    socklen_t peer_len = sizeof(peer);

    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    int poll_rc;
    do {
        poll_rc = poll(&pfd, 1, bx_nc_timeout_ms(options->timeout_seconds));
    } while (poll_rc < 0 && errno == EINTR);

    if (poll_rc < 0) {
        bx_diag(diag, "poll failed: %s", strerror(errno));
        return 1;
    }

    if (poll_rc == 0) {
        return 0;
    }

    ssize_t nread = recvfrom(fd, io_buffer, sizeof(io_buffer), 0, (struct sockaddr*)&peer, &peer_len);
    if (nread < 0) {
        bx_diag(diag, "recvfrom failed: %s", strerror(errno));
        return 1;
    }

    if (connect(fd, (struct sockaddr*)&peer, peer_len) != 0) {
        bx_diag(diag, "udp peer connect failed: %s", strerror(errno));
        return 1;
    }

    bx_nc_report_peer((struct sockaddr*)&peer, peer_len, options);

    if (nread > 0 && !bx_xwrite_all(STDOUT_FILENO, io_buffer, (size_t)nread)) {
        bx_diag(diag, "stdout write failed: %s", strerror(errno));
        return 1;
    }

    return bx_nc_relay_connected_socket(fd, options, !options->detach_stdin, diag);
}

static int bx_nc_run_tcp_listener(int fd, const struct bx_nc_options* options, struct bx_diag_ctx* diag) {
    int result = 0;

    while (true) {
        struct sockaddr_storage peer;
        socklen_t peer_len = sizeof(peer);

        int conn_fd;
        do {
            conn_fd = accept(fd, (struct sockaddr*)&peer, &peer_len);
        } while (conn_fd < 0 && errno == EINTR);

        if (conn_fd < 0) {
            bx_diag(diag, "accept failed: %s", strerror(errno));
            return 1;
        }

        bx_nc_report_peer((struct sockaddr*)&peer, peer_len, options);

        int conn_rc = bx_nc_relay_connected_socket(conn_fd, options, !options->detach_stdin, diag);
        close(conn_fd);

        if (conn_rc != 0) {
            result = conn_rc;
            break;
        }

        if (!options->keep_open) {
            break;
        }
    }

    return result;
}

static int bx_nc_run_listener(const struct bx_nc_options* options, struct bx_diag_ctx* diag) {
    int fd = bx_nc_open_listener_socket(options, diag);
    if (fd < 0) {
        return 1;
    }

    int rc;
    if (options->udp_mode) {
        rc = bx_nc_run_udp_listener(fd, options, diag);
    }
    else {
        rc = bx_nc_run_tcp_listener(fd, options, diag);
    }

    close(fd);
    return rc;
}

int bx_nc_main(int argc, char** argv) {
    struct bx_nc_options options;
    struct bx_diag_ctx diag = {
        .progname = "nc",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };

    if (!bx_nc_parse_options(argc, argv, &options, &diag)) {
        bx_nc_print_try_help(options.progname);
        return 2;
    }

    if (options.show_help) {
        bx_nc_print_help(stdout, options.progname);
        return 0;
    }

    if (options.show_version) {
        bx_nc_print_version(options.progname);
        return 0;
    }

    signal(SIGPIPE, SIG_IGN);

    if (options.listen_mode) {
        return bx_nc_run_listener(&options, &diag);
    }

    return bx_nc_run_client(&options, &diag);
}
