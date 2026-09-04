#define _DEFAULT_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/ip.h>
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
#include "bx/diag.h"
#include "lib/cli_common.h"
#include "lib/args_common.h"
#include "lib/fd_ops.h"
#include "lib/internet_checksum.h"
#include "lib/poll_deadline.h"
#include "lib/sockaddr_format.h"
#include "lib/time_parse.h"

#define BX_PING_DEFAULT_COUNT 1u
#define BX_PING_DEFAULT_TIMEOUT_MS 1000u
#define BX_PING_DEFAULT_PAYLOAD_SIZE 56u
#define BX_PING_MAX_COUNT 65535u
#define BX_PING_MAX_TIMEOUT_SECS 3600u
#define BX_PING_MAX_PAYLOAD_SIZE 1400u

struct bx_ping_options {
    const char* progname;
    bool show_help;
    bool show_version;
    unsigned int count;
    unsigned int timeout_ms;
    unsigned int payload_size;
    const char* destination;
};

struct bx_ping_socket {
    int fd;
    bool raw;
};

struct bx_ping_reply {
    bool received;
    double rtt_ms;
};

static void bx_ping_print_help(FILE* stream, const char* progname) {
    int default_timeout_seconds = 0;

    (void)bx_time_milliseconds_to_seconds_int_floor(BX_PING_DEFAULT_TIMEOUT_MS, &default_timeout_seconds);

    fprintf(stream, "Usage: %s [OPTION]... HOST\n", progname);
    fprintf(stream, "Send ICMP ECHO_REQUEST packets to an IPv4 host.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -4, --ipv4            use IPv4 (default and only mode in this phase)\n");
    fprintf(stream, "  -c, --count=COUNT     stop after COUNT probes (default: %u)\n", BX_PING_DEFAULT_COUNT);
    fprintf(stream, "  -W, --timeout=SECS    wait up to SECS for each reply (default: %d)\n", default_timeout_seconds);
    fprintf(stream, "  -s, --size=BYTES      ICMP payload size (default: %u)\n", BX_PING_DEFAULT_PAYLOAD_SIZE);
    fprintf(stream, "  -h, --help            display this help and exit\n");
    fprintf(stream, "  -V, --version         output version information and exit\n");
}

static bool bx_ping_parse_uint(const char* text, unsigned int min_value, unsigned int max_value, unsigned int* out_value) {
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

    *out_value = (unsigned int)value;
    return true;
}

static bool bx_ping_parse_options(int argc, char** argv, struct bx_ping_options* options, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"ipv4", no_argument, NULL, '4'},
        {"count", required_argument, NULL, 'c'},
        {"timeout", required_argument, NULL, 'W'},
        {"size", required_argument, NULL, 's'},
        {"help", no_argument, NULL, 'h'},
        {"version", no_argument, NULL, 'V'},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "ping");
    options->count = BX_PING_DEFAULT_COUNT;
    options->timeout_ms = BX_PING_DEFAULT_TIMEOUT_MS;
    options->payload_size = BX_PING_DEFAULT_PAYLOAD_SIZE;
    diag->progname = options->progname;

    bx_args_getopt_reset();

    while (true) {
        int c = bx_args_getopt_long(argc, argv, "+4c:W:s:hV", long_options, NULL);
        if (c == -1) {
            break;
        }

        unsigned int value = 0;
        switch (c) {
            case '4':
                break;
            case 'c':
                if (!bx_ping_parse_uint(optarg, 1u, BX_PING_MAX_COUNT, &value)) {
                    bx_diag(diag, "invalid count '%s'", optarg != NULL ? optarg : "");
                    return false;
                }
                options->count = value;
                break;
            case 'W':
                if (!bx_ping_parse_uint(optarg, 1u, BX_PING_MAX_TIMEOUT_SECS, &value)) {
                    bx_diag(diag, "invalid timeout '%s'", optarg != NULL ? optarg : "");
                    return false;
                }
                uintmax_t timeout_ms = 0;
                if (!bx_time_seconds_to_milliseconds_uint(value, &timeout_ms) || timeout_ms > (uintmax_t)UINT_MAX) {
                    bx_diag(diag, "invalid timeout '%s'", optarg != NULL ? optarg : "");
                    return false;
                }
                options->timeout_ms = (unsigned int)timeout_ms;
                break;
            case 's':
                if (!bx_ping_parse_uint(optarg, 0u, BX_PING_MAX_PAYLOAD_SIZE, &value)) {
                    bx_diag(diag, "invalid payload size '%s'", optarg != NULL ? optarg : "");
                    return false;
                }
                options->payload_size = value;
                break;
            case 'h':
                options->show_help = true;
                return true;
            case 'V':
                options->show_version = true;
                return true;
            case '?':
                bx_cli_diag_unrecognized_option(diag, optopt, optind, argc, argv);
                return false;
            case ':':
                bx_cli_diag_option_requires_arg(diag, optopt, optind, argc, argv);
                return false;
            default:
                return false;
        }
    }

    if (optind >= argc) {
        bx_diag(diag, "missing HOST operand");
        return false;
    }

    options->destination = argv[optind++];

    if (optind < argc) {
        bx_diag(diag, "extra operand '%s'", argv[optind]);
        return false;
    }

    return true;
}

static bool bx_ping_resolve_destination(const char* host, struct sockaddr_in* destination, char* address_text, size_t address_text_len, struct bx_diag_ctx* diag) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;

    struct addrinfo* result = NULL;
    int gai_rc = getaddrinfo(host, NULL, &hints, &result);
    if (gai_rc != 0) {
        bx_diag(diag, "cannot resolve '%s': %s", host, gai_strerror(gai_rc));
        return false;
    }

    bool ok = false;
    for (const struct addrinfo* it = result; it != NULL; it = it->ai_next) {
        if (it->ai_family != AF_INET || it->ai_addr == NULL || it->ai_addrlen < sizeof(struct sockaddr_in)) {
            continue;
        }

        memcpy(destination, it->ai_addr, sizeof(*destination));
        if (bx_sockaddr_format_numeric(
                (const struct sockaddr*)destination,
                sizeof(*destination),
                address_text,
                address_text_len,
                NULL,
                0) != 0) {
            break;
        }

        ok = true;
        break;
    }

    freeaddrinfo(result);

    if (!ok) {
        bx_diag(diag, "cannot resolve '%s' to an IPv4 address", host);
    }
    return ok;
}

static bool bx_ping_open_socket(struct bx_ping_socket* socket_state, struct bx_diag_ctx* diag) {
    socket_state->fd = -1;
    socket_state->raw = false;

    int fd = bx_fd_socket_cloexec(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
    if (fd >= 0) {
        socket_state->fd = fd;
        return true;
    }

    fd = bx_fd_socket_cloexec(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (fd >= 0) {
        socket_state->fd = fd;
        socket_state->raw = true;
        return true;
    }

    bx_diag(diag, "failed to open ICMP socket: %s", strerror(errno));
    return false;
}

static double bx_ping_now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0.0;
    }

    double milliseconds = 0.0;
    if (!bx_time_timespec_to_milliseconds_double(&ts, &milliseconds)) {
        return 0.0;
    }
    return milliseconds;
}

static bool bx_ping_send_probe(const struct bx_ping_socket* socket_state,
                               const struct sockaddr_in* destination,
                               uint16_t identifier,
                               uint16_t sequence,
                               unsigned int payload_size,
                               struct bx_diag_ctx* diag) {
    unsigned char packet[sizeof(struct icmphdr) + BX_PING_MAX_PAYLOAD_SIZE];
    size_t packet_len = sizeof(struct icmphdr) + (size_t)payload_size;

    memset(packet, 0, packet_len);

    struct icmphdr* header = (struct icmphdr*)packet;
    header->type = ICMP_ECHO;
    header->code = 0;
    header->un.echo.id = htons(identifier);
    header->un.echo.sequence = htons(sequence);

    for (unsigned int i = 0; i < payload_size; i++) {
        packet[sizeof(struct icmphdr) + i] = (unsigned char)(0x20u + (unsigned char)(i & 0x5fu));
    }

    header->checksum = bx_internet_checksum_host(packet, packet_len);

    ssize_t sent = sendto(socket_state->fd, packet, packet_len, 0, (const struct sockaddr*)destination, sizeof(*destination));
    if (sent < 0) {
        bx_diag(diag, "sendto failed: %s", strerror(errno));
        return false;
    }
    if ((size_t)sent != packet_len) {
        bx_diag(diag, "short send while transmitting probe");
        return false;
    }

    return true;
}

static bool bx_ping_extract_icmp(const unsigned char* packet, size_t packet_len, bool raw_socket, const struct icmphdr** header_out) {
    if (raw_socket) {
        if (packet_len < sizeof(struct ip) + sizeof(struct icmphdr)) {
            return false;
        }

        const struct ip* ip_header = (const struct ip*)packet;
        size_t ip_header_len = (size_t)ip_header->ip_hl * 4u;
        if (ip_header_len < sizeof(struct ip) || packet_len < ip_header_len + sizeof(struct icmphdr)) {
            return false;
        }

        *header_out = (const struct icmphdr*)(packet + ip_header_len);
        return true;
    }

    if (packet_len < sizeof(struct icmphdr)) {
        return false;
    }

    *header_out = (const struct icmphdr*)packet;
    return true;
}

static bool bx_ping_wait_for_reply(const struct bx_ping_socket* socket_state,
                                   const struct in_addr* expected_addr,
                                   uint16_t expected_identifier,
                                   uint16_t expected_sequence,
                                   unsigned int timeout_ms,
                                   struct bx_ping_reply* reply,
                                   struct bx_diag_ctx* diag) {
    reply->received = false;
    reply->rtt_ms = 0.0;

    struct bx_poll_deadline deadline;
    int poll_timeout =
        timeout_ms > (unsigned int)INT_MAX ? INT_MAX : (int)timeout_ms;
    bx_poll_deadline_init(&deadline, poll_timeout);
    double started_ms = bx_ping_now_ms();

    while (true) {
        struct pollfd pfd;
        memset(&pfd, 0, sizeof(pfd));
        pfd.fd = socket_state->fd;
        pfd.events = POLLIN;

        int poll_rc = bx_poll_deadline_wait(&deadline, &pfd, 1);
        if (poll_rc == 0) {
            return true;
        }
        if (poll_rc < 0) {
            bx_diag(diag, "poll failed: %s", strerror(errno));
            return false;
        }
        if ((pfd.revents & POLLIN) == 0) {
            continue;
        }

        unsigned char packet[2048];
        struct sockaddr_in source_addr;
        memset(&source_addr, 0, sizeof(source_addr));
        socklen_t source_len = sizeof(source_addr);

        ssize_t received = recvfrom(socket_state->fd, packet, sizeof(packet), 0, (struct sockaddr*)&source_addr, &source_len);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return true;
            }
            bx_diag(diag, "recvfrom failed: %s", strerror(errno));
            return false;
        }

        const struct icmphdr* header = NULL;
        if (!bx_ping_extract_icmp(packet, (size_t)received, socket_state->raw, &header)) {
            continue;
        }

        if (header->type != ICMP_ECHOREPLY || header->code != 0) {
            continue;
        }
        if (source_len < sizeof(source_addr) || source_addr.sin_family != AF_INET) {
            continue;
        }
        if (source_addr.sin_addr.s_addr != expected_addr->s_addr) {
            continue;
        }

        uint16_t sequence = ntohs(header->un.echo.sequence);
        if (sequence != expected_sequence) {
            continue;
        }

        if (socket_state->raw) {
            uint16_t identifier = ntohs(header->un.echo.id);
            if (identifier != expected_identifier) {
                continue;
            }
        }

        reply->received = true;
        reply->rtt_ms = bx_ping_now_ms() - started_ms;
        if (reply->rtt_ms < 0.0) {
            reply->rtt_ms = 0.0;
        }
        return true;
    }
}

static void bx_ping_sleep_between_probes(void) {
    struct timespec interval;
    if (!bx_time_seconds_to_timespec(1.0, &interval)) {
        return;
    }

    while (nanosleep(&interval, &interval) != 0 && errno == EINTR) {
    }
}

int bx_ping_main(int argc, char** argv) {
    struct bx_diag_ctx diag = {
        .progname = "ping",
        .exit_status = 0,
    };

    struct bx_ping_options options;
    if (!bx_ping_parse_options(argc, argv, &options, &diag)) {
        bx_cli_print_try_help(diag.progname);
        return diag.exit_status ? diag.exit_status : 1;
    }

    if (options.show_help) {
        bx_ping_print_help(stdout, options.progname);
        return 0;
    }
    if (options.show_version) {
        bx_cli_print_version(options.progname);
        return 0;
    }

    struct sockaddr_in destination;
    memset(&destination, 0, sizeof(destination));

    char destination_addr_text[INET_ADDRSTRLEN];
    destination_addr_text[0] = '\0';

    if (!bx_ping_resolve_destination(options.destination, &destination, destination_addr_text, sizeof(destination_addr_text), &diag)) {
        return diag.exit_status ? diag.exit_status : 1;
    }

    struct bx_ping_socket socket_state;
    if (!bx_ping_open_socket(&socket_state, &diag)) {
        return diag.exit_status ? diag.exit_status : 1;
    }

    printf("PING %s (%s): %u data bytes\n", options.destination, destination_addr_text, options.payload_size);

    uint16_t identifier = (uint16_t)getpid();
    unsigned int transmitted = 0;
    unsigned int received = 0;

    for (unsigned int i = 0; i < options.count; i++) {
        uint16_t sequence = (uint16_t)(i + 1u);

        if (!bx_ping_send_probe(&socket_state, &destination, identifier, sequence, options.payload_size, &diag)) {
            close(socket_state.fd);
            return diag.exit_status ? diag.exit_status : 1;
        }

        transmitted++;

        struct bx_ping_reply reply;
        if (!bx_ping_wait_for_reply(&socket_state, &destination.sin_addr, identifier, sequence, options.timeout_ms, &reply, &diag)) {
            close(socket_state.fd);
            return diag.exit_status ? diag.exit_status : 1;
        }

        if (reply.received) {
            received++;
            printf("%u bytes from %s: icmp_seq=%u time=%.3f ms\n", options.payload_size + (unsigned int)sizeof(struct icmphdr), destination_addr_text, (unsigned int)sequence, reply.rtt_ms);
        }
        else {
            printf("Request timeout for icmp_seq %u\n", (unsigned int)sequence);
        }

        if (i + 1u < options.count) {
            bx_ping_sleep_between_probes();
        }
    }

    close(socket_state.fd);

    unsigned int lost = transmitted - received;
    unsigned int packet_loss_percent = (transmitted == 0) ? 0u : (lost * 100u) / transmitted;

    printf("\n--- %s ping statistics ---\n", options.destination);
    printf("%u packets transmitted, %u packets received, %u%% packet loss\n", transmitted, received, packet_loss_percent);

    return (received == transmitted) ? 0 : 1;
}
