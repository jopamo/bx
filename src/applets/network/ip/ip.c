#define _DEFAULT_SOURCE
#include "applets.h"
#include "bx/diag.h"

static void ip_help(FILE* out) {
    fputs("usage: ip link set [dev] IFACE {up|down}\n"
          "       ip [-4] {a|addr|address} [show [dev IFACE]]\n"
          "       ip [-4] {addr|address} {replace|del} IPV4/PREFIX dev IFACE\n"
          "       ip [-4] route {replace|del} default via IPV4 dev IFACE [metric NUMBER]\n"
          "PREFIX is a CIDR length or a contiguous dotted netmask. Linux only.\n", out);
}

#ifdef __linux__
#include <arpa/inet.h>
#include <limits.h>
#include <net/if.h>
#include <stdint.h>

#include "lib/size_parse.h"
#include "rtnetlink.h"

enum ip_action { IP_REPLACE, IP_DELETE };

static bool ip_action(const char* word, enum ip_action* action) {
    if (strcmp(word, "replace") == 0)
        *action = IP_REPLACE;
    else if (strcmp(word, "del") == 0 || strcmp(word, "delete") == 0)
        *action = IP_DELETE;
    else
        return false;
    return true;
}

static int ip_usage(struct bx_diag_ctx* diag) {
    bx_diag(diag, "invalid or unsupported arguments (see ip --help)");
    return 1;
}

static unsigned int ip_ifindex(const char* interface, struct bx_diag_ctx* diag) {
    if (interface[0] == '\0' || strlen(interface) >= IF_NAMESIZE ||
        strchr(interface, '/') != NULL || strchr(interface, ':') != NULL) {
        bx_diag(diag, "invalid interface name: '%s'", interface);
        return 0;
    }
    unsigned int index = if_nametoindex(interface);
    if (index == 0)
        bx_perror_path(diag, interface);
    return index;
}

static int ip_apply(void* request, const char* action, const char* interface, struct bx_diag_ctx* diag) {
    if (bx_rtnl_apply(request) == 0)
        return 0;
    bx_diag(diag, "%s on '%s': %s", action, interface, bx_strerror(errno));
    return 1;
}

static int ip_link(int argc, char** argv, struct bx_diag_ctx* diag) {
    if (argc < 1 || strcmp(argv[0], "set") != 0)
        return ip_usage(diag);
    if (argc == 4 && strcmp(argv[1], "dev") == 0) {
        argc--;
        argv++;
    }
    if (argc != 3 || (strcmp(argv[2], "up") != 0 && strcmp(argv[2], "down") != 0))
        return ip_usage(diag);
    unsigned int index = ip_ifindex(argv[1], diag);
    if (index == 0)
        return 1;
    if (index > INT_MAX)
        return ip_usage(diag);
    struct {
        struct nlmsghdr header;
        struct ifinfomsg link;
    } request = {
        .header = {.nlmsg_len = NLMSG_LENGTH(sizeof(request.link)), .nlmsg_type = RTM_NEWLINK},
        .link = {.ifi_family = AF_UNSPEC, .ifi_index = (int)index, .ifi_change = IFF_UP,
                 .ifi_flags = strcmp(argv[2], "up") == 0 ? IFF_UP : 0},
    };
    return ip_apply(&request, "link set", argv[1], diag);
}

static bool ip_parse_prefix(const char* text, struct in_addr* local, unsigned char* bits) {
    const char* slash = strchr(text, '/');
    char host[INET_ADDRSTRLEN];
    if (slash == NULL || slash == text || (size_t)(slash - text) >= sizeof(host))
        return false;
    size_t length = (size_t)(slash - text);
    memcpy(host, text, length);
    host[length] = '\0';
    if (inet_pton(AF_INET, host, local) != 1)
        return false;
    uintmax_t number;
    if (bx_size_parse_uint(slash + 1, &number)) {
        if (number > 32)
            return false;
        *bits = (unsigned char)number;
    } else {
        struct in_addr mask;
        if (inet_pton(AF_INET, slash + 1, &mask) != 1)
            return false;
        uint32_t inverse = ~ntohl(mask.s_addr);
        if ((inverse & (inverse + 1u)) != 0)
            return false;
        *bits = 32;
        for (; inverse != 0; inverse >>= 1)
            (*bits)--;
    }
    return true;
}

static const char* ip_scope(unsigned char scope) {
    switch (scope) {
    case RT_SCOPE_UNIVERSE: return "global";
    case RT_SCOPE_SITE: return "site";
    case RT_SCOPE_LINK: return "link";
    case RT_SCOPE_HOST: return "host";
    case RT_SCOPE_NOWHERE: return "nowhere";
    default: return NULL;
    }
}

static int ip_address_record(const struct nlmsghdr* header, void* context) {
    unsigned int selected = *(const unsigned int*)context;
    struct ifaddrmsg addr;
    const size_t offset = NLMSG_LENGTH(sizeof(addr));
    if (header->nlmsg_len < offset)
        goto invalid;
    const unsigned char* data = (const void*)header;
    memcpy(&addr, data + NLMSG_HDRLEN, sizeof(addr));
    if (addr.ifa_family != AF_INET || (selected && addr.ifa_index != selected))
        return 0;
    if (!addr.ifa_index || addr.ifa_prefixlen > 32)
        goto invalid;
    struct in_addr local, peer;
    bool has_local = false, has_peer = false;
    const unsigned char* cursor = data + offset;
    size_t left = header->nlmsg_len - offset;
    while (left) {
        struct rtattr attribute;
        if (left < sizeof(attribute))
            goto invalid;
        memcpy(&attribute, cursor, sizeof(attribute));
        size_t size = RTA_ALIGN(attribute.rta_len);
        if (attribute.rta_len < RTA_LENGTH(0) || size > left)
            goto invalid;
        if (attribute.rta_type == IFA_LOCAL || attribute.rta_type == IFA_ADDRESS) {
            bool is_local = attribute.rta_type == IFA_LOCAL;
            bool* seen = is_local ? &has_local : &has_peer;
            if (*seen || attribute.rta_len != RTA_LENGTH(sizeof(local)))
                goto invalid;
            *seen = true;
            memcpy(is_local ? &local : &peer, cursor + RTA_LENGTH(0), sizeof(local));
        }
        cursor += size;
        left -= size;
    }
    if (!has_local && !has_peer)
        goto invalid;
    char name[IF_NAMESIZE];
    if (!if_indextoname(addr.ifa_index, name))
        return errno == ENODEV || errno == ENXIO ? 0 : -1;
    char host[INET_ADDRSTRLEN], other[INET_ADDRSTRLEN];
    if (!inet_ntop(AF_INET, has_local ? &local : &peer, host, sizeof(host)))
        return -1;
    bool point_to_point = has_local && has_peer && local.s_addr != peer.s_addr;
    if (point_to_point && !inet_ntop(AF_INET, &peer, other, sizeof(other)))
        return -1;
    char numeric_scope[4];
    const char* scope = ip_scope(addr.ifa_scope);
    if (!scope) {
        snprintf(numeric_scope, sizeof(numeric_scope), "%u", (unsigned int)addr.ifa_scope);
        scope = numeric_scope;
    }
    return printf("%u: %s inet %s%s%s/%u scope %s\n", addr.ifa_index, name, host,
                  point_to_point ? " peer " : "", point_to_point ? other : "",
                  (unsigned int)addr.ifa_prefixlen, scope) < 0 ? -1 : 0;

invalid:
    errno = EPROTO;
    return -1;
}

static int ip_show_addresses(int argc, char** argv, struct bx_diag_ctx* diag) {
    if (argc && (strcmp(argv[0], "show") == 0 || strcmp(argv[0], "list") == 0 ||
                 strcmp(argv[0], "lst") == 0)) {
        argc--;
        argv++;
    }
    unsigned int index = 0;
    if (argc) {
        if (argc != 2 || strcmp(argv[0], "dev") != 0)
            return ip_usage(diag);
        index = ip_ifindex(argv[1], diag);
        if (!index)
            return 1;
    }
    struct {
        struct nlmsghdr header;
        struct ifaddrmsg address;
    } request = {
        .header = {.nlmsg_len = NLMSG_LENGTH(sizeof(request.address)), .nlmsg_type = RTM_GETADDR},
        .address = {.ifa_family = AF_INET},
    };
    if (bx_rtnl_dump(&request, RTM_NEWADDR, ip_address_record, &index) < 0) {
        bx_perror_path(diag, ferror(stdout) ? "standard output" : "address dump");
        return 1;
    }
    if (fflush(stdout) == EOF) {
        bx_perror_path(diag, "standard output");
        return 1;
    }
    return 0;
}

static int ip_address(int argc, char** argv, struct bx_diag_ctx* diag) {
    enum ip_action action;
    if (!argc || !ip_action(argv[0], &action))
        return ip_show_addresses(argc, argv, diag);
    if (argc != 4 || strcmp(argv[2], "dev") != 0)
        return ip_usage(diag);
    struct in_addr local;
    unsigned char prefix;
    if (!ip_parse_prefix(argv[1], &local, &prefix)) {
        bx_diag(diag, "invalid IPv4 address or prefix: '%s'", argv[1]);
        return 1;
    }
    unsigned int index = ip_ifindex(argv[3], diag);
    if (index == 0)
        return 1;
    struct {
        struct nlmsghdr header;
        struct ifaddrmsg address;
        unsigned char attrs[2 * RTA_SPACE(sizeof(local))];
    } request = {
        .header = {.nlmsg_len = NLMSG_LENGTH(sizeof(request.address)),
                   .nlmsg_type = action == IP_REPLACE ? RTM_NEWADDR : RTM_DELADDR,
                   .nlmsg_flags = action == IP_REPLACE ? NLM_F_CREATE | NLM_F_REPLACE : 0},
        .address = {.ifa_family = AF_INET, .ifa_prefixlen = prefix, .ifa_index = index,
                    .ifa_scope = action == IP_REPLACE && (ntohl(local.s_addr) >> 24) == 127 ?
                        RT_SCOPE_HOST : RT_SCOPE_UNIVERSE},
    };
    if (!bx_rtnl_attr(&request, sizeof(request), IFA_LOCAL, &local, sizeof(local)) ||
        !bx_rtnl_attr(&request, sizeof(request), IFA_ADDRESS, &local, sizeof(local))) {
        bx_perror_path(diag, "address request");
        return 1;
    }
    return ip_apply(&request, action == IP_REPLACE ? "address replace" : "address del", argv[3], diag);
}

static int ip_route(int argc, char** argv, struct bx_diag_ctx* diag) {
    enum ip_action action;
    if ((argc != 6 && argc != 8) || !ip_action(argv[0], &action) ||
        strcmp(argv[1], "default") != 0 || strcmp(argv[2], "via") != 0 ||
        strcmp(argv[4], "dev") != 0 || (argc == 8 && strcmp(argv[6], "metric") != 0))
        return ip_usage(diag);
    struct in_addr gateway;
    if (inet_pton(AF_INET, argv[3], &gateway) != 1) {
        bx_diag(diag, "invalid IPv4 gateway: '%s'", argv[3]);
        return 1;
    }
    uintmax_t number = 0;
    if (argc == 8 && (!bx_size_parse_uint(argv[7], &number) || number > UINT32_MAX)) {
        bx_diag(diag, "invalid route metric: '%s'", argv[7]);
        return 1;
    }
    uint32_t metric = (uint32_t)number;
    uint32_t index = ip_ifindex(argv[5], diag);
    if (index == 0)
        return 1;
    struct {
        struct nlmsghdr header;
        struct rtmsg route;
        unsigned char attrs[RTA_SPACE(sizeof(gateway)) + RTA_SPACE(sizeof(index)) + RTA_SPACE(sizeof(metric))];
    } request = {
        .header = {.nlmsg_len = NLMSG_LENGTH(sizeof(request.route)),
                   .nlmsg_type = action == IP_REPLACE ? RTM_NEWROUTE : RTM_DELROUTE,
                   .nlmsg_flags = action == IP_REPLACE ? NLM_F_CREATE | NLM_F_REPLACE : 0},
        .route = {.rtm_family = AF_INET, .rtm_table = RT_TABLE_MAIN,
                  .rtm_protocol = action == IP_REPLACE ? RTPROT_BOOT : RTPROT_UNSPEC,
                  .rtm_scope = action == IP_REPLACE ? RT_SCOPE_UNIVERSE : RT_SCOPE_NOWHERE,
                  .rtm_type = action == IP_REPLACE ? RTN_UNICAST : RTN_UNSPEC},
    };
    if (!bx_rtnl_attr(&request, sizeof(request), RTA_GATEWAY, &gateway, sizeof(gateway)) ||
        !bx_rtnl_attr(&request, sizeof(request), RTA_OIF, &index, sizeof(index)) ||
        (argc == 8 && !bx_rtnl_attr(&request, sizeof(request), RTA_PRIORITY, &metric, sizeof(metric)))) {
        bx_perror_path(diag, "route request");
        return 1;
    }
    return ip_apply(&request, action == IP_REPLACE ? "route replace" : "route del", argv[5], diag);
}
#endif

int bx_ip_main(int argc, char** argv) {
    struct bx_diag_ctx diag = {.progname = "ip"};
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        ip_help(stdout);
        return 0;
    }
#ifdef __linux__
    if (argc > 1 && strcmp(argv[1], "-4") == 0) {
        argc--;
        argv++;
    }
    if (argc >= 2) {
        if (strcmp(argv[1], "link") == 0)
            return ip_link(argc - 2, argv + 2, &diag);
        if (strcmp(argv[1], "a") == 0 || strcmp(argv[1], "addr") == 0 || strcmp(argv[1], "address") == 0)
            return ip_address(argc - 2, argv + 2, &diag);
        if (strcmp(argv[1], "route") == 0)
            return ip_route(argc - 2, argv + 2, &diag);
    }
    return ip_usage(&diag);
#else
    bx_diag(&diag, "rtnetlink is unsupported on this platform");
    return 1;
#endif
}
