#define _DEFAULT_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "applets.h"
#include "diag.h"

enum bx_ip_object {
    BX_IP_OBJECT_NONE = 0,
    BX_IP_OBJECT_LINK,
    BX_IP_OBJECT_ADDR,
};

struct bx_ip_options {
    const char* progname;
    bool show_help;
    bool show_version;
    enum bx_ip_object object;
    const char* device;
};

static const char* bx_ip_progname(const char* argv0) {
    if (argv0 == NULL || argv0[0] == '\0') {
        return "ip";
    }

    const char* base = strrchr(argv0, '/');
    if (base != NULL && base[1] != '\0') {
        return base + 1;
    }

    return argv0;
}

static void bx_ip_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... OBJECT [COMMAND [ARG]...]\n", progname);
    fprintf(stream, "Inspect network links and addresses.\n");
    fprintf(stream, "\n");
    fprintf(stream, "Supported object/command forms in this phase:\n");
    fprintf(stream, "  %s link [show|list] [dev IFACE]\n", progname);
    fprintf(stream, "  %s addr|address [show|list] [dev IFACE]\n", progname);
    fprintf(stream, "\n");
    fprintf(stream, "  -h, --help     display this help and exit\n");
    fprintf(stream, "  -V, --version  output version information and exit\n");
}

static void bx_ip_print_try_help(const char* progname) {
    fprintf(stderr, "Try '%s --help' for more information.\n", progname);
}

static void bx_ip_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

static bool bx_ip_parse_object(const char* text, enum bx_ip_object* object_out) {
    if (text == NULL || text[0] == '\0') {
        return false;
    }

    if (strcmp(text, "link") == 0) {
        *object_out = BX_IP_OBJECT_LINK;
        return true;
    }

    if (strcmp(text, "addr") == 0 || strcmp(text, "address") == 0) {
        *object_out = BX_IP_OBJECT_ADDR;
        return true;
    }

    return false;
}

static bool bx_ip_is_show_command(const char* text) {
    return text != NULL && (strcmp(text, "show") == 0 || strcmp(text, "list") == 0);
}

static bool bx_ip_parse_options(int argc, char** argv, struct bx_ip_options* options, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"help", no_argument, NULL, 'h'},
        {"version", no_argument, NULL, 'V'},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_ip_progname((argc > 0) ? argv[0] : NULL);
    diag->progname = options->progname;

    opterr = 0;
    optind = 1;

    while (true) {
        int c = getopt_long(argc, argv, "+hV", long_options, NULL);
        if (c == -1) {
            break;
        }

        switch (c) {
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
        bx_diag(diag, "missing OBJECT operand");
        return false;
    }

    const char* object_text = argv[optind++];
    if (!bx_ip_parse_object(object_text, &options->object)) {
        bx_diag(diag, "unsupported object '%s'", object_text);
        return false;
    }

    if (optind < argc && !bx_ip_is_show_command(argv[optind])) {
        bx_diag(diag, "unsupported command '%s' for object '%s'", argv[optind], object_text);
        return false;
    }

    if (optind < argc) {
        optind++;
    }

    if (optind < argc && strcmp(argv[optind], "dev") == 0) {
        optind++;
        if (optind >= argc) {
            bx_diag(diag, "missing interface name after 'dev'");
            return false;
        }
    }

    if (optind < argc) {
        options->device = argv[optind++];
        if (options->device[0] == '\0') {
            bx_diag(diag, "interface name may not be empty");
            return false;
        }
    }

    if (optind < argc) {
        bx_diag(diag, "extra operand '%s'", argv[optind]);
        return false;
    }

    return true;
}

static void bx_ip_append_flag(char* buffer, size_t buffer_size, bool* first, const char* text) {
    if (buffer_size == 0) {
        return;
    }

    size_t used = strlen(buffer);
    if (used >= buffer_size - 1) {
        return;
    }

    int written = snprintf(buffer + used, buffer_size - used, "%s%s", (*first) ? "" : ",", text);
    if (written > 0) {
        *first = false;
    }
}

static void bx_ip_format_flags(unsigned int flags, char* buffer, size_t buffer_size) {
    if (buffer_size == 0) {
        return;
    }

    buffer[0] = '\0';
    bool first = true;

    if ((flags & IFF_UP) != 0) {
        bx_ip_append_flag(buffer, buffer_size, &first, "UP");
    }
    if ((flags & IFF_BROADCAST) != 0) {
        bx_ip_append_flag(buffer, buffer_size, &first, "BROADCAST");
    }
    if ((flags & IFF_LOOPBACK) != 0) {
        bx_ip_append_flag(buffer, buffer_size, &first, "LOOPBACK");
    }
    if ((flags & IFF_RUNNING) != 0) {
        bx_ip_append_flag(buffer, buffer_size, &first, "RUNNING");
    }
    if ((flags & IFF_MULTICAST) != 0) {
        bx_ip_append_flag(buffer, buffer_size, &first, "MULTICAST");
    }

#ifdef IFF_LOWER_UP
    if ((flags & IFF_LOWER_UP) != 0) {
        bx_ip_append_flag(buffer, buffer_size, &first, "LOWER_UP");
    }
#endif

    if (buffer[0] == '\0') {
        snprintf(buffer, buffer_size, "NOFLAGS");
    }
}

static void bx_ip_trim_newline(char* text) {
    if (text == NULL) {
        return;
    }

    size_t len = strlen(text);
    while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r')) {
        text[len - 1] = '\0';
        len--;
    }
}

static const char* bx_ip_read_operstate(const char* ifname, char* buffer, size_t buffer_size) {
    if (ifname == NULL || buffer == NULL || buffer_size == 0) {
        return NULL;
    }

    char path[256];
    int path_len = snprintf(path, sizeof(path), "/sys/class/net/%s/operstate", ifname);
    if (path_len <= 0 || (size_t)path_len >= sizeof(path)) {
        return NULL;
    }

    FILE* stream = fopen(path, "r");
    if (stream == NULL) {
        return NULL;
    }

    char* line = fgets(buffer, (int)buffer_size, stream);
    fclose(stream);
    if (line == NULL) {
        return NULL;
    }

    bx_ip_trim_newline(buffer);
    if (buffer[0] == '\0') {
        return NULL;
    }

    return buffer;
}

static void bx_ip_print_link_address_line(int sockfd, const char* ifname) {
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    ifr.ifr_name[IFNAMSIZ - 1] = '\0';

    if (ioctl(sockfd, SIOCGIFHWADDR, &ifr) != 0) {
        return;
    }

    int family = ifr.ifr_hwaddr.sa_family;
    const unsigned char* bytes = (const unsigned char*)ifr.ifr_hwaddr.sa_data;

    if (family == ARPHRD_ETHER || family == ARPHRD_LOOPBACK) {
        const char* label = (family == ARPHRD_ETHER) ? "ether" : "loopback";
        printf("    link/%s %02x:%02x:%02x:%02x:%02x:%02x\n", label, bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5]);
        return;
    }

    printf("    link/%d\n", family);
}

static bool bx_ip_print_link_entry(int sockfd, const char* ifname, struct bx_diag_ctx* diag) {
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    ifr.ifr_name[IFNAMSIZ - 1] = '\0';

    if (ioctl(sockfd, SIOCGIFFLAGS, &ifr) != 0) {
        bx_diag(diag, "failed to read link flags for '%s': %s", ifname, strerror(errno));
        return false;
    }
    unsigned int flags = (unsigned int)(unsigned short)ifr.ifr_flags;

    if (ioctl(sockfd, SIOCGIFMTU, &ifr) != 0) {
        bx_diag(diag, "failed to read MTU for '%s': %s", ifname, strerror(errno));
        return false;
    }
    int mtu = ifr.ifr_mtu;

    char flag_buffer[192];
    bx_ip_format_flags(flags, flag_buffer, sizeof(flag_buffer));

    char state_buffer[32];
    const char* state = bx_ip_read_operstate(ifname, state_buffer, sizeof(state_buffer));
    if (state == NULL) {
        state = ((flags & IFF_UP) != 0) ? "unknown" : "down";
    }

    unsigned int index = if_nametoindex(ifname);
    printf("%u: %s: <%s> mtu %d state %s\n", index, ifname, flag_buffer, mtu, state);
    bx_ip_print_link_address_line(sockfd, ifname);
    return true;
}

static bool bx_ip_show_link(const struct bx_ip_options* options, struct bx_diag_ctx* diag) {
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        bx_diag(diag, "failed to open ioctl socket: %s", strerror(errno));
        return false;
    }

    struct if_nameindex* interfaces = if_nameindex();
    if (interfaces == NULL) {
        bx_diag(diag, "failed to enumerate interfaces: %s", strerror(errno));
        close(sockfd);
        return false;
    }

    bool matched = false;
    bool success = true;
    for (const struct if_nameindex* item = interfaces; item->if_index != 0 && item->if_name != NULL; item++) {
        if (options->device != NULL && strcmp(options->device, item->if_name) != 0) {
            continue;
        }

        matched = true;
        if (!bx_ip_print_link_entry(sockfd, item->if_name, diag)) {
            success = false;
            break;
        }
    }

    if (!matched && options->device != NULL) {
        bx_diag(diag, "device '%s' does not exist", options->device);
        success = false;
    }

    if_freenameindex(interfaces);
    close(sockfd);
    return success;
}

static int bx_ip_prefixlen_from_netmask(const struct sockaddr* netmask) {
    if (netmask == NULL) {
        return 0;
    }

    if (netmask->sa_family == AF_INET) {
        uint32_t value = ntohl(((const struct sockaddr_in*)netmask)->sin_addr.s_addr);
        int prefix = 0;
        while ((value & 0x80000000U) != 0U) {
            prefix++;
            value <<= 1;
        }
        return prefix;
    }

    if (netmask->sa_family == AF_INET6) {
        const uint8_t* bytes = ((const uint8_t*)&((const struct sockaddr_in6*)netmask)->sin6_addr);
        int prefix = 0;
        for (size_t i = 0; i < 16; i++) {
            uint8_t value = bytes[i];
            for (int bit = 7; bit >= 0; bit--) {
                if (((value >> bit) & 1U) == 0U) {
                    return prefix;
                }
                prefix++;
            }
        }
        return prefix;
    }

    return 0;
}

static bool bx_ip_print_addr_for_interface(const char* ifname, unsigned int index, const struct ifaddrs* ifaddr_list, struct bx_diag_ctx* diag) {
    bool printed_header = false;

    for (const struct ifaddrs* ifa = ifaddr_list; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_name == NULL || strcmp(ifa->ifa_name, ifname) != 0 || ifa->ifa_addr == NULL) {
            continue;
        }

        int family = ifa->ifa_addr->sa_family;
        char addr_text[INET6_ADDRSTRLEN];
        int prefix = bx_ip_prefixlen_from_netmask(ifa->ifa_netmask);

        if (family == AF_INET) {
            const struct sockaddr_in* sin = (const struct sockaddr_in*)ifa->ifa_addr;
            if (inet_ntop(AF_INET, &sin->sin_addr, addr_text, sizeof(addr_text)) == NULL) {
                bx_diag(diag, "failed to format IPv4 address for '%s': %s", ifname, strerror(errno));
                return false;
            }
        }
        else if (family == AF_INET6) {
            const struct sockaddr_in6* sin6 = (const struct sockaddr_in6*)ifa->ifa_addr;
            if (inet_ntop(AF_INET6, &sin6->sin6_addr, addr_text, sizeof(addr_text)) == NULL) {
                bx_diag(diag, "failed to format IPv6 address for '%s': %s", ifname, strerror(errno));
                return false;
            }
        }
        else {
            continue;
        }

        if (!printed_header) {
            printf("%u: %s\n", index, ifname);
            printed_header = true;
        }

        if (family == AF_INET) {
            printf("    inet %s/%d\n", addr_text, prefix);
        }
        else {
            printf("    inet6 %s/%d\n", addr_text, prefix);
        }
    }

    return true;
}

static bool bx_ip_show_addr(const struct bx_ip_options* options, struct bx_diag_ctx* diag) {
    struct ifaddrs* ifaddr_list = NULL;
    if (getifaddrs(&ifaddr_list) != 0) {
        bx_diag(diag, "failed to enumerate interface addresses: %s", strerror(errno));
        return false;
    }

    struct if_nameindex* interfaces = if_nameindex();
    if (interfaces == NULL) {
        bx_diag(diag, "failed to enumerate interfaces: %s", strerror(errno));
        freeifaddrs(ifaddr_list);
        return false;
    }

    bool matched = false;
    bool success = true;
    for (const struct if_nameindex* item = interfaces; item->if_index != 0 && item->if_name != NULL; item++) {
        if (options->device != NULL && strcmp(options->device, item->if_name) != 0) {
            continue;
        }

        matched = true;
        if (!bx_ip_print_addr_for_interface(item->if_name, item->if_index, ifaddr_list, diag)) {
            success = false;
            break;
        }
    }

    if (!matched && options->device != NULL) {
        bx_diag(diag, "device '%s' does not exist", options->device);
        success = false;
    }

    if_freenameindex(interfaces);
    freeifaddrs(ifaddr_list);
    return success;
}

int bx_ip_main(int argc, char** argv) {
    struct bx_ip_options options;
    struct bx_diag_ctx diag = {
        .progname = "ip",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };

    if (!bx_ip_parse_options(argc, argv, &options, &diag)) {
        bx_ip_print_try_help(options.progname);
        return (diag.exit_status != 0) ? diag.exit_status : 1;
    }

    if (options.show_help) {
        bx_ip_print_help(stdout, options.progname);
        return 0;
    }

    if (options.show_version) {
        bx_ip_print_version(options.progname);
        return 0;
    }

    bool success = false;
    switch (options.object) {
        case BX_IP_OBJECT_LINK:
            success = bx_ip_show_link(&options, &diag);
            break;
        case BX_IP_OBJECT_ADDR:
            success = bx_ip_show_addr(&options, &diag);
            break;
        default:
            bx_diag(&diag, "internal error: unsupported object");
            success = false;
            break;
    }

    if (!success) {
        return (diag.exit_status != 0) ? diag.exit_status : 1;
    }

    return 0;
}
