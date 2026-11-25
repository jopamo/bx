#define _DEFAULT_SOURCE
#include "applets.h"
#include "bx/diag.h"

static void ip_help(FILE* out) {
    fputs("usage: ip [-4] [-s] {l|link} [show [dev IFACE]]\n"
          "       ip link set [dev] IFACE {up|down}\n"
          "       ip [-4] {a|addr|address} [show [dev IFACE]]\n"
          "       ip [-4] {addr|address} {add|replace|del} IPV4/PREFIX dev IFACE\n"
          "       ip [-4] {r|route} [show [dev IFACE]]\n"
          "       ip [-4] route {add|replace|del} [default|PREFIX] [via IPV4] [dev IFACE] [metric NUMBER]\n"
          "PREFIX is a CIDR length or a contiguous dotted netmask. -s prints link counters. Linux only.\n", out);
}

#ifdef __linux__
#include <arpa/inet.h>
#include <limits.h>
#include <linux/if_link.h>
#include <net/if.h>
#include <stdint.h>

#include "lib/size_parse.h"
#include "rtnetlink.h"

enum ip_action { IP_ADD, IP_REPLACE, IP_DELETE };

static bool ip_action(const char* word, enum ip_action* action) {
    if (strcmp(word, "add") == 0)
        *action = IP_ADD;
    else if (strcmp(word, "replace") == 0)
        *action = IP_REPLACE;
    else if (strcmp(word, "del") == 0 || strcmp(word, "delete") == 0)
        *action = IP_DELETE;
    else
        return false;
    return true;
}

static unsigned short ip_action_flags(enum ip_action action) {
    if (action == IP_ADD)
        return NLM_F_CREATE;
    if (action == IP_REPLACE)
        return NLM_F_CREATE | NLM_F_REPLACE;
    return 0;
}

static const char* ip_action_name(enum ip_action action) {
    return action == IP_ADD ? "add" : action == IP_REPLACE ? "replace" : "del";
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

static int ip_set_link(int argc, char** argv, struct bx_diag_ctx* diag) {
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

struct ip_show_context {
    unsigned int index;
    bool stats;
};

static int ip_address_record(const struct nlmsghdr* header, void* context) {
    unsigned int selected = ((const struct ip_show_context*)context)->index;
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
        const unsigned char* value;
        if (!bx_rtnl_next_attr(&cursor, &left, &attribute, &value))
            return -1;
        if (attribute.rta_type == IFA_LOCAL || attribute.rta_type == IFA_ADDRESS) {
            bool is_local = attribute.rta_type == IFA_LOCAL;
            bool* seen = is_local ? &has_local : &has_peer;
            if (*seen || attribute.rta_len != RTA_LENGTH(sizeof(local)))
                goto invalid;
            *seen = true;
            memcpy(is_local ? &local : &peer, value, sizeof(local));
        }
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

static int ip_show_index(int argc, char** argv, unsigned int* index, struct bx_diag_ctx* diag) {
    *index = 0;
    if (argc && (strcmp(argv[0], "show") == 0 || strcmp(argv[0], "list") == 0 ||
                 strcmp(argv[0], "lst") == 0)) {
        argc--;
        argv++;
    }
    if (argc) {
        if (argc != 2 || strcmp(argv[0], "dev") != 0)
            return ip_usage(diag);
        *index = ip_ifindex(argv[1], diag);
        if (!*index)
            return 1;
    }
    return 0;
}

static int ip_show(void* request, unsigned short type, int (*record)(const struct nlmsghdr*, void*),
                   void* context, const char* label, struct bx_diag_ctx* diag) {
    if (bx_rtnl_dump(request, type, record, context) < 0) {
        bx_perror_path(diag, ferror(stdout) ? "standard output" : label);
        return 1;
    }
    if (fflush(stdout) == EOF) {
        bx_perror_path(diag, "standard output");
        return 1;
    }
    return 0;
}

static int ip_link_record(const struct nlmsghdr* header, void* context) {
    const struct ip_show_context* show = context;
    unsigned int selected = show->index;
    struct ifinfomsg link;
    const size_t offset = NLMSG_LENGTH(sizeof(link));
    if (header->nlmsg_len < offset)
        goto invalid;
    const unsigned char* data = (const void*)header;
    memcpy(&link, data + NLMSG_HDRLEN, sizeof(link));
    if (link.ifi_index <= 0)
        goto invalid;
    if (selected && selected != (unsigned int)link.ifi_index)
        return 0;

    const unsigned char* cursor = data + offset;
    size_t left = header->nlmsg_len - offset;
    const char* name = NULL;
    uint32_t mtu = 0;
    bool has_mtu = false;
    int carrier = -1;
    struct rtnl_link_stats64 stats;
    bool has_stats = false;
    while (left) {
        struct rtattr attr;
        const unsigned char* value;
        if (!bx_rtnl_next_attr(&cursor, &left, &attr, &value))
            return -1;
        if (attr.rta_type == IFLA_IFNAME) {
            size_t len = attr.rta_len - RTA_LENGTH(0);
            if (name || len < 2 || len > IF_NAMESIZE || memchr(value, '\0', len) != value + len - 1)
                goto invalid;
            name = (const char*)value;
        } else if (attr.rta_type == IFLA_MTU) {
            if (has_mtu || attr.rta_len != RTA_LENGTH(sizeof(mtu)))
                goto invalid;
            has_mtu = true;
            memcpy(&mtu, value, sizeof(mtu));
        } else if (attr.rta_type == IFLA_CARRIER) {
            if (carrier >= 0 || attr.rta_len != RTA_LENGTH(1) || *value > 1)
                goto invalid;
            carrier = *value;
        } else if (attr.rta_type == IFLA_STATS64) {
            if (has_stats || attr.rta_len < RTA_LENGTH(sizeof(stats)))
                goto invalid;
            memcpy(&stats, value, sizeof(stats));
            has_stats = true;
        }
    }
    if (!name || !has_mtu)
        goto invalid;
    if (printf("%d: %s mtu %u admin %s carrier %s\n", link.ifi_index, name,
               (unsigned int)mtu, link.ifi_flags & IFF_UP ? "up" : "down",
               carrier < 0 ? "unknown" : carrier ? "up" : "down") < 0)
        return -1;
    if (show->stats) {
        if (!has_stats)
            goto invalid;
        if (printf("    RX: bytes %llu packets %llu errors %llu dropped %llu missed %llu mcast %llu\n",
                   (unsigned long long)stats.rx_bytes, (unsigned long long)stats.rx_packets,
                   (unsigned long long)stats.rx_errors, (unsigned long long)stats.rx_dropped,
                   (unsigned long long)stats.rx_missed_errors,
                   (unsigned long long)stats.multicast) < 0 ||
            printf("    TX: bytes %llu packets %llu errors %llu dropped %llu carrier %llu collsns %llu\n",
                   (unsigned long long)stats.tx_bytes, (unsigned long long)stats.tx_packets,
                   (unsigned long long)stats.tx_errors, (unsigned long long)stats.tx_dropped,
                   (unsigned long long)stats.tx_carrier_errors,
                   (unsigned long long)stats.collisions) < 0)
            return -1;
    }
    return 0;
invalid:
    errno = EPROTO;
    return -1;
}

static int ip_link(int argc, char** argv, bool stats, struct bx_diag_ctx* diag) {
    if (argc && strcmp(argv[0], "set") == 0) {
        if (stats)
            return ip_usage(diag);
        return ip_set_link(argc, argv, diag);
    }
    unsigned int index;
    if (ip_show_index(argc, argv, &index, diag))
        return 1;
    struct {
        struct nlmsghdr header;
        struct ifinfomsg link;
    } request = {
        .header = {.nlmsg_len = NLMSG_LENGTH(sizeof(request.link)), .nlmsg_type = RTM_GETLINK},
        .link = {.ifi_family = AF_UNSPEC},
    };
    struct ip_show_context context = {.index = index, .stats = stats};
    return ip_show(&request, RTM_NEWLINK, ip_link_record, &context, "link dump", diag);
}

static int ip_show_addresses(int argc, char** argv, struct bx_diag_ctx* diag) {
    unsigned int index;
    if (ip_show_index(argc, argv, &index, diag))
        return 1;
    struct {
        struct nlmsghdr header;
        struct ifaddrmsg address;
    } request = {
        .header = {.nlmsg_len = NLMSG_LENGTH(sizeof(request.address)), .nlmsg_type = RTM_GETADDR},
        .address = {.ifa_family = AF_INET},
    };
    struct ip_show_context context = {.index = index};
    return ip_show(&request, RTM_NEWADDR, ip_address_record, &context, "address dump", diag);
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
                   .nlmsg_type = action == IP_DELETE ? RTM_DELADDR : RTM_NEWADDR,
                   .nlmsg_flags = ip_action_flags(action)},
        .address = {.ifa_family = AF_INET, .ifa_prefixlen = prefix, .ifa_index = index,
                    .ifa_scope = action != IP_DELETE && (ntohl(local.s_addr) >> 24) == 127 ?
                        RT_SCOPE_HOST : RT_SCOPE_UNIVERSE},
    };
    if (!bx_rtnl_attr(&request, sizeof(request), IFA_LOCAL, &local, sizeof(local)) ||
        !bx_rtnl_attr(&request, sizeof(request), IFA_ADDRESS, &local, sizeof(local))) {
        bx_perror_path(diag, "address request");
        return 1;
    }
    char label[32];
    snprintf(label, sizeof(label), "address %s", ip_action_name(action));
    return ip_apply(&request, label, argv[3], diag);
}

static int ip_route_record(const struct nlmsghdr* header, void* context) {
    unsigned int selected = ((const struct ip_show_context*)context)->index;
    struct rtmsg route;
    const size_t offset = NLMSG_LENGTH(sizeof(route));
    if (header->nlmsg_len < offset)
        goto invalid;
    const unsigned char* data = (const void*)header;
    memcpy(&route, data + NLMSG_HDRLEN, sizeof(route));
    if (route.rtm_family != AF_INET || route.rtm_type != RTN_UNICAST)
        return 0;
    if (route.rtm_dst_len > 32)
        goto invalid;

    struct in_addr destination = {0}, gateway, source;
    uint32_t index = 0, metric, table = route.rtm_table;
    struct route_field { void* value; size_t size; bool seen; };
    struct route_field fields[] = {
        [RTA_DST] = {&destination, sizeof(destination), false},
        [RTA_GATEWAY] = {&gateway, sizeof(gateway), false},
        [RTA_PREFSRC] = {&source, sizeof(source), false},
        [RTA_OIF] = {&index, sizeof(index), false},
        [RTA_PRIORITY] = {&metric, sizeof(metric), false},
        [RTA_TABLE] = {&table, sizeof(table), false},
    };
    bool complex = false;
    const unsigned char* cursor = data + offset;
    size_t left = header->nlmsg_len - offset;
    while (left) {
        struct rtattr attr;
        const unsigned char* value;
        if (!bx_rtnl_next_attr(&cursor, &left, &attr, &value))
            return -1;
        if (attr.rta_type == RTA_MULTIPATH || attr.rta_type == RTA_NH_ID || attr.rta_type == RTA_VIA)
            complex = true;
        if (attr.rta_type >= sizeof(fields) / sizeof(*fields) || !fields[attr.rta_type].value)
            continue;
        struct route_field* field = &fields[attr.rta_type];
        if (field->seen || attr.rta_len != RTA_LENGTH(field->size))
            goto invalid;
        field->seen = true;
        memcpy(field->value, value, field->size);
    }
    if (table != RT_TABLE_MAIN)
        return 0;
    if (complex) {
        errno = EOPNOTSUPP;
        return -1;
    }
    if (selected && selected != index)
        return 0;
    if (route.rtm_dst_len && !fields[RTA_DST].seen)
        goto invalid;

    char dst[INET_ADDRSTRLEN], via[INET_ADDRSTRLEN], src[INET_ADDRSTRLEN], name[IF_NAMESIZE];
    if (!inet_ntop(AF_INET, &destination, dst, sizeof(dst)) ||
        (fields[RTA_GATEWAY].seen && !inet_ntop(AF_INET, &gateway, via, sizeof(via))) ||
        (fields[RTA_PREFSRC].seen && !inet_ntop(AF_INET, &source, src, sizeof(src))))
        return -1;
    if (index && !if_indextoname(index, name))
        return errno == ENODEV || errno == ENXIO ? 0 : -1;

    char prefix[sizeof(dst) + sizeof("/255") - 1], priority[sizeof(" metric 4294967295")];
    const char* scope = ip_scope(route.rtm_scope);
    snprintf(prefix, sizeof(prefix), "%s/%u", dst, (unsigned int)route.rtm_dst_len);
    if (fields[RTA_PRIORITY].seen)
        snprintf(priority, sizeof(priority), " metric %u", (unsigned int)metric);
    return printf("%s%s%s%s%s%s%s%s%s%s\n",
                  route.rtm_dst_len ? prefix : "default",
                  fields[RTA_GATEWAY].seen ? " via " : "", fields[RTA_GATEWAY].seen ? via : "",
                  index ? " dev " : "", index ? name : "",
                  scope && route.rtm_scope != RT_SCOPE_UNIVERSE ? " scope " : "",
                  scope && route.rtm_scope != RT_SCOPE_UNIVERSE ? scope : "",
                  fields[RTA_PREFSRC].seen ? " src " : "", fields[RTA_PREFSRC].seen ? src : "",
                  fields[RTA_PRIORITY].seen ? priority : "") < 0 ? -1 : 0;
invalid:
    errno = EPROTO;
    return -1;
}

static int ip_show_routes(int argc, char** argv, struct bx_diag_ctx* diag) {
    unsigned int index;
    if (ip_show_index(argc, argv, &index, diag))
        return 1;
    struct {
        struct nlmsghdr header;
        struct rtmsg route;
    } request = {
        .header = {.nlmsg_len = NLMSG_LENGTH(sizeof(request.route)), .nlmsg_type = RTM_GETROUTE},
        .route = {.rtm_family = AF_INET},
    };
    struct ip_show_context context = {.index = index};
    return ip_show(&request, RTM_NEWROUTE, ip_route_record, &context, "route dump", diag);
}

static bool ip_parse_route_destination(const char* text, struct in_addr* destination,
                                       unsigned char* bits) {
    if (strcmp(text, "default") == 0) {
        destination->s_addr = 0;
        *bits = 0;
        return true;
    }
    return ip_parse_prefix(text, destination, bits);
}

static int ip_route(int argc, char** argv, struct bx_diag_ctx* diag) {
    enum ip_action action;
    if (!argc || !ip_action(argv[0], &action))
        return ip_show_routes(argc, argv, diag);
    if (argc < 2)
        return ip_usage(diag);

    struct in_addr destination;
    unsigned char destination_bits;
    if (!ip_parse_route_destination(argv[1], &destination, &destination_bits)) {
        bx_diag(diag, "invalid IPv4 route destination: '%s'", argv[1]);
        return 1;
    }

    struct in_addr gateway = {0};
    uint32_t index = 0;
    uint32_t metric = 0;
    bool has_gateway = false;
    bool has_index = false;
    bool has_metric = false;

    for (int i = 2; i < argc; i += 2) {
        if (i + 1 >= argc)
            return ip_usage(diag);
        if (strcmp(argv[i], "via") == 0 && !has_gateway) {
            if (inet_pton(AF_INET, argv[i + 1], &gateway) != 1) {
                bx_diag(diag, "invalid IPv4 gateway: '%s'", argv[i + 1]);
                return 1;
            }
            has_gateway = true;
        }
        else if (strcmp(argv[i], "dev") == 0 && !has_index) {
            index = ip_ifindex(argv[i + 1], diag);
            if (index == 0)
                return 1;
            has_index = true;
        }
        else if (strcmp(argv[i], "metric") == 0 && !has_metric) {
            uintmax_t number = 0;
            if (!bx_size_parse_uint(argv[i + 1], &number) || number > UINT32_MAX) {
                bx_diag(diag, "invalid route metric: '%s'", argv[i + 1]);
                return 1;
            }
            metric = (uint32_t)number;
            has_metric = true;
        }
        else {
            return ip_usage(diag);
        }
    }

    struct {
        struct nlmsghdr header;
        struct rtmsg route;
        unsigned char attrs[RTA_SPACE(sizeof(destination)) + RTA_SPACE(sizeof(gateway)) +
                            RTA_SPACE(sizeof(index)) + RTA_SPACE(sizeof(metric))];
    } request = {
        .header = {.nlmsg_len = NLMSG_LENGTH(sizeof(request.route)),
                   .nlmsg_type = action == IP_DELETE ? RTM_DELROUTE : RTM_NEWROUTE,
                   .nlmsg_flags = ip_action_flags(action)},
        .route = {.rtm_family = AF_INET, .rtm_table = RT_TABLE_MAIN,
                  .rtm_dst_len = destination_bits,
                  .rtm_protocol = action == IP_DELETE ? RTPROT_UNSPEC : RTPROT_BOOT,
                  .rtm_scope = action == IP_DELETE ? RT_SCOPE_NOWHERE :
                      (has_gateway ? RT_SCOPE_UNIVERSE : RT_SCOPE_LINK),
                  .rtm_type = action == IP_DELETE ? RTN_UNSPEC : RTN_UNICAST},
    };
    if ((destination_bits > 0 && !bx_rtnl_attr(&request, sizeof(request), RTA_DST,
                                               &destination, sizeof(destination))) ||
        (has_gateway && !bx_rtnl_attr(&request, sizeof(request), RTA_GATEWAY,
                                      &gateway, sizeof(gateway))) ||
        (has_index && !bx_rtnl_attr(&request, sizeof(request), RTA_OIF,
                                    &index, sizeof(index))) ||
        (has_metric && !bx_rtnl_attr(&request, sizeof(request), RTA_PRIORITY,
                                     &metric, sizeof(metric)))) {
        bx_perror_path(diag, "route request");
        return 1;
    }
    char label[32];
    snprintf(label, sizeof(label), "route %s", ip_action_name(action));
    return ip_apply(&request, label, argv[1], diag);
}

#endif

int bx_ip_main(int argc, char** argv) {
    struct bx_diag_ctx diag = {.progname = "ip"};
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        ip_help(stdout);
        return 0;
    }
#ifdef __linux__
    bool stats = false;
    while (argc > 1 && argv[1][0] == '-') {
        if (strcmp(argv[1], "-4") == 0) {
            argc--;
            argv++;
            continue;
        }
        if (strcmp(argv[1], "-s") == 0) {
            stats = true;
            argc--;
            argv++;
            continue;
        }
        break;
    }
    if (argc >= 2) {
        if (strcmp(argv[1], "l") == 0 || strcmp(argv[1], "link") == 0)
            return ip_link(argc - 2, argv + 2, stats, &diag);
        if (stats)
            return ip_usage(&diag);
        if (strcmp(argv[1], "a") == 0 || strcmp(argv[1], "addr") == 0 || strcmp(argv[1], "address") == 0)
            return ip_address(argc - 2, argv + 2, &diag);
        if (strcmp(argv[1], "r") == 0 || strcmp(argv[1], "route") == 0)
            return ip_route(argc - 2, argv + 2, &diag);
    }
    return ip_usage(&diag);
#else
    bx_diag(&diag, "rtnetlink is unsupported on this platform");
    return 1;
#endif
}
