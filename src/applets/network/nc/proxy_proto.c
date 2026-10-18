#include "netcat.h"
#include "proxy_proto.h"
#include "lib/poll_deadline.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

struct proxy_info {
    struct sockaddr_storage src;
    struct sockaddr_storage dst;
    int family;
};

struct proxy_hdr_v2 {
    uint8_t sig[12];
    uint8_t ver_cmd;
    uint8_t fam_proto;
    uint16_t len;
    union {
        struct {
            uint32_t src_addr;
            uint32_t dst_addr;
            uint16_t src_port;
            uint16_t dst_port;
        } ipv4_addr;
        struct {
            uint8_t src_addr[16];
            uint8_t dst_addr[16];
            uint16_t src_port;
            uint16_t dst_port;
        } ipv6_addr;
    } addr;
} __attribute__((packed));

static const uint8_t sig_v2[12] = {0x0d, 0x0a, 0x0d, 0x0a, 0x00, 0x0d, 0x0a, 0x51, 0x55, 0x49, 0x54, 0x0a};

static int parse_proxy_port(const char* text, uint16_t* port_out) {
    if (text == NULL || text[0] == '\0')
        return -1;
    for (const char* p = text; *p; p++) {
        if (*p < '0' || *p > '9')
            return -1;
    }

    errno = 0;
    char* end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if (errno == ERANGE || end == text || end == NULL || *end != '\0' || value > 65535ul)
        return -1;

    *port_out = (uint16_t)value;
    return 0;
}

static int parse_proxy_v1(const char* line, struct proxy_info* info) {
    char marker[6], protocol[16], src_addr[INET6_ADDRSTRLEN], dst_addr[INET6_ADDRSTRLEN];
    char src_port_text[16], dst_port_text[16], extra;
    uint16_t src_port, dst_port;

    size_t len = strlen(line);
    if (len < 2 || line[len - 2] != '\r' || line[len - 1] != '\n')
        return -1;
    for (size_t i = 0; i < len - 2; i++) {
        if ((unsigned char)line[i] < 0x20 || (unsigned char)line[i] > 0x7e)
            return -1;
    }

    if (sscanf(line,
               "%5s %15s %45s %45s %15s %15s %c",
               marker,
               protocol,
               src_addr,
               dst_addr,
               src_port_text,
               dst_port_text,
               &extra)
            == 6
        && strcmp(marker, "PROXY") == 0
        && parse_proxy_port(src_port_text, &src_port) == 0
        && parse_proxy_port(dst_port_text, &dst_port) == 0) {
        if (strcmp(protocol, "TCP4") == 0) {
            struct sockaddr_in* sin_src = (struct sockaddr_in*)&info->src;
            struct sockaddr_in* sin_dst = (struct sockaddr_in*)&info->dst;
            info->family = AF_INET;
            sin_src->sin_family = AF_INET;
            sin_dst->sin_family = AF_INET;
            if (inet_pton(AF_INET, src_addr, &sin_src->sin_addr) != 1)
                return -1;
            if (inet_pton(AF_INET, dst_addr, &sin_dst->sin_addr) != 1)
                return -1;
            sin_src->sin_port = htons(src_port);
            sin_dst->sin_port = htons(dst_port);
            return 0;
        }
        else if (strcmp(protocol, "TCP6") == 0) {
            struct sockaddr_in6* sin6_src = (struct sockaddr_in6*)&info->src;
            struct sockaddr_in6* sin6_dst = (struct sockaddr_in6*)&info->dst;
            info->family = AF_INET6;
            sin6_src->sin6_family = AF_INET6;
            sin6_dst->sin6_family = AF_INET6;
            if (inet_pton(AF_INET6, src_addr, &sin6_src->sin6_addr) != 1)
                return -1;
            if (inet_pton(AF_INET6, dst_addr, &sin6_dst->sin6_addr) != 1)
                return -1;
            sin6_src->sin6_port = htons(src_port);
            sin6_dst->sin6_port = htons(dst_port);
            return 0;
        }
    }

    if (strncmp(line, "PROXY UNKNOWN", 13) == 0 && (line[13] == ' ' || line[13] == '\r')) {
        info->family = AF_UNSPEC;
        return 0;
    }

    return -1;
}

static int parse_proxy_v2(const uint8_t* buf, size_t len, struct proxy_info* info) {
    const struct proxy_hdr_v2* hdr = (const struct proxy_hdr_v2*)buf;
    size_t addr_len;

    if (len < 16)
        return -1;

    if (memcmp(hdr->sig, sig_v2, 12) != 0 || (hdr->ver_cmd & 0xf0) != 0x20)
        return -1;

    uint16_t h_len = ntohs(hdr->len);
    if (len != (size_t)16 + h_len)
        return -1;

    if ((hdr->ver_cmd & 0x0f) == 0) {
        info->family = AF_UNSPEC;
        return 0;
    }
    if ((hdr->ver_cmd & 0x0f) != 1)
        return -1;

    if (hdr->fam_proto == 0)
        addr_len = 0;
    else if (hdr->fam_proto == 0x11)
        addr_len = 12;
    else if (hdr->fam_proto == 0x21)
        addr_len = 36;
    else
        return -1;
    if (h_len < addr_len)
        return -1;
    for (size_t i = 16 + addr_len; i < len;) {
        if (len - i < 3)
            return -1;
        size_t value_len = ((size_t)buf[i + 1] << 8) | buf[i + 2];
        i += 3;
        if (value_len > len - i)
            return -1;
        i += value_len;
    }

    if (hdr->fam_proto == 0) {
        info->family = AF_UNSPEC;
        return 0;
    }
    else if (hdr->fam_proto == 0x11) {
        struct sockaddr_in* sin_src = (struct sockaddr_in*)&info->src;
        struct sockaddr_in* sin_dst = (struct sockaddr_in*)&info->dst;
        info->family = AF_INET;
        sin_src->sin_family = AF_INET;
        sin_dst->sin_family = AF_INET;
        sin_src->sin_addr.s_addr = hdr->addr.ipv4_addr.src_addr;
        sin_dst->sin_addr.s_addr = hdr->addr.ipv4_addr.dst_addr;
        sin_src->sin_port = hdr->addr.ipv4_addr.src_port;
        sin_dst->sin_port = hdr->addr.ipv4_addr.dst_port;
        return 0;
    }
    else if (hdr->fam_proto == 0x21) {
        struct sockaddr_in6* sin6_src = (struct sockaddr_in6*)&info->src;
        struct sockaddr_in6* sin6_dst = (struct sockaddr_in6*)&info->dst;
        info->family = AF_INET6;
        sin6_src->sin6_family = AF_INET6;
        sin6_dst->sin6_family = AF_INET6;
        memcpy(&sin6_src->sin6_addr, hdr->addr.ipv6_addr.src_addr, 16);
        memcpy(&sin6_dst->sin6_addr, hdr->addr.ipv6_addr.dst_addr, 16);
        sin6_src->sin6_port = hdr->addr.ipv6_addr.src_port;
        sin6_dst->sin6_port = hdr->addr.ipv6_addr.dst_port;
        return 0;
    }
    return -1;
}

static int serialize_proxy_v2(uint8_t* buf, size_t len, const struct proxy_info* info) {
    struct proxy_hdr_v2* hdr = (struct proxy_hdr_v2*)buf;
    if (len < 16) {
        errno = EMSGSIZE;
        return -1;
    }

    memset(hdr, 0, 16);
    memcpy(hdr->sig, sig_v2, 12);
    hdr->ver_cmd = 0x21; /* v2, PROXY */

    if (info->family == AF_INET) {
        const struct sockaddr_in* src4 = (const struct sockaddr_in*)&info->src;
        const struct sockaddr_in* dst4 = (const struct sockaddr_in*)&info->dst;

        if (len < 16 + 12) {
            errno = EMSGSIZE;
            return -1;
        }
        hdr->fam_proto = 0x11;
        hdr->len = htons(12);
        hdr->addr.ipv4_addr.src_addr = src4->sin_addr.s_addr;
        hdr->addr.ipv4_addr.dst_addr = dst4->sin_addr.s_addr;
        hdr->addr.ipv4_addr.src_port = src4->sin_port;
        hdr->addr.ipv4_addr.dst_port = dst4->sin_port;
        return 16 + 12;
    }
    else if (info->family == AF_INET6) {
        const struct sockaddr_in6* src6 = (const struct sockaddr_in6*)&info->src;
        const struct sockaddr_in6* dst6 = (const struct sockaddr_in6*)&info->dst;

        if (len < 16 + 36) {
            errno = EMSGSIZE;
            return -1;
        }
        hdr->fam_proto = 0x21;
        hdr->len = htons(36);
        memcpy(hdr->addr.ipv6_addr.src_addr, &src6->sin6_addr, 16);
        memcpy(hdr->addr.ipv6_addr.dst_addr, &dst6->sin6_addr, 16);
        hdr->addr.ipv6_addr.src_port = src6->sin6_port;
        hdr->addr.ipv6_addr.dst_port = dst6->sin6_port;
        return 16 + 36;
    }
    else {
        errno = EAFNOSUPPORT;
        return -1;
    }
}

static void report_proxy(const struct proxy_info* info, int version) {
    if (vflag) {
        char src[INET6_ADDRSTRLEN], dst[INET6_ADDRSTRLEN];
        uint16_t src_p = 0, dst_p = 0;

        if (info->family == AF_INET) {
            const struct sockaddr_in* src4 = (const struct sockaddr_in*)&info->src;
            const struct sockaddr_in* dst4 = (const struct sockaddr_in*)&info->dst;

            inet_ntop(AF_INET, &src4->sin_addr, src, sizeof(src));
            inet_ntop(AF_INET, &dst4->sin_addr, dst, sizeof(dst));
            src_p = ntohs(src4->sin_port);
            dst_p = ntohs(dst4->sin_port);
            fprintf(stderr, "PROXY v%d: %s:%u -> %s:%u\n", version, src, src_p, dst, dst_p);
        }
        else if (info->family == AF_INET6) {
            const struct sockaddr_in6* src6 = (const struct sockaddr_in6*)&info->src;
            const struct sockaddr_in6* dst6 = (const struct sockaddr_in6*)&info->dst;

            inet_ntop(AF_INET6, &src6->sin6_addr, src, sizeof(src));
            inet_ntop(AF_INET6, &dst6->sin6_addr, dst, sizeof(dst));
            src_p = ntohs(src6->sin6_port);
            dst_p = ntohs(dst6->sin6_port);
            fprintf(stderr, "PROXY v%d: [%s]:%u -> [%s]:%u\n", version, src, src_p, dst, dst_p);
        }
        else {
            fprintf(stderr, "PROXY v%d: UNKNOWN\n", version);
        }
    }
}

static int get_proxy_info_from_fd(int fd, struct proxy_info* info) {
    socklen_t len;
    int type;

    memset(info, 0, sizeof(*info));
    len = sizeof(info->src);
    if (getsockname(fd, (struct sockaddr*)&info->src, &len) == -1)
        return -1;
    len = sizeof(info->dst);
    if (getpeername(fd, (struct sockaddr*)&info->dst, &len) == -1)
        return -1;
    len = sizeof(type);
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &len) == -1)
        return -1;
    if (type != SOCK_STREAM) {
        errno = EPROTOTYPE;
        return -1;
    }

    info->family = info->src.ss_family;
    if ((info->family != AF_INET && info->family != AF_INET6) || info->dst.ss_family != info->family) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    return 0;
}

static int proxy_io_exact(int fd, void* data, size_t len, struct bx_poll_deadline* deadline, int sending) {
    uint8_t* bytes = data;
    size_t pos = 0;

    while (pos < len) {
        struct pollfd pfd = {.fd = fd, .events = sending ? POLLOUT : POLLIN};
        int ready = bx_poll_deadline_wait(deadline, &pfd, 1);
        if (ready <= 0) {
            if (ready == 0)
                errno = ETIMEDOUT;
            return -1;
        }
        ssize_t n = sending ? send(fd, bytes + pos, len - pos, MSG_DONTWAIT | MSG_NOSIGNAL) : recv(fd, bytes + pos, len - pos, MSG_DONTWAIT);
        if (n > 0)
            pos += (size_t)n;
        else if (n == 0) {
            errno = sending ? EPIPE : EPROTO;
            return -1;
        }
        else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
            return -1;
    }
    return 0;
}

int send_proxy_v2(int fd) {
    struct proxy_info info;
    struct bx_poll_deadline deadline;
    uint8_t buf[sizeof(struct proxy_hdr_v2)];
    int n;

    if (get_proxy_info_from_fd(fd, &info) == -1)
        return -1;

    n = serialize_proxy_v2(buf, sizeof(buf), &info);
    if (n == -1)
        return -1;
    bx_poll_deadline_init(&deadline, timeout);
    return proxy_io_exact(fd, buf, (size_t)n, &deadline, 1);
}

int recv_proxy(int fd) {
    struct proxy_info info;
    struct bx_poll_deadline deadline;
    uint8_t buf[256];

    memset(&info, 0, sizeof(info));
    bx_poll_deadline_init(&deadline, timeout);

    if (proxy_io_exact(fd, buf, 5, &deadline, 0) == -1)
        return -1;

    if (memcmp(buf, "PROXY", 5) == 0) {
        char line[128];
        size_t i = 5;
        memcpy(line, "PROXY", 5);
        while (i < sizeof(line) - 1) {
            if (proxy_io_exact(fd, &line[i], 1, &deadline, 0) == -1)
                return -1;
            if (line[i] == '\0')
                goto invalid;
            if (line[i] == '\n') {
                line[++i] = '\0';
                break;
            }
            i++;
        }
        if (i == sizeof(line) - 1)
            goto invalid;
        if (parse_proxy_v1(line, &info) == 0)
            report_proxy(&info, 1);
        else
            goto invalid;
    }
    else if (memcmp(buf, sig_v2, 5) == 0) {
        if (proxy_io_exact(fd, buf + 5, 11, &deadline, 0) == -1)
            return -1;

        uint16_t len = ntohs(((struct proxy_hdr_v2*)buf)->len);
        if (len > sizeof(buf) - 16)
            goto invalid;

        if (proxy_io_exact(fd, buf + 16, len, &deadline, 0) == -1)
            return -1;

        if (parse_proxy_v2(buf, 16 + len, &info) == 0)
            report_proxy(&info, 2);
        else
            goto invalid;
    }
    else
        goto invalid;
    return 0;

invalid:
    errno = EPROTO;
    return -1;
}
