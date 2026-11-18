#define _DEFAULT_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"
#include "lib/cli_common.h"
#include "lib/child_runner.h"
#include "lib/args_common.h"
#include "lib/fd_ops.h"
#include "lib/poll_deadline.h"
#include "lib/random_bytes.h"
#include "lib/time_parse.h"

#define BX_DHCP_BOOTP_FIXED_LEN 236u
#define BX_DHCP_PACKET_MIN_LEN 240u
#define BX_DHCP_PACKET_MAX_LEN 1500u

#define BX_DHCP_DEFAULT_TIMEOUT_MS 5000u
#define BX_DHCP_DEFAULT_RETRIES 3u
#define BX_DHCP_DEFAULT_SERVER_PORT 67u
#define BX_DHCP_DEFAULT_CLIENT_PORT 68u

#define BX_DHCP_OPTION_SUBNET_MASK 1u
#define BX_DHCP_OPTION_ROUTER 3u
#define BX_DHCP_OPTION_DNS 6u
#define BX_DHCP_OPTION_REQUESTED_IP 50u
#define BX_DHCP_OPTION_LEASE_TIME 51u
#define BX_DHCP_OPTION_MESSAGE_TYPE 53u
#define BX_DHCP_OPTION_SERVER_ID 54u
#define BX_DHCP_OPTION_PARAMETER_REQUEST_LIST 55u
#define BX_DHCP_OPTION_T1 58u
#define BX_DHCP_OPTION_T2 59u
#define BX_DHCP_OPTION_CLIENT_ID 61u
#define BX_DHCP_OPTION_PAD 0u
#define BX_DHCP_OPTION_END 255u

#define BX_DHCP_MESSAGE_DISCOVER 1u
#define BX_DHCP_MESSAGE_OFFER 2u
#define BX_DHCP_MESSAGE_REQUEST 3u
#define BX_DHCP_MESSAGE_ACK 5u
#define BX_DHCP_MESSAGE_NAK 6u

struct bx_dhcp_options {
    const char* progname;
    bool show_help;
    bool show_version;
    const char* ifname;
    const char* script;
    int stop_fd;
    sigset_t caller_mask;
    unsigned int timeout_ms;
    unsigned int retries;
    uint16_t server_port;
    uint16_t client_port;
    struct in_addr server_addr;
};

/* Repeated list options share the packet's total option budget. */
#define BX_DHCP_MAX_ADDRS ((BX_DHCP_PACKET_MAX_LEN - BX_DHCP_PACKET_MIN_LEN) / sizeof(struct in_addr))
struct bx_dhcp_addresses {
    size_t count;
    struct in_addr values[BX_DHCP_MAX_ADDRS];
};

struct bx_dhcp_message {
    uint8_t message_type;
    uint32_t xid;
    struct in_addr yiaddr;
    bool have_server_id;
    struct in_addr server_id;
    bool have_subnet_mask;
    struct in_addr subnet_mask;
    struct bx_dhcp_addresses router, dns;
    bool have_lease_time;
    uint32_t lease_time;
    uint32_t t1, t2;
};

enum bx_dhcp_wait_result {
    BX_DHCP_WAIT_MESSAGE = 0,
    BX_DHCP_WAIT_TIMEOUT,
    BX_DHCP_WAIT_ERROR,
    BX_DHCP_WAIT_STOP,
};

static const unsigned char bx_dhcp_magic_cookie[4] = {99u, 130u, 83u, 99u};
static const unsigned char bx_dhcp_parameter_request_list[] = {
    BX_DHCP_OPTION_SUBNET_MASK, BX_DHCP_OPTION_ROUTER, BX_DHCP_OPTION_DNS, BX_DHCP_OPTION_LEASE_TIME, BX_DHCP_OPTION_SERVER_ID, BX_DHCP_OPTION_T1, BX_DHCP_OPTION_T2,
};

static void bx_dhcp_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... IFACE\n", progname);
    fprintf(stream, "Acquire an IPv4 lease. By default, print it and exit.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -i, --interface=IFACE        network interface name\n");
    fprintf(stream, "  -t, --timeout=SECS           response timeout per stage (default: 5)\n");
    fprintf(stream, "  -s, --script=PATH           maintain the lease in foreground; call PATH on lease changes\n");
    fprintf(stream, "  -r, --retries=COUNT          discovery attempts (default: 3)\n");
    fprintf(stream, "      --server-address=ADDR    server destination address (default: 255.255.255.255)\n");
    fprintf(stream, "      --server-port=PORT       server UDP port (default: 67)\n");
    fprintf(stream, "      --client-port=PORT       client UDP port (default: 68; use 0 for an ephemeral port)\n");
    fprintf(stream, "  -h, --help                   display this help and exit\n");
    fprintf(stream, "  -V, --version                output version information and exit\n");
    fprintf(stream, "\n");
    fprintf(stream, "On success, prints lease details as KEY=VALUE lines.\n");
}

static bool bx_dhcp_parse_uint(const char* text, unsigned int min_value, unsigned int max_value, unsigned int* out_value) {
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

static bool bx_dhcp_parse_ipv4(const char* text, struct in_addr* out_addr) {
    if (text == NULL || text[0] == '\0') {
        return false;
    }

    struct in_addr parsed;
    int rc = inet_pton(AF_INET, text, &parsed);
    if (rc != 1) {
        return false;
    }

    *out_addr = parsed;
    return true;
}

static bool bx_dhcp_parse_options(int argc, char** argv, struct bx_dhcp_options* options, struct bx_diag_ctx* diag) {
    enum {
        BX_DHCP_OPT_SERVER_PORT = 256,
        BX_DHCP_OPT_CLIENT_PORT,
        BX_DHCP_OPT_SERVER_ADDRESS,
    };

    static const struct option long_options[] = {
        {"script", required_argument, NULL, 's'},
        {"interface", required_argument, NULL, 'i'},
        {"timeout", required_argument, NULL, 't'},
        {"retries", required_argument, NULL, 'r'},
        {"server-port", required_argument, NULL, BX_DHCP_OPT_SERVER_PORT},
        {"client-port", required_argument, NULL, BX_DHCP_OPT_CLIENT_PORT},
        {"server-address", required_argument, NULL, BX_DHCP_OPT_SERVER_ADDRESS},
        {"help", no_argument, NULL, 'h'},
        {"version", no_argument, NULL, 'V'},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->stop_fd = -1;
    options->progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "dhcp");
    options->timeout_ms = BX_DHCP_DEFAULT_TIMEOUT_MS;
    options->retries = BX_DHCP_DEFAULT_RETRIES;
    options->server_port = BX_DHCP_DEFAULT_SERVER_PORT;
    options->client_port = BX_DHCP_DEFAULT_CLIENT_PORT;
    options->server_addr.s_addr = htonl(INADDR_BROADCAST);
    diag->progname = options->progname;

    bx_args_getopt_reset();

    while (true) {
        int c = bx_args_getopt_long(argc, argv, "+:i:t:r:s:hV", long_options, NULL);
        if (c == -1) {
            break;
        }

        unsigned int value = 0;
        switch (c) {
            case 's':
                options->script = optarg;
                if (optarg[0] != '/') {
                    bx_diag(diag, "lease script must be an absolute path");
                    return false;
                }
                break;
            case 'i':
                options->ifname = optarg;
                break;
            case 't':
                if (!bx_dhcp_parse_uint(optarg, 1u, 3600u, &value)) {
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
            case 'r':
                if (!bx_dhcp_parse_uint(optarg, 1u, 100u, &value)) {
                    bx_diag(diag, "invalid retry count '%s'", optarg != NULL ? optarg : "");
                    return false;
                }
                options->retries = value;
                break;
            case BX_DHCP_OPT_SERVER_PORT:
                if (!bx_dhcp_parse_uint(optarg, 1u, 65535u, &value)) {
                    bx_diag(diag, "invalid server port '%s'", optarg != NULL ? optarg : "");
                    return false;
                }
                options->server_port = (uint16_t)value;
                break;
            case BX_DHCP_OPT_CLIENT_PORT:
                if (!bx_dhcp_parse_uint(optarg, 0u, 65535u, &value)) {
                    bx_diag(diag, "invalid client port '%s'", optarg != NULL ? optarg : "");
                    return false;
                }
                options->client_port = (uint16_t)value;
                break;
            case BX_DHCP_OPT_SERVER_ADDRESS:
                if (!bx_dhcp_parse_ipv4(optarg, &options->server_addr)) {
                    bx_diag(diag, "invalid server address '%s'", optarg != NULL ? optarg : "");
                    return false;
                }
                break;
            case 'h':
                options->show_help = true;
                return true;
            case 'V':
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

    if (options->ifname == NULL && optind < argc) {
        options->ifname = argv[optind++];
    }

    if (optind < argc) {
        bx_diag(diag, "extra operand '%s'", argv[optind]);
        return false;
    }

    if (options->ifname == NULL || options->ifname[0] == '\0') {
        bx_diag(diag, "missing interface operand");
        return false;
    }

    if (strlen(options->ifname) >= IFNAMSIZ) {
        bx_diag(diag, "interface name is too long: '%s'", options->ifname);
        return false;
    }

    return true;
}

static bool bx_dhcp_get_hwaddr(const char* ifname, unsigned char hwaddr[6], struct bx_diag_ctx* diag) {
    int fd = bx_fd_socket_cloexec(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        bx_diag(diag, "failed to open socket for interface query: %s", strerror(errno));
        return false;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    ifr.ifr_name[IFNAMSIZ - 1] = '\0';

    if (ioctl(fd, SIOCGIFHWADDR, &ifr) != 0) {
        bx_diag(diag, "failed to query hardware address for '%s': %s", ifname, strerror(errno));
        close(fd);
        return false;
    }

    memcpy(hwaddr, ifr.ifr_hwaddr.sa_data, 6);
    close(fd);
    return true;
}

static int bx_dhcp_open_socket(const struct bx_dhcp_options* options, struct bx_diag_ctx* diag) {
    int fd = bx_fd_socket_cloexec(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        bx_diag(diag, "failed to open DHCP socket: %s", strerror(errno));
        return -1;
    }

    int enabled = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) != 0) {
        bx_diag(diag, "failed to set SO_REUSEADDR: %s", strerror(errno));
        close(fd);
        return -1;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &enabled, sizeof(enabled)) != 0) {
        bx_diag(diag, "failed to set SO_BROADCAST: %s", strerror(errno));
        close(fd);
        return -1;
    }

    if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, options->ifname, (socklen_t)(strlen(options->ifname) + 1u)) != 0) {
        bx_diag(diag, "failed to bind DHCP socket to '%s': %s", options->ifname, strerror(errno));
        close(fd);
        return -1;
    }

    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(options->client_port);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, (const struct sockaddr*)&bind_addr, sizeof(bind_addr)) != 0) {
        bx_diag(diag, "failed to bind DHCP client socket to port %u: %s", (unsigned int)options->client_port, strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

static uint32_t bx_dhcp_generate_xid(void) {
    uint32_t xid = 0;
    if (bx_random_bytes(&xid, sizeof(xid)) && xid != 0u) {
        return xid;
    }

    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return (uint32_t)getpid() ^ 0x4b584448u;
    }

    xid = (uint32_t)ts.tv_nsec ^ (uint32_t)ts.tv_sec ^ ((uint32_t)getpid() << 16);
    if (xid == 0u) {
        xid = 0x4b584448u;
    }
    return xid;
}

static bool bx_dhcp_append_option_bytes(unsigned char* packet, size_t capacity, size_t* packet_len, uint8_t code, const unsigned char* data, size_t data_len) {
    if (data_len > 255u) {
        return false;
    }
    if (*packet_len + 2u + data_len + 1u > capacity) {
        return false;
    }

    packet[(*packet_len)++] = code;
    packet[(*packet_len)++] = (uint8_t)data_len;
    if (data_len > 0u) {
        memcpy(packet + *packet_len, data, data_len);
        *packet_len += data_len;
    }
    return true;
}

static bool bx_dhcp_append_option_u8(unsigned char* packet, size_t capacity, size_t* packet_len, uint8_t code, uint8_t value) {
    return bx_dhcp_append_option_bytes(packet, capacity, packet_len, code, &value, 1u);
}

static bool bx_dhcp_append_option_addr(unsigned char* packet, size_t capacity, size_t* packet_len, uint8_t code, const struct in_addr* addr) {
    return bx_dhcp_append_option_bytes(packet, capacity, packet_len, code, (const unsigned char*)&addr->s_addr, sizeof(addr->s_addr));
}

static bool bx_dhcp_append_option_end(unsigned char* packet, size_t capacity, size_t* packet_len) {
    if (*packet_len + 1u > capacity) {
        return false;
    }

    packet[(*packet_len)++] = BX_DHCP_OPTION_END;
    return true;
}

static size_t bx_dhcp_build_bootp_base(unsigned char* packet, size_t capacity, uint32_t xid, const unsigned char hwaddr[6]) {
    if (capacity < BX_DHCP_PACKET_MIN_LEN) {
        return 0u;
    }

    memset(packet, 0, BX_DHCP_PACKET_MIN_LEN);
    packet[0] = 1u;
    packet[1] = 1u;
    packet[2] = 6u;
    packet[3] = 0u;

    uint32_t xid_net = htonl(xid);
    memcpy(packet + 4u, &xid_net, sizeof(xid_net));

    uint16_t flags = htons(0x8000u);
    memcpy(packet + 10u, &flags, sizeof(flags));

    memcpy(packet + 28u, hwaddr, 6u);
    memcpy(packet + BX_DHCP_BOOTP_FIXED_LEN, bx_dhcp_magic_cookie, sizeof(bx_dhcp_magic_cookie));
    return BX_DHCP_PACKET_MIN_LEN;
}

static size_t bx_dhcp_build_discover(unsigned char* packet, size_t capacity, uint32_t xid, const unsigned char hwaddr[6]) {
    size_t packet_len = bx_dhcp_build_bootp_base(packet, capacity, xid, hwaddr);
    if (packet_len == 0u) {
        return 0u;
    }

    if (!bx_dhcp_append_option_u8(packet, capacity, &packet_len, BX_DHCP_OPTION_MESSAGE_TYPE, BX_DHCP_MESSAGE_DISCOVER)) {
        return 0u;
    }

    unsigned char client_id[7];
    client_id[0] = 1u;
    memcpy(client_id + 1u, hwaddr, 6u);
    if (!bx_dhcp_append_option_bytes(packet, capacity, &packet_len, BX_DHCP_OPTION_CLIENT_ID, client_id, sizeof(client_id))) {
        return 0u;
    }

    if (!bx_dhcp_append_option_bytes(packet, capacity, &packet_len, BX_DHCP_OPTION_PARAMETER_REQUEST_LIST, bx_dhcp_parameter_request_list, sizeof(bx_dhcp_parameter_request_list))) {
        return 0u;
    }

    if (!bx_dhcp_append_option_end(packet, capacity, &packet_len)) {
        return 0u;
    }

    return packet_len;
}

static size_t bx_dhcp_build_request(unsigned char* packet, size_t capacity, uint32_t xid, const unsigned char hwaddr[6], const struct in_addr* requested_ip, const struct in_addr* server_id) {
    size_t packet_len = bx_dhcp_build_bootp_base(packet, capacity, xid, hwaddr);
    if (packet_len == 0u) {
        return 0u;
    }

    if (!bx_dhcp_append_option_u8(packet, capacity, &packet_len, BX_DHCP_OPTION_MESSAGE_TYPE, BX_DHCP_MESSAGE_REQUEST)) {
        return 0u;
    }

    if (server_id != NULL) {
        if (!bx_dhcp_append_option_addr(packet, capacity, &packet_len, BX_DHCP_OPTION_REQUESTED_IP, requested_ip) ||
            !bx_dhcp_append_option_addr(packet, capacity, &packet_len, BX_DHCP_OPTION_SERVER_ID, server_id))
            return 0u;
    }
    else {
        memset(packet + 10, 0, 2);
        memcpy(packet + 12, &requested_ip->s_addr, 4);
    }

    unsigned char client_id[7];
    client_id[0] = 1u;
    memcpy(client_id + 1u, hwaddr, 6u);
    if (!bx_dhcp_append_option_bytes(packet, capacity, &packet_len, BX_DHCP_OPTION_CLIENT_ID, client_id, sizeof(client_id))) {
        return 0u;
    }

    if (!bx_dhcp_append_option_bytes(packet, capacity, &packet_len, BX_DHCP_OPTION_PARAMETER_REQUEST_LIST, bx_dhcp_parameter_request_list, sizeof(bx_dhcp_parameter_request_list))) {
        return 0u;
    }

    if (!bx_dhcp_append_option_end(packet, capacity, &packet_len)) {
        return 0u;
    }

    return packet_len;
}

static bool bx_dhcp_parse_packet(const unsigned char* packet, size_t packet_len, struct bx_dhcp_message* message_out) {
    if (packet_len < BX_DHCP_PACKET_MIN_LEN) {
        return false;
    }

    if (memcmp(packet + BX_DHCP_BOOTP_FIXED_LEN, bx_dhcp_magic_cookie, sizeof(bx_dhcp_magic_cookie)) != 0) {
        return false;
    }

    memset(message_out, 0, sizeof(*message_out));

    uint32_t xid_net;
    memcpy(&xid_net, packet + 4u, sizeof(xid_net));
    message_out->xid = ntohl(xid_net);

    memcpy(&message_out->yiaddr.s_addr, packet + 16u, sizeof(message_out->yiaddr.s_addr));

    size_t offset = BX_DHCP_PACKET_MIN_LEN;
    while (offset < packet_len) {
        uint8_t code = packet[offset++];
        if (code == BX_DHCP_OPTION_PAD) {
            continue;
        }
        if (code == BX_DHCP_OPTION_END) {
            break;
        }
        if (offset >= packet_len) {
            return false;
        }

        uint8_t option_len = packet[offset++];
        if (offset + (size_t)option_len > packet_len) {
            return false;
        }

        const unsigned char* option_data = packet + offset;
        switch (code) {
            case BX_DHCP_OPTION_MESSAGE_TYPE:
                if (option_len == 1u) {
                    message_out->message_type = option_data[0];
                }
                break;
            case BX_DHCP_OPTION_SERVER_ID:
                if (option_len == 4u) {
                    memcpy(&message_out->server_id.s_addr, option_data, 4u);
                    message_out->have_server_id = true;
                }
                break;
            case BX_DHCP_OPTION_SUBNET_MASK:
                if (option_len == 4u) {
                    memcpy(&message_out->subnet_mask.s_addr, option_data, 4u);
                    message_out->have_subnet_mask = true;
                }
                break;
            case BX_DHCP_OPTION_ROUTER:
            case BX_DHCP_OPTION_DNS: {
                struct bx_dhcp_addresses* list = code == BX_DHCP_OPTION_ROUTER ? &message_out->router : &message_out->dns;
                size_t n = option_len / sizeof(struct in_addr);
                if (!n || option_len % sizeof(struct in_addr) || n > BX_DHCP_MAX_ADDRS - list->count)
                    return false;
                memcpy(list->values + list->count, option_data, option_len);
                list->count += n;
                break;
            }
            case BX_DHCP_OPTION_T1:
            case BX_DHCP_OPTION_T2:
                if (option_len == 4u) {
                    uint32_t seconds;
                    memcpy(&seconds, option_data, sizeof(seconds));
                    if (code == BX_DHCP_OPTION_T1)
                        message_out->t1 = ntohl(seconds);
                    else
                        message_out->t2 = ntohl(seconds);
                }
                break;
            case BX_DHCP_OPTION_LEASE_TIME:
                if (option_len == 4u) {
                    uint32_t lease_time_net;
                    memcpy(&lease_time_net, option_data, sizeof(lease_time_net));
                    message_out->lease_time = ntohl(lease_time_net);
                    message_out->have_lease_time = true;
                }
                break;
            default:
                break;
        }

        offset += (size_t)option_len;
    }

    return message_out->message_type != 0u;
}

static enum bx_dhcp_wait_result bx_dhcp_wait_for_message(int fd,
                                                         const struct bx_dhcp_options* options,
                                                         uint32_t xid,
                                                         const unsigned char hwaddr[6],
                                                         const struct bx_dhcp_message* offer,
                                                         bool rebinding,
                                                         int timeout_ms,
                                                         struct bx_dhcp_message* message_out,
                                                         struct bx_diag_ctx* diag) {
    struct bx_poll_deadline deadline;
    bx_poll_deadline_init(&deadline, timeout_ms);

    while (true) {
        struct pollfd pfds[] = {{.fd = fd, .events = POLLIN}, {.fd = options->stop_fd, .events = POLLIN}};
        int poll_rc = bx_poll_deadline_wait(&deadline, pfds, 2);

        if (poll_rc < 0) {
            bx_diag(diag, "failed while waiting for DHCP response: %s", strerror(errno));
            return BX_DHCP_WAIT_ERROR;
        }
        if (poll_rc == 0) {
            return BX_DHCP_WAIT_TIMEOUT;
        }

        if (pfds[1].revents)
            return BX_DHCP_WAIT_STOP;
        unsigned char packet[BX_DHCP_PACKET_MAX_LEN];
        struct sockaddr_in source;
        socklen_t source_len = sizeof(source);
        ssize_t recv_len = recvfrom(fd, packet, sizeof(packet), MSG_TRUNC, (struct sockaddr*)&source, &source_len);
        if (recv_len < 0) {
            if (errno == EINTR) {
                continue;
            }
            bx_diag(diag, "failed to read DHCP response: %s", strerror(errno));
            return BX_DHCP_WAIT_ERROR;
        }
        if (recv_len < (ssize_t)BX_DHCP_PACKET_MIN_LEN || (size_t)recv_len > sizeof(packet) || source.sin_port != htons(options->server_port) || packet[0] != 2u || packet[1] != 1u ||
            packet[2] != 6u || memcmp(packet + 28u, hwaddr, 6u) != 0) {
            continue;
        }

        struct bx_dhcp_message message;
        if (!bx_dhcp_parse_packet(packet, (size_t)recv_len, &message)) {
            continue;
        }
        if (message.xid != xid) {
            continue;
        }

        if (!message.have_server_id) {
            message.server_id = source.sin_addr;
            message.have_server_id = true;
        }

        if (offer == NULL) {
            if (message.message_type != BX_DHCP_MESSAGE_OFFER || message.yiaddr.s_addr == INADDR_ANY)
                continue;
        }
        else if ((!rebinding && message.server_id.s_addr != offer->server_id.s_addr) ||
                 (message.message_type != BX_DHCP_MESSAGE_NAK &&
                  (message.message_type != BX_DHCP_MESSAGE_ACK || (message.yiaddr.s_addr != INADDR_ANY && message.yiaddr.s_addr != offer->yiaddr.s_addr)))) {
            continue;
        }
        *message_out = message;
        return BX_DHCP_WAIT_MESSAGE;
    }
}

static bool bx_dhcp_send_packet(int fd, const struct sockaddr_in* destination, const unsigned char* packet, size_t packet_len, struct bx_diag_ctx* diag) {
    ssize_t sent_len = sendto(fd, packet, packet_len, 0, (const struct sockaddr*)destination, sizeof(*destination));
    if (sent_len < 0) {
        bx_diag(diag, "failed to send DHCP packet: %s", strerror(errno));
        return false;
    }

    if ((size_t)sent_len != packet_len) {
        bx_diag(diag, "short DHCP packet write");
        return false;
    }

    return true;
}

static void bx_dhcp_merge_lease(struct bx_dhcp_message* ack, const struct bx_dhcp_message* offer) {
    if (ack->yiaddr.s_addr == INADDR_ANY)
        ack->yiaddr = offer->yiaddr;
    if (!ack->have_subnet_mask) {
        ack->subnet_mask = offer->subnet_mask;
        ack->have_subnet_mask = offer->have_subnet_mask;
    }
    if (!ack->router.count)
        ack->router = offer->router;
    if (!ack->dns.count)
        ack->dns = offer->dns;
}

static bool bx_dhcp_clock(uint64_t* now, struct bx_diag_ctx* diag) {
    struct timespec ts;
    uintmax_t ms;
    if (clock_gettime(CLOCK_BOOTTIME, &ts) != 0 || !bx_time_timespec_to_milliseconds_uint(&ts, &ms) || ms > UINT64_MAX - (uint64_t)UINT32_MAX * 1000u) {
        bx_diag(diag, "cannot read lease clock");
        return false;
    }
    *now = (uint64_t)ms;
    return true;
}

static bool bx_dhcp_acquire_lease(int fd, const unsigned char hwaddr[6], const struct bx_dhcp_options* options, struct bx_dhcp_message* lease_out, uint64_t* started, struct bx_diag_ctx* diag) {
    bool success = false;
    unsigned char packet[BX_DHCP_PACKET_MAX_LEN];

    struct sockaddr_in destination;
    memset(&destination, 0, sizeof(destination));
    destination.sin_family = AF_INET;
    destination.sin_port = htons(options->server_port);
    destination.sin_addr = options->server_addr;

    for (unsigned int attempt = 0; attempt < options->retries; attempt++) {
        uint32_t xid = bx_dhcp_generate_xid();
        size_t discover_len = bx_dhcp_build_discover(packet, sizeof(packet), xid, hwaddr);
        if (discover_len == 0u) {
            bx_diag(diag, "internal error: failed to build DHCPDISCOVER");
            break;
        }
        if (!bx_dhcp_send_packet(fd, &destination, packet, discover_len, diag)) {
            break;
        }

        struct bx_dhcp_message offer;
        enum bx_dhcp_wait_result wait_offer = bx_dhcp_wait_for_message(fd, options, xid, hwaddr, NULL, false, (int)options->timeout_ms, &offer, diag);
        if (wait_offer == BX_DHCP_WAIT_TIMEOUT) {
            continue;
        }
        if (wait_offer == BX_DHCP_WAIT_STOP)
            return false;
        if (wait_offer == BX_DHCP_WAIT_ERROR) {
            break;
        }

        size_t request_len = bx_dhcp_build_request(packet, sizeof(packet), xid, hwaddr, &offer.yiaddr, &offer.server_id);
        if (request_len == 0u) {
            bx_diag(diag, "internal error: failed to build DHCPREQUEST");
            break;
        }
        if (!bx_dhcp_clock(started, diag) || !bx_dhcp_send_packet(fd, &destination, packet, request_len, diag)) {
            break;
        }

        struct bx_dhcp_message reply;
        enum bx_dhcp_wait_result wait_reply = bx_dhcp_wait_for_message(fd, options, xid, hwaddr, &offer, false, (int)options->timeout_ms, &reply, diag);
        if (wait_reply == BX_DHCP_WAIT_TIMEOUT) {
            continue;
        }
        if (wait_reply == BX_DHCP_WAIT_STOP)
            return false;
        if (wait_reply == BX_DHCP_WAIT_ERROR) {
            break;
        }
        if (reply.message_type == BX_DHCP_MESSAGE_NAK) {
            continue;
        }

        bx_dhcp_merge_lease(&reply, &offer);
        if (!reply.have_lease_time && options->script == NULL) {
            reply.lease_time = offer.lease_time;
            reply.have_lease_time = offer.have_lease_time;
        }
        *lease_out = reply;
        success = true;
        break;
    }

    if (!success && diag->exit_status == 0) {
        bx_diag(diag, "failed to acquire DHCP lease on '%s' after %u attempt(s)", options->ifname, options->retries);
    }

    return success;
}

static void bx_dhcp_format_address(const struct in_addr* addr, char* buffer, size_t buffer_size) {
    if (buffer_size == 0u) {
        return;
    }

    if (inet_ntop(AF_INET, addr, buffer, (socklen_t)buffer_size) == NULL) {
        snprintf(buffer, buffer_size, "0.0.0.0");
    }
}

static void bx_dhcp_print_lease(const struct bx_dhcp_options* options, const struct bx_dhcp_message* lease) {
    char address_text[INET_ADDRSTRLEN];
    bx_dhcp_format_address(&lease->yiaddr, address_text, sizeof(address_text));

    printf("INTERFACE=%s\n", options->ifname);
    printf("IP=%s\n", address_text);

    if (lease->have_server_id) {
        char server_text[INET_ADDRSTRLEN];
        bx_dhcp_format_address(&lease->server_id, server_text, sizeof(server_text));
        printf("SERVER=%s\n", server_text);
    }

    if (lease->have_subnet_mask) {
        char subnet_text[INET_ADDRSTRLEN];
        bx_dhcp_format_address(&lease->subnet_mask, subnet_text, sizeof(subnet_text));
        printf("SUBNET=%s\n", subnet_text);
    }

    if (lease->router.count) {
        char router_text[INET_ADDRSTRLEN];
        bx_dhcp_format_address(lease->router.values, router_text, sizeof(router_text));
        printf("ROUTER=%s\n", router_text);
    }

    if (lease->dns.count) {
        char dns_text[INET_ADDRSTRLEN];
        bx_dhcp_format_address(lease->dns.values, dns_text, sizeof(dns_text));
        printf("DNS=%s\n", dns_text);
    }

    if (lease->have_lease_time) {
        printf("LEASE=%" PRIu32 "\n", lease->lease_time);
    }
}

struct bx_dhcp_event {
    const struct bx_dhcp_options* options;
    const struct bx_dhcp_message* lease;
    const char* name;
};

static bool bx_dhcp_event_addr(const char* key, const struct in_addr* address) {
    char text[INET_ADDRSTRLEN];
    if (!address)
        return unsetenv(key) == 0;
    return inet_ntop(AF_INET, address, text, sizeof(text)) != NULL && setenv(key, text, 1) == 0;
}

static bool bx_dhcp_event_addrs(const char* key, const struct bx_dhcp_addresses* list) {
    if (!list || !list->count)
        return unsetenv(key) == 0;
    char text[BX_DHCP_MAX_ADDRS * INET_ADDRSTRLEN];
    char* end = text;
    for (size_t i = 0; i < list->count; ++i) {
        if (i)
            *end++ = ' ';
        if (!inet_ntop(AF_INET, &list->values[i], end, INET_ADDRSTRLEN))
            return false;
        end += strlen(end);
    }
    return setenv(key, text, 1) == 0;
}

static int bx_dhcp_exec_script(void* data) {
    const struct bx_dhcp_event* event = data;
    const struct bx_dhcp_message* lease = event->lease;
    if (sigprocmask(SIG_SETMASK, &event->options->caller_mask, NULL) < 0)
        return 1;
    if (setenv("interface", event->options->ifname, 1) < 0 || !bx_dhcp_event_addr("ip", lease ? &lease->yiaddr : NULL) ||
        !bx_dhcp_event_addr("subnet", lease && lease->have_subnet_mask ? &lease->subnet_mask : NULL) || !bx_dhcp_event_addrs("router", lease ? &lease->router : NULL) ||
        !bx_dhcp_event_addrs("dns", lease ? &lease->dns : NULL))
        return 1;
    execl(event->options->script, event->options->script, event->name, (char*)NULL);
    perror(event->options->script);
    return 1;
}

static bool bx_dhcp_notify(const struct bx_dhcp_options* options, const char* name, const struct bx_dhcp_message* lease, struct bx_diag_ctx* diag) {
    struct bx_dhcp_event event = {options, lease, name};
    int status;
    if (bx_child_fork_callback_wait(bx_dhcp_exec_script, &event, BX_CHILD_SIGNALS_RESET_CAUGHT, &status) < 0) {
        bx_diag(diag, "cannot run %s for %s: %s", options->script, name, strerror(errno));
        return false;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        bx_diag(diag, "%s failed for %s (status %d)", options->script, name, WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status));
        return false;
    }
    return true;
}

/* A zero deadline waits indefinitely. Any stop signal stays queued until cleanup. */
static bool bx_dhcp_sleep_until(int stop_fd, uint64_t when, struct bx_diag_ctx* diag) {
    int timer = -1;
    if (when) {
        struct itimerspec spec = {.it_value = {.tv_sec = (time_t)(when / 1000u), .tv_nsec = (long)((when % 1000u) * 1000000u)}};
        timer = timerfd_create(CLOCK_BOOTTIME, TFD_CLOEXEC);
        if (timer < 0 || timerfd_settime(timer, TFD_TIMER_ABSTIME, &spec, NULL) < 0) {
            bx_diag(diag, "lease timer: %s", strerror(errno));
            if (timer >= 0)
                close(timer);
            return false;
        }
    }
    struct pollfd pfds[] = {{.fd = stop_fd, .events = POLLIN}, {.fd = timer, .events = POLLIN}};
    int rc = bx_poll_retry_eintr(pfds, 2, -1);
    if (rc < 0)
        bx_diag(diag, "lease wait: %s", strerror(errno));
    if (timer >= 0)
        close(timer);
    return rc > 0 && !pfds[0].revents && pfds[1].revents == POLLIN;
}

/* Return to discovery on expiry or NAK. Never extend a deadline without an ACK. */
static bool bx_dhcp_maintain(int fd, const unsigned char hwaddr[6], const struct bx_dhcp_options* options, struct bx_dhcp_message lease, uint64_t start, struct bx_diag_ctx* diag) {
    const char* event = "bound";
    for (;;) {
        if (!lease.have_lease_time || !lease.lease_time) {
            bx_diag(diag, "server did not grant a lease duration");
            return true;
        }
        uint64_t duration = (uint64_t)lease.lease_time * 1000u;
        uint64_t t1 = lease.t1 ? (uint64_t)lease.t1 * 1000u : duration / 2u;
        uint64_t t2 = lease.t2 ? (uint64_t)lease.t2 * 1000u : duration * 7u / 8u;
        if (t1 >= t2 || t2 >= duration) {
            t1 = duration / 2u;
            t2 = duration * 7u / 8u;
        }
        uint64_t now;
        if (!bx_dhcp_clock(&now, diag))
            return false;
        if (lease.lease_time != UINT32_MAX && now >= start + duration)
            return true;
        if (!bx_dhcp_notify(options, event, &lease, diag))
            return false;
        if (lease.lease_time == UINT32_MAX) {
            return bx_dhcp_sleep_until(options->stop_fd, 0, diag);
        }
        if (!bx_dhcp_sleep_until(options->stop_fd, start + t1, diag))
            return false;
        uint32_t xid = bx_dhcp_generate_xid();
        unsigned char packet[BX_DHCP_PACKET_MAX_LEN];
        size_t size = bx_dhcp_build_request(packet, sizeof(packet), xid, hwaddr, &lease.yiaddr, NULL);
        if (!size) {
            bx_diag(diag, "cannot build renewal request");
            return false;
        }
        for (;;) {
            if (!bx_dhcp_clock(&now, diag))
                return false;
            if (now >= start + duration)
                return true;
            bool rebind = now >= start + t2;
            uint64_t limit = start + (rebind ? duration : t2);
            uint64_t interval = limit - now;
            if (interval > options->timeout_ms)
                interval = options->timeout_ms;
            struct sockaddr_in dest = {.sin_family = AF_INET, .sin_port = htons(options->server_port), .sin_addr = rebind ? options->server_addr : lease.server_id};
            struct bx_dhcp_message reply;
            if (bx_dhcp_send_packet(fd, &dest, packet, size, diag)) {
                enum bx_dhcp_wait_result result = bx_dhcp_wait_for_message(fd, options, xid, hwaddr, &lease, rebind, (int)interval, &reply, diag);
                if (result == BX_DHCP_WAIT_STOP)
                    return false;
                if (result == BX_DHCP_WAIT_MESSAGE) {
                    uint64_t received;
                    if (!bx_dhcp_clock(&received, diag))
                        return false;
                    if (received >= start + duration || reply.message_type == BX_DHCP_MESSAGE_NAK)
                        return true;
                    bx_dhcp_merge_lease(&reply, &lease);
                    lease = reply;
                    start = now;
                    event = "renew";
                    break;
                }
            }
            if (!bx_dhcp_sleep_until(options->stop_fd, now + interval, diag))
                return false;
        }
    }
}

static int bx_dhcp_run(const struct bx_dhcp_options* options, struct bx_diag_ctx* diag) {
    if (options->script && !bx_dhcp_notify(options, "deconfig", NULL, diag))
        return 1;
    for (;;) {
        diag->exit_status = 0;
        unsigned char hwaddr[6];
        struct bx_dhcp_message lease;
        uint64_t started;
        int fd = bx_dhcp_get_hwaddr(options->ifname, hwaddr, diag) ? bx_dhcp_open_socket(options, diag) : -1;
        bool acquired = fd >= 0 && bx_dhcp_acquire_lease(fd, hwaddr, options, &lease, &started, diag);
        if (!options->script) {
            if (fd >= 0)
                close(fd);
            if (!acquired)
                return 1;
            bx_dhcp_print_lease(options, &lease);
            return 0;
        }
        bool retry = !acquired || bx_dhcp_maintain(fd, hwaddr, options, lease, started, diag);
        if (fd >= 0)
            close(fd);
        if (acquired && !bx_dhcp_notify(options, "deconfig", NULL, diag))
            return 1;
        struct pollfd stop = {.fd = options->stop_fd, .events = POLLIN};
        int rc = bx_poll_for_milliseconds(&stop, 1, retry ? (int)options->timeout_ms : 0);
        if (rc > 0 && stop.revents == POLLIN)
            return 0;
        if (!retry || rc < 0)
            return 1;
    }
}

int bx_dhcp_main(int argc, char** argv) {
    struct bx_dhcp_options options;
    struct bx_diag_ctx diag = {
        .progname = "dhcp",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };

    if (!bx_dhcp_parse_options(argc, argv, &options, &diag)) {
        bx_cli_print_try_help(options.progname);
        return (diag.exit_status != 0) ? diag.exit_status : 1;
    }

    if (options.show_help) {
        bx_dhcp_print_help(stdout, options.progname);
        return 0;
    }

    if (options.show_version) {
        bx_cli_print_version(options.progname);
        return 0;
    }

    if (!options.script)
        return bx_dhcp_run(&options, &diag);
    sigset_t stop;
    sigemptyset(&stop);
    sigaddset(&stop, SIGTERM);
    sigaddset(&stop, SIGINT);
    if (sigprocmask(SIG_BLOCK, &stop, &options.caller_mask) < 0) {
        bx_diag(&diag, "cannot block stop signals: %s", strerror(errno));
        return 1;
    }
    options.stop_fd = bx_fd_signalfd_cloexec(-1, &stop, SFD_NONBLOCK);
    int status = 1;
    if (options.stop_fd >= 0) {
        status = bx_dhcp_run(&options, &diag);
        struct signalfd_siginfo info;
        while (read(options.stop_fd, &info, sizeof(info)) > 0) {
        }
        close(options.stop_fd);
    }
    else
        bx_diag(&diag, "cannot monitor stop signals: %s", strerror(errno));
    if (sigprocmask(SIG_SETMASK, &options.caller_mask, NULL) < 0)
        return 1;
    return status;
}
