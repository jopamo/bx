#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <linux/errqueue.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/ip_icmp.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "applets.h"
#include "diag.h"
#include "libbx.h"

#ifndef IP_RECVERR
#define IP_RECVERR 11
#endif

struct bx_traceroute_options {
    const char* progname;
    bool show_help;
    bool show_version;
    bool numeric_only;
    unsigned int first_hop;
    unsigned int max_hops;
    unsigned int queries;
    unsigned int base_port;
    double wait_secs;
    const char* destination;
};

struct bx_traceroute_probe_result {
    bool replied;
    bool reached;
    struct in_addr responder;
    int icmp_type;
    int icmp_code;
    double rtt_ms;
};

static const char* bx_traceroute_progname(const char* argv0) {
    if (argv0 == NULL || argv0[0] == '\0') {
        return "traceroute";
    }

    const char* base = strrchr(argv0, '/');
    if (base != NULL && base[1] != '\0') {
        return base + 1;
    }

    return argv0;
}

static void bx_traceroute_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... HOST\n", progname);
    fprintf(stream, "Trace the IPv4 route to HOST with UDP probes.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -4, --ipv4            use IPv4 (default and only mode in this phase)\n");
    fprintf(stream, "  -f, --first-hop=NUM   start at hop NUM (default: 1)\n");
    fprintf(stream, "  -m, --max-hops=NUM    set max hops (default: 30)\n");
    fprintf(stream, "  -q, --queries=NUM     probes per hop (default: 3)\n");
    fprintf(stream, "  -w, --wait=SECS       wait per probe in seconds (default: 5)\n");
    fprintf(stream, "  -p, --port=PORT       destination base UDP port (default: 33434)\n");
    fprintf(stream, "  -n, --numeric         do not resolve host names\n");
    fprintf(stream, "  -h, --help            display this help and exit\n");
    fprintf(stream, "  -V, --version         output version information and exit\n");
}

static void bx_traceroute_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

static bool bx_traceroute_parse_uint(const char* text, unsigned int min_value, unsigned int max_value, unsigned int* out) {
    if (text == NULL || text[0] == '\0') {
        return false;
    }

    errno = 0;
    char* end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        return false;
    }
    if (value < min_value || value > max_value) {
        return false;
    }

    *out = (unsigned int)value;
    return true;
}

static bool bx_traceroute_parse_double(const char* text, double min_value, double max_value, double* out) {
    if (text == NULL || text[0] == '\0') {
        return false;
    }

    errno = 0;
    char* end = NULL;
    double value = strtod(text, &end);
    if (errno != 0 || end == text || *end != '\0') {
        return false;
    }
    if (!(value >= min_value && value <= max_value)) {
        return false;
    }

    *out = value;
    return true;
}

static bool bx_traceroute_parse_options(int argc, char** argv, struct bx_traceroute_options* options, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"ipv4", no_argument, NULL, '4'},           {"first-hop", required_argument, NULL, 'f'},
        {"max-hops", required_argument, NULL, 'm'}, {"queries", required_argument, NULL, 'q'},
        {"wait", required_argument, NULL, 'w'},     {"port", required_argument, NULL, 'p'},
        {"numeric", no_argument, NULL, 'n'},        {"help", no_argument, NULL, 'h'},
        {"version", no_argument, NULL, 'V'},        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_traceroute_progname((argc > 0) ? argv[0] : NULL);
    options->first_hop = 1;
    options->max_hops = 30;
    options->queries = 3;
    options->base_port = 33434;
    options->wait_secs = 5.0;

    diag->progname = options->progname;

    opterr = 0;
    optind = 1;

    while (true) {
        int c = getopt_long(argc, argv, ":+4f:m:q:w:p:nhV", long_options, NULL);
        if (c == -1) {
            break;
        }

        switch (c) {
            case '4':
                break;
            case 'f':
                if (!bx_traceroute_parse_uint(optarg, 1, 255, &options->first_hop)) {
                    bx_diag(diag, "invalid first hop: '%s'", optarg);
                    return false;
                }
                break;
            case 'm':
                if (!bx_traceroute_parse_uint(optarg, 1, 255, &options->max_hops)) {
                    bx_diag(diag, "invalid max hops: '%s'", optarg);
                    return false;
                }
                break;
            case 'q':
                if (!bx_traceroute_parse_uint(optarg, 1, 32, &options->queries)) {
                    bx_diag(diag, "invalid query count: '%s'", optarg);
                    return false;
                }
                break;
            case 'w':
                if (!bx_traceroute_parse_double(optarg, 0.001, 86400.0, &options->wait_secs)) {
                    bx_diag(diag, "invalid wait time: '%s'", optarg);
                    return false;
                }
                break;
            case 'p':
                if (!bx_traceroute_parse_uint(optarg, 1, 65535, &options->base_port)) {
                    bx_diag(diag, "invalid port: '%s'", optarg);
                    return false;
                }
                break;
            case 'n':
                options->numeric_only = true;
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
            default:
                return false;
        }
    }

    if (options->first_hop > options->max_hops) {
        bx_diag(diag, "first hop (%u) must be <= max hops (%u)", options->first_hop, options->max_hops);
        return false;
    }

    if (optind >= argc) {
        bx_diag(diag, "missing operand: HOST");
        return false;
    }

    options->destination = argv[optind++];

    if (optind != argc) {
        bx_diag(diag, "extra operand: '%s'", argv[optind]);
        return false;
    }

    return true;
}

static bool bx_traceroute_now_secs(double* out_secs, struct bx_diag_ctx* diag) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        bx_diag(diag, "failed to read monotonic clock: %s", strerror(errno));
        return false;
    }

    *out_secs = (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
    return true;
}

static void bx_traceroute_init_payload(unsigned char* payload, size_t payload_len) {
    for (size_t i = 0; i < payload_len; i++) {
        payload[i] = (unsigned char)(0x40u + (unsigned char)(i & 0x3fu));
    }
}

static bool bx_traceroute_resolve_destination(const char* host, struct sockaddr_in* destination, char* address_text, size_t address_text_len, struct bx_diag_ctx* diag) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    struct addrinfo* result = NULL;
    int gai_rc = getaddrinfo(host, NULL, &hints, &result);
    if (gai_rc != 0) {
        bx_diag(diag, "cannot resolve '%s': %s", host, gai_strerror(gai_rc));
        return false;
    }

    const struct sockaddr_in* resolved = (const struct sockaddr_in*)result->ai_addr;
    *destination = *resolved;

    if (inet_ntop(AF_INET, &resolved->sin_addr, address_text, address_text_len) == NULL) {
        bx_diag(diag, "cannot format destination address: %s", strerror(errno));
        freeaddrinfo(result);
        return false;
    }

    freeaddrinfo(result);
    return true;
}

static bool bx_traceroute_recv_icmp_reply(int fd, struct bx_traceroute_probe_result* result, bool* out_replied, struct bx_diag_ctx* diag) {
    *out_replied = false;

    for (;;) {
        char payload[2048];
        unsigned char control[1024];
        struct sockaddr_in sender;
        memset(&sender, 0, sizeof(sender));

        struct iovec iov;
        iov.iov_base = payload;
        iov.iov_len = sizeof(payload);

        struct msghdr msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_name = &sender;
        msg.msg_namelen = sizeof(sender);
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = control;
        msg.msg_controllen = sizeof(control);

        ssize_t recv_rc;
        do {
            recv_rc = recvmsg(fd, &msg, MSG_ERRQUEUE | MSG_DONTWAIT);
        } while (recv_rc < 0 && errno == EINTR);

        if (recv_rc < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return true;
            }
            bx_diag(diag, "recvmsg(MSG_ERRQUEUE) failed: %s", strerror(errno));
            return false;
        }

        struct sock_extended_err* ext_err = NULL;
        for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
            if (cmsg->cmsg_level == SOL_IP && cmsg->cmsg_type == IP_RECVERR) {
                ext_err = (struct sock_extended_err*)CMSG_DATA(cmsg);
                break;
            }
        }

        if (ext_err == NULL) {
            continue;
        }

        if (ext_err->ee_origin != SO_EE_ORIGIN_ICMP) {
            continue;
        }

        result->replied = true;
        result->icmp_type = ext_err->ee_type;
        result->icmp_code = ext_err->ee_code;

        struct sockaddr* offender = SO_EE_OFFENDER(ext_err);
        if (offender != NULL && offender->sa_family == AF_INET) {
            result->responder = ((struct sockaddr_in*)offender)->sin_addr;
        }
        else {
            result->responder = sender.sin_addr;
        }

        if (ext_err->ee_type == ICMP_DEST_UNREACH && ext_err->ee_code == ICMP_PORT_UNREACH) {
            result->reached = true;
        }

        *out_replied = true;
        return true;
    }
}

static bool bx_traceroute_send_probe_socket(const struct sockaddr_in* destination,
                                            unsigned int ttl,
                                            unsigned int port,
                                            const unsigned char* payload,
                                            size_t payload_len,
                                            int* out_fd,
                                            double* out_send_time,
                                            struct bx_diag_ctx* diag) {
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        bx_diag(diag, "cannot open probe socket: %s", strerror(errno));
        return false;
    }

    int ttl_value = (int)ttl;
    if (setsockopt(fd, SOL_IP, IP_TTL, &ttl_value, sizeof(ttl_value)) != 0) {
        bx_diag(diag, "cannot set TTL: %s", strerror(errno));
        close(fd);
        return false;
    }

    int recv_err = 1;
    if (setsockopt(fd, SOL_IP, IP_RECVERR, &recv_err, sizeof(recv_err)) != 0) {
        bx_diag(diag, "cannot enable IP_RECVERR: %s", strerror(errno));
        close(fd);
        return false;
    }

    struct sockaddr_in target = *destination;
    target.sin_port = htons((uint16_t)port);

    if (connect(fd, (const struct sockaddr*)&target, sizeof(target)) != 0) {
        bx_diag(diag, "cannot connect probe socket: %s", strerror(errno));
        close(fd);
        return false;
    }

    double start_time = bx_traceroute_now_secs();
    ssize_t send_rc = send(fd, payload, payload_len, 0);
    if (send_rc < 0) {
        bx_diag(diag, "cannot send probe: %s", strerror(errno));
        close(fd);
        return false;
    }

    *out_fd = fd;
    *out_send_time = start_time;

    return true;
}

static void bx_traceroute_format_endpoint(const struct in_addr* address, bool numeric_only, char* out, size_t out_len) {
    char address_text[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, address, address_text, sizeof(address_text)) == NULL) {
        snprintf(out, out_len, "?.?.?.?");
        return;
    }

    if (numeric_only) {
        snprintf(out, out_len, "%s", address_text);
        return;
    }

    struct sockaddr_in sockaddr;
    memset(&sockaddr, 0, sizeof(sockaddr));
    sockaddr.sin_family = AF_INET;
    sockaddr.sin_addr = *address;

    char host[NI_MAXHOST];
    int name_rc = getnameinfo((const struct sockaddr*)&sockaddr, sizeof(sockaddr), host, sizeof(host), NULL, 0, NI_NAMEREQD);
    if (name_rc == 0) {
        snprintf(out, out_len, "%s (%s)", host, address_text);
    }
    else {
        snprintf(out, out_len, "%s", address_text);
    }
}

static void bx_traceroute_print_hop_line(unsigned int ttl, const struct bx_traceroute_probe_result* results, unsigned int queries, bool numeric_only) {
    printf("%2u  ", ttl);

    char last_endpoint[NI_MAXHOST + INET_ADDRSTRLEN + 8];
    last_endpoint[0] = '\0';

    for (unsigned int i = 0; i < queries; i++) {
        const struct bx_traceroute_probe_result* result = &results[i];
        if (!result->replied) {
            printf("*  ");
            last_endpoint[0] = '\0';
            continue;
        }

        char endpoint[NI_MAXHOST + INET_ADDRSTRLEN + 8];
        bx_traceroute_format_endpoint(&result->responder, numeric_only, endpoint, sizeof(endpoint));

        if (last_endpoint[0] == '\0' || strcmp(last_endpoint, endpoint) != 0) {
            printf("%s  ", endpoint);
            snprintf(last_endpoint, sizeof(last_endpoint), "%s", endpoint);
        }

        printf("%.3f ms", result->rtt_ms);

        if (result->icmp_type == ICMP_DEST_UNREACH && result->icmp_code != ICMP_PORT_UNREACH) {
            printf(" !%d", result->icmp_code);
        }

        printf("  ");
    }

    printf("\n");
}

int bx_traceroute_main(int argc, char** argv) {
    struct bx_traceroute_options options;
    struct bx_diag_ctx diag = {
        .progname = "traceroute",
        .exit_status = 0,
    };

    if (!bx_traceroute_parse_options(argc, argv, &options, &diag)) {
        return 1;
    }

    if (options.show_help) {
        bx_traceroute_print_help(stdout, options.progname);
        return 0;
    }

    if (options.show_version) {
        bx_traceroute_print_version(options.progname);
        return 0;
    }

    struct sockaddr_in destination;
    char destination_text[INET_ADDRSTRLEN];
    if (!bx_traceroute_resolve_destination(options.destination, &destination, destination_text, sizeof(destination_text), &diag)) {
        return 1;
    }

    const size_t payload_len = 40;
    unsigned char payload[payload_len];
    bx_traceroute_init_payload(payload, payload_len);

    struct bx_traceroute_probe_result* results = xmalloc((size_t)options.queries * sizeof(*results));
    int* probe_fds = xmalloc((size_t)options.queries * sizeof(*probe_fds));
    double* probe_start_times = xmalloc((size_t)options.queries * sizeof(*probe_start_times));

    printf("traceroute to %s (%s), %u hops max, %zu byte packets\n", options.destination, destination_text, options.max_hops, payload_len);

    bool reached = false;
    for (unsigned int ttl = options.first_hop; ttl <= options.max_hops; ttl++) {
        memset(results, 0, (size_t)options.queries * sizeof(*results));
        for (unsigned int query = 0; query < options.queries; query++) {
            probe_fds[query] = -1;
            probe_start_times[query] = 0.0;
        }

        unsigned int hop_offset = (ttl - options.first_hop) * options.queries;
        unsigned int port_span = 65536u - options.base_port;

        for (unsigned int query = 0; query < options.queries; query++) {
            unsigned int port = options.base_port;
            if (port_span > 0u) {
                port += (hop_offset + query) % port_span;
            }

            if (!bx_traceroute_send_probe_socket(&destination, ttl, port, payload, payload_len, &probe_fds[query], &probe_start_times[query], &diag)) {
                for (unsigned int i = 0; i <= query; i++) {
                    if (probe_fds[i] >= 0) {
                        close(probe_fds[i]);
                        probe_fds[i] = -1;
                    }
                }
                free(probe_start_times);
                free(probe_fds);
                free(results);
                return 1;
            }
        }

        double hop_deadline = bx_traceroute_now_secs() + options.wait_secs;
        for (unsigned int query = 0; query < options.queries; query++) {
            double remaining_secs = hop_deadline - bx_traceroute_now_secs();
            int timeout_ms = 0;
            if (remaining_secs > 0.0) {
                timeout_ms = (int)(remaining_secs * 1000.0);
                if (timeout_ms < 1) {
                    timeout_ms = 1;
                }
            }

            if (!bx_traceroute_wait_for_reply(probe_fds[query], timeout_ms, &results[query], &diag)) {
                for (unsigned int i = query; i < options.queries; i++) {
                    if (probe_fds[i] >= 0) {
                        close(probe_fds[i]);
                        probe_fds[i] = -1;
                    }
                }
                free(probe_start_times);
                free(probe_fds);
                free(results);
                return 1;
            }

            double end_time = bx_traceroute_now_secs();
            close(probe_fds[query]);
            probe_fds[query] = -1;

            if (results[query].replied) {
                results[query].rtt_ms = (end_time - probe_start_times[query]) * 1000.0;
            }
            if (results[query].reached) {
                reached = true;
            }
        }

        bx_traceroute_print_hop_line(ttl, results, options.queries, options.numeric_only);

        if (reached) {
            break;
        }
    }

    free(probe_start_times);
    free(probe_fds);
    free(results);
    return 0;
}
