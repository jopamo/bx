#include "netcat.h"
#include <stddef.h>
#include "syscalls.h"

#ifdef __linux__
#include <fcntl.h>
#include <linux/genetlink.h>
#include <linux/vm_sockets.h>
#include <linux/mptcp.h>
#include "mptcp_pm.h"
#endif

#include "lib/fd_ops.h"
#include "lib/sockaddr_format.h"

#ifndef IPTOS_DSCP_CS0
#define IPTOS_DSCP_CS0 0x00
#define IPTOS_DSCP_CS1 0x20
#define IPTOS_DSCP_CS2 0x40
#define IPTOS_DSCP_CS3 0x60
#define IPTOS_DSCP_CS4 0x80
#define IPTOS_DSCP_CS5 0xa0
#define IPTOS_DSCP_CS6 0xc0
#define IPTOS_DSCP_CS7 0xe0
#endif

#ifndef IPTOS_DSCP_VA
#define IPTOS_DSCP_VA 0xB0
#endif

#ifndef AF_VSOCK
#define AF_VSOCK 40
#endif

#ifndef VMADDR_CID_ANY
#define VMADDR_CID_ANY -1U
#endif

#ifndef VMADDR_CID_LOCAL
#define VMADDR_CID_LOCAL 1
#endif

#ifndef VMADDR_PORT_ANY
#define VMADDR_PORT_ANY -1U
#endif

#ifndef __linux__
#ifndef HAVE_SOCKADDR_VM
struct sockaddr_vm {
    unsigned short svm_family;
    unsigned short svm_reserved1;
    unsigned int svm_port;
    unsigned int svm_cid;
    unsigned char svm_zero[sizeof(struct sockaddr) - sizeof(unsigned short) - sizeof(unsigned short) -
                           sizeof(unsigned int) - sizeof(unsigned int)];
};
#endif
#endif

int vsock_listen(const char* cid_str, const char* port_str) {
    struct sockaddr_vm svm;
    int s;
    const char* errstr;

    memset(&svm, 0, sizeof(svm));
    svm.svm_family = AF_VSOCK;

    if (cid_str == NULL || strcmp(cid_str, "any") == 0)
        svm.svm_cid = VMADDR_CID_ANY;
    else
        svm.svm_cid = (unsigned int)nc_strtonum(cid_str, 0, UINT_MAX, &errstr);

    if (port_str == NULL)
        svm.svm_port = VMADDR_PORT_ANY;
    else
        svm.svm_port = (unsigned int)nc_strtonum(port_str, 0, UINT_MAX, &errstr);

    if ((s = bx_fd_socket_cloexec(AF_VSOCK, SOCK_STREAM, 0)) == -1)
        return -1;

    set_common_sockopts(s, AF_VSOCK);

    if (direct_bind(s, (struct sockaddr*)&svm, sizeof(svm)) == -1) {
        close(s);
        return -1;
    }

    if (direct_listen(s, 5) == -1) {
        close(s);
        return -1;
    }

    if (vflag) {
        char buf[64];
        snprintf(buf, sizeof(buf), "vsock:%u:%u", svm.svm_cid, svm.svm_port);
        report_sock("Listening", NULL, 0, buf);
    }

    return s;
}

int vsock_connect(const char* cid_str, const char* port_str) {
    struct sockaddr_vm svm;
    int s;
    const char* errstr;

    memset(&svm, 0, sizeof(svm));
    svm.svm_family = AF_VSOCK;

    if (strcmp(cid_str, "local") == 0)
        svm.svm_cid = VMADDR_CID_LOCAL;
    else
        svm.svm_cid = (unsigned int)nc_strtonum(cid_str, 0, UINT_MAX, &errstr);

    svm.svm_port = (unsigned int)nc_strtonum(port_str, 0, UINT_MAX, &errstr);

    if ((s = bx_fd_socket_cloexec(AF_VSOCK, SOCK_STREAM, 0)) == -1)
        return -1;

    set_common_sockopts(s, AF_VSOCK);

    if (direct_connect(s, (struct sockaddr*)&svm, sizeof(svm)) == -1) {
        close(s);
        return -1;
    }

    return s;
}

/*
 * unix_bind()
 * Returns a unix socket bound to the given path
 */
int unix_bind(char* path, int flags) {
    struct sockaddr_un s_un;
    int s, save_errno;
    socklen_t len;

    /* Create unix domain socket. */
    if ((s = bx_fd_socket_cloexec(AF_UNIX, flags | (uflag ? SOCK_DGRAM : SOCK_STREAM), 0)) == -1)
        return -1;

    memset(&s_un, 0, sizeof(struct sockaddr_un));
    s_un.sun_family = AF_UNIX;

    if (path[0] == '@') {
        s_un.sun_path[0] = '\0';
        if (nc_strlcpy(&s_un.sun_path[1], &path[1], sizeof(s_un.sun_path) - 1) >= sizeof(s_un.sun_path) - 1) {
            close(s);
            errno = ENAMETOOLONG;
            return -1;
        }
        len = offsetof(struct sockaddr_un, sun_path) + strlen(path);
    }
    else {
        if (nc_strlcpy(s_un.sun_path, path, sizeof(s_un.sun_path)) >= sizeof(s_un.sun_path)) {
            close(s);
            errno = ENAMETOOLONG;
            return -1;
        }
        len = sizeof(s_un);
    }

    if (direct_bind(s, (struct sockaddr*)&s_un, len) == -1) {
        save_errno = errno;
        close(s);
        errno = save_errno;
        return -1;
    }
    if (vflag)
        report_sock("Bound", NULL, 0, path);

    return s;
}

/*
 * unix_connect()
 * Returns a socket connected to a local unix socket. Returns -1 on failure.
 */
int unix_connect(char* path) {
    struct sockaddr_un s_un;
    int s, save_errno;
    socklen_t len;

    if (uflag) {
        if ((s = unix_bind(unix_dg_tmp_socket, SOCK_CLOEXEC)) == -1)
            return -1;
    }
    else {
        if ((s = bx_fd_socket_cloexec(AF_UNIX, SOCK_STREAM, 0)) == -1)
            return -1;
    }

    memset(&s_un, 0, sizeof(struct sockaddr_un));
    s_un.sun_family = AF_UNIX;

    if (path[0] == '@') {
        s_un.sun_path[0] = '\0';
        if (nc_strlcpy(&s_un.sun_path[1], &path[1], sizeof(s_un.sun_path) - 1) >= sizeof(s_un.sun_path) - 1) {
            close(s);
            errno = ENAMETOOLONG;
            return -1;
        }
        len = offsetof(struct sockaddr_un, sun_path) + strlen(path);
    }
    else {
        if (nc_strlcpy(s_un.sun_path, path, sizeof(s_un.sun_path)) >= sizeof(s_un.sun_path)) {
            close(s);
            errno = ENAMETOOLONG;
            return -1;
        }
        len = sizeof(s_un);
    }

    if (direct_connect(s, (struct sockaddr*)&s_un, len) == -1) {
        save_errno = errno;
        close(s);
        errno = save_errno;
        return -1;
    }
    return s;
}

/*
 * unix_listen()
 * Create a unix domain socket, and listen on it.
 */
int unix_listen(char* path) {
    int s;

    if ((s = unix_bind(path, 0)) == -1)
        return -1;
    if (direct_listen(s, 5) == -1) {
        close(s);
        return -1;
    }
    if (vflag)
        report_sock("Listening", NULL, 0, path);

    return s;
}

/*
 * remote_connect()
 * Returns a socket connected to a remote host. Properly binds to a local
 * port or source address if needed. Returns -1 on failure.
 */
int remote_connect(const char* host, const char* port, struct addrinfo hints, char* ipaddr) {
    struct addrinfo *res, *res0;
    int s = -1, error, herr, save_errno;
#ifdef SO_BINDANY
    int on = 1;
#endif

    if ((error = getaddrinfo(host, port, &hints, &res0)))
        errx(EXIT_RUNTIME, "getaddrinfo for host \"%s\" port %s: %s", host, port, gai_strerror(error));

    for (res = res0; res; res = res->ai_next) {
        int proto = res->ai_protocol;
#ifdef IPPROTO_MPTCP
        if (mptcpflag && res->ai_protocol == IPPROTO_TCP)
            proto = IPPROTO_MPTCP;
#endif
        if ((s = bx_fd_socket_cloexec(res->ai_family, res->ai_socktype | SOCK_NONBLOCK, proto)) == -1)
            continue;

        /* Bind to a local port or source address if specified. */
        if (sflag || pflag) {
            struct addrinfo ahints, *ares;

            /* try SO_BINDANY when available, but don't insist */
#ifdef SO_BINDANY
            setsockopt(s, SOL_SOCKET, SO_BINDANY, &on, sizeof(on));
#endif
            memset(&ahints, 0, sizeof(struct addrinfo));
            ahints.ai_family = res->ai_family;
            ahints.ai_socktype = uflag ? SOCK_DGRAM : SOCK_STREAM;
            ahints.ai_protocol = uflag ? IPPROTO_UDP : IPPROTO_TCP;
            ahints.ai_flags = AI_PASSIVE;
            if ((error = getaddrinfo(sflag, pflag, &ahints, &ares)))
                close(s);
            errx(EXIT_RUNTIME, "getaddrinfo: %s", gai_strerror(error));

            if (direct_bind(s, (struct sockaddr*)ares->ai_addr, ares->ai_addrlen) == -1) {
                freeaddrinfo(ares);
                close(s);
                err(EXIT_RUNTIME, "bind failed");
            }
            freeaddrinfo(ares);
        }

        set_common_sockopts(s, res->ai_family);

        if (ipaddr != NULL) {
            herr = getnameinfo(res->ai_addr, res->ai_addrlen, ipaddr, NI_MAXHOST, NULL, 0, NI_NUMERICHOST);
            switch (herr) {
                case 0:
                    break;
                case EAI_SYSTEM:
                    close(s);
                    err(EXIT_RUNTIME, "getnameinfo");
                default:
                    close(s);
                    errx(EXIT_RUNTIME, "getnameinfo: %s", gai_strerror(herr));
            }
        }

        if (timeout_connect(s, res->ai_addr, res->ai_addrlen) == 0)
            break;

        if (vflag) {
            /* only print IP if there is something to report */
            if (nflag || ipaddr == NULL || (strncmp(host, ipaddr, NI_MAXHOST) == 0))
                warn("connect to %s port %s (%s) failed", host, port, uflag ? "udp" : "tcp");
            else
                warn("connect to %s (%s) port %s (%s) failed", host, ipaddr, port, uflag ? "udp" : "tcp");
        }

        save_errno = errno;
        close(s);
        errno = save_errno;
        s = -1;
    }

    if (vflag >= 2 && !uflag)
        report_mptcp_info(s);

    freeaddrinfo(res0);

    return s;
}

int timeout_connect(int s, const struct sockaddr* name, socklen_t namelen) {
    socklen_t optlen;
    int optval;
    int ret;

    if ((ret = direct_connect(s, name, namelen)) != 0 && errno == EINPROGRESS) {
        ret = nc_wait_fd_events_monotonic(s, POLLOUT, timeout);
        if (ret == 1) {
            optlen = sizeof(optval);
            if ((ret = getsockopt(s, SOL_SOCKET, SO_ERROR, &optval, &optlen)) == 0) {
                errno = optval;
                ret = optval == 0 ? 0 : -1;
            }
            /* success or getsockopt error, return ret */
        }
        else if (ret == 0) {
            errno = ETIMEDOUT;
            ret = -1;
        }
        else {
            close(s);
            err(EXIT_RUNTIME, "connect wait failed");
        }
    }

    return ret;
}

/*
 * local_listen()
 * Returns a socket listening on a local port, binds to specified source
 * address. Returns -1 on failure.
 */
int local_listen(const char* host, const char* port, struct addrinfo hints) {
    struct addrinfo *res, *res0;
    int s = -1, ret, x = 1, save_errno;
    int error;

    /* Allow nodename to be null. */
    hints.ai_flags |= AI_PASSIVE;

    /*
     * In the case of binding to a wildcard address
     * default to binding to an ipv4 address.
     */
    if (host == NULL && hints.ai_family == AF_UNSPEC)
        hints.ai_family = AF_INET;

    if ((error = getaddrinfo(host, port, &hints, &res0)))
        errx(EXIT_RUNTIME, "getaddrinfo: %s", gai_strerror(error));

    for (res = res0; res; res = res->ai_next) {
        int proto = res->ai_protocol;
#ifdef IPPROTO_MPTCP
        if (mptcpflag && res->ai_protocol == IPPROTO_TCP)
            proto = IPPROTO_MPTCP;
#endif
        if ((s = bx_fd_socket_cloexec(res->ai_family, res->ai_socktype, proto)) == -1)
            continue;

        ret = setsockopt(s, SOL_SOCKET, SO_REUSEPORT, &x, sizeof(x));
        if (ret == -1) {
            freeaddrinfo(res0);
            close(s);
            err(EXIT_RUNTIME, NULL);
        }

        set_common_sockopts(s, res->ai_family);

        if (direct_bind(s, (struct sockaddr*)res->ai_addr, res->ai_addrlen) == 0)
            break;

        save_errno = errno;
        close(s);
        errno = save_errno;
        s = -1;
    }

    if (!uflag && s != -1) {
        if (direct_listen(s, 1) == -1) {
            freeaddrinfo(res0);
            close(s);
            err(EXIT_RUNTIME, "listen");
        }
    }
    if (vflag && s != -1) {
        struct sockaddr_storage ss;
        socklen_t len;

        len = sizeof(ss);
        if (getsockname(s, (struct sockaddr*)&ss, &len) == -1) {
            freeaddrinfo(res0);
            close(s);
            err(EXIT_RUNTIME, "getsockname");
        }
        report_sock(uflag ? "Bound" : "Listening", (struct sockaddr*)&ss, len, NULL);
    }

    if (vflag >= 2 && !uflag)
        report_mptcp_info(s);

    freeaddrinfo(res0);

    return s;
}

/*
 * udptest()
 * Do a few writes to see if the UDP port is there.
 * Fails once PF state table is full.
 */
int udptest(int s) {
    int i, ret;

    /* Only write to the socket in scan mode or interactive mode. */
    if (!zflag && !isatty(STDIN_FILENO))
        return 0;

    for (i = 0; i <= 3; i++) {
        if (direct_write(s, "X", 1) == 1)
            ret = 1;
        else
            ret = -1;
    }
    return ret;
}

void connection_info(int s, const char* host, const char* port, const char* proto, const char* ipaddr) {
    struct servent* sv;
    char* service = "*";

    /* Look up service name unless -n. */
    if (!nflag) {
        const char* errstr;

        int p = nc_strtonum(port, 1, PORT_MAX, &errstr);
        if (errstr)
            errx(EXIT_USAGE, "port number %s: %s", errstr, port);
        sv = getservbyport(htons(p), proto);
        if (sv != NULL)
            service = sv->s_name;
    }

    if (jflag) {
        char src_addr[NI_MAXHOST];
        char src_port[NI_MAXSERV];
        char dst_addr[NI_MAXHOST];
        char dst_port[NI_MAXSERV];
        const char* src_addr_p = NULL;
        const char* src_port_p = NULL;
        const char* dst_addr_p = NULL;
        const char* dst_port_p = NULL;
        const char* tls_state = usetls ? "negotiating" : "disabled";

        if (json_socket_tuple_from_fd(s, src_addr, sizeof(src_addr), src_port, sizeof(src_port), dst_addr,
                                      sizeof(dst_addr), dst_port, sizeof(dst_port)) == 0) {
            src_addr_p = src_addr;
            src_port_p = src_port;
            dst_addr_p = dst_addr;
            dst_port_p = dst_port;
        }

        json_event_begin(stderr, "info", "connection_succeeded", "out", proto, tls_state, "disabled", src_addr_p,
                         src_port_p, dst_addr_p, dst_port_p);
        fprintf(stderr, ",\"host\":\"%s\",\"ip\":\"%s\",\"port\":\"%s\",\"proto\":\"%s\",\"service\":\"%s\"}\n", host,
                ipaddr, port, proto, service);
    }
    else {
        fprintf(stderr, "Connection to %s", host);

        /*
         * if we aren't connecting thru a proxy and
         * there is something to report, print IP
         */
        if (!nflag && !xflag && strcmp(host, ipaddr) != 0)
            fprintf(stderr, " (%s)", ipaddr);

        fprintf(stderr, " %s port [%s/%s] succeeded!\n", port, proto, service);
    }
}

void set_common_sockopts(int s, int af) {
    int x = 1;

    if (Dflag) {
        if (setsockopt(s, SOL_SOCKET, SO_DEBUG, &x, sizeof(x)) == -1) {
            close(s);
            err(EXIT_RUNTIME, NULL);
        }
    }
    if (Tflag != -1) {
        if (af == AF_INET && setsockopt(s, IPPROTO_IP, IP_TOS, &Tflag, sizeof(Tflag)) == -1) {
            close(s);
            err(EXIT_RUNTIME, "set IP ToS");
        }

        else if (af == AF_INET6 && setsockopt(s, IPPROTO_IPV6, IPV6_TCLASS, &Tflag, sizeof(Tflag)) == -1) {
            close(s);
            err(EXIT_RUNTIME, "set IPv6 traffic class");
        }
    }
    if (Iflag) {
        if (setsockopt(s, SOL_SOCKET, SO_RCVBUF, &Iflag, sizeof(Iflag)) == -1) {
            close(s);
            err(EXIT_RUNTIME, "set TCP receive buffer size");
        }
    }
    if (Oflag) {
        if (setsockopt(s, SOL_SOCKET, SO_SNDBUF, &Oflag, sizeof(Oflag)) == -1) {
            close(s);
            err(EXIT_RUNTIME, "set TCP send buffer size");
        }
    }

    if (ttl != -1) {
        if (af == AF_INET && setsockopt(s, IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl))) {
            close(s);
            err(EXIT_RUNTIME, "set IP TTL");
        }

        else if (af == AF_INET6 && setsockopt(s, IPPROTO_IPV6, IPV6_UNICAST_HOPS, &ttl, sizeof(ttl))) {
            close(s);
            err(EXIT_RUNTIME, "set IPv6 unicast hops");
        }
    }

    if (minttl != -1) {
        if (af == AF_INET && setsockopt(s, IPPROTO_IP, IP_MINTTL, &minttl, sizeof(minttl))) {
            close(s);
            err(EXIT_RUNTIME, "set IP min TTL");
        }

        else if (af == AF_INET6 && setsockopt(s, IPPROTO_IPV6, IPV6_MINHOPCOUNT, &minttl, sizeof(minttl))) {
            close(s);
            err(EXIT_RUNTIME, "set IPv6 min hop count");
        }
    }

#ifdef SO_MARK
    if (sockmark != -1) {
        if (setsockopt(s, SOL_SOCKET, SO_MARK, &sockmark, sizeof(sockmark)) == -1) {
            close(s);
            err(EXIT_RUNTIME, "set SO_MARK");
        }
    }
#endif

#ifdef SO_PRIORITY
    if (sockpriority != -1) {
        if (setsockopt(s, SOL_SOCKET, SO_PRIORITY, &sockpriority, sizeof(sockpriority)) == -1) {
            close(s);
            err(EXIT_RUNTIME, "set SO_PRIORITY");
        }
    }
#endif

#ifdef SO_BINDTODEVICE
    if (iface != NULL) {
        if (setsockopt(s, SOL_SOCKET, SO_BINDTODEVICE, iface, strlen(iface)) == -1) {
            close(s);
            err(EXIT_RUNTIME, "set SO_BINDTODEVICE");
        }
    }
#endif

    if (transparent) {
#ifdef IP_TRANSPARENT
        if (af == AF_INET || af == AF_INET6) {
            if (setsockopt(s, SOL_IP, IP_TRANSPARENT, &x, sizeof(x)) == -1) {
                close(s);
                err(EXIT_RUNTIME, "set IP_TRANSPARENT");
            }
        }
#endif
    }

    if (tfoflag) {
#ifdef TCP_FASTOPEN_CONNECT
        if (!lflag) {
            if (setsockopt(s, IPPROTO_TCP, TCP_FASTOPEN_CONNECT, &x, sizeof(x)) == -1) {
                close(s);
                err(EXIT_RUNTIME, "set TCP_FASTOPEN_CONNECT");
            }
        }
#endif
#ifdef TCP_FASTOPEN
        if (lflag) {
            int qlen = 5;
            if (setsockopt(s, IPPROTO_TCP, TCP_FASTOPEN, &qlen, sizeof(qlen)) == -1) {
                close(s);
                err(EXIT_RUNTIME, "set TCP_FASTOPEN");
            }
        }
#endif
    }
}

int process_tos_opt(char* s, int* val) {
    /* DiffServ Codepoints and other TOS mappings */
    const struct toskeywords {
        const char* keyword;
        int val;
    } *t, toskeywords[] = {
              {"af11", IPTOS_DSCP_AF11},
              {"af12", IPTOS_DSCP_AF12},
              {"af13", IPTOS_DSCP_AF13},
              {"af21", IPTOS_DSCP_AF21},
              {"af22", IPTOS_DSCP_AF22},
              {"af23", IPTOS_DSCP_AF23},
              {"af31", IPTOS_DSCP_AF31},
              {"af32", IPTOS_DSCP_AF32},
              {"af33", IPTOS_DSCP_AF33},
              {"af41", IPTOS_DSCP_AF41},
              {"af42", IPTOS_DSCP_AF42},
              {"af43", IPTOS_DSCP_AF43},
              {"critical", IPTOS_PREC_CRITIC_ECP},
              {"cs0", IPTOS_DSCP_CS0},
              {"cs1", IPTOS_DSCP_CS1},
              {"cs2", IPTOS_DSCP_CS2},
              {"cs3", IPTOS_DSCP_CS3},
              {"cs4", IPTOS_DSCP_CS4},
              {"cs5", IPTOS_DSCP_CS5},
              {"cs6", IPTOS_DSCP_CS6},
              {"cs7", IPTOS_DSCP_CS7},
              {"ef", IPTOS_DSCP_EF},
              {"inetcontrol", IPTOS_PREC_INTERNETCONTROL},
              {"lowdelay", IPTOS_LOWDELAY},
              {"netcontrol", IPTOS_PREC_NETCONTROL},
              {"reliability", IPTOS_RELIABILITY},
              {"throughput", IPTOS_THROUGHPUT},
              {"va", IPTOS_DSCP_VA},
              {NULL, -1},
          };

    for (t = toskeywords; t->keyword != NULL; t++) {
        if (strcmp(s, t->keyword) == 0) {
            *val = t->val;
            return 1;
        }
    }

    return 0;
}

#ifdef IPPROTO_MPTCP
struct mptcp_subflow_view {
    unsigned int id;
    unsigned int state;
    char local[NI_MAXHOST + NI_MAXSERV + 8];
    char remote[NI_MAXHOST + NI_MAXSERV + 8];
};

struct mptcp_diag_cache {
    int fd;
    int initialized;
    int fallback;
    char mode[16];
    size_t subflow_count;
    struct mptcp_subflow_view* subflows;
};

static struct mptcp_diag_cache mptcp_cache =
    {.fd = -1, .initialized = 0, .fallback = 0, .mode = "", .subflow_count = 0, .subflows = NULL};

#if defined(__linux__)
struct mptcp_pm_monitor {
    int fd;
    int initialized;
    int unavailable;
    uint16_t family_id;
    uint32_t group_id;
    uint32_t seq;
};

static struct mptcp_pm_monitor mptcp_pm =
    {.fd = -1, .initialized = 0, .unavailable = 0, .family_id = 0, .group_id = 0, .seq = 1};

static void mptcp_pm_close(void) {
    if (mptcp_pm.fd != -1)
        close(mptcp_pm.fd);
    mptcp_pm.fd = -1;
    mptcp_pm.initialized = 0;
    mptcp_pm.family_id = 0;
    mptcp_pm.group_id = 0;
}

static int mptcp_nlmsg_add_attr(struct nlmsghdr* nlh, size_t maxlen, uint16_t type, const void* data, size_t len) {
    size_t old_len;
    size_t attr_len;
    size_t new_len;
    struct nlattr* nla;

    old_len = NLMSG_ALIGN(nlh->nlmsg_len);
    attr_len = NLA_ALIGN(NLA_HDRLEN + len);
    new_len = old_len + attr_len;
    if (new_len > maxlen)
        return -1;

    nla = (struct nlattr*)((char*)nlh + old_len);
    nla->nla_type = type;
    nla->nla_len = NLA_HDRLEN + len;
    memcpy((char*)nla + NLA_HDRLEN, data, len);
    if (attr_len > (size_t)nla->nla_len)
        memset((char*)nla + nla->nla_len, 0, attr_len - nla->nla_len);

    nlh->nlmsg_len = (unsigned int)new_len;
    return 0;
}

static int mptcp_pm_resolve_family(int fd, uint16_t* family_id, uint32_t* group_id) {
    unsigned char req_buf[256];
    unsigned char resp_buf[8192];
    struct nlmsghdr* req_nlh = (struct nlmsghdr*)req_buf;
    struct genlmsghdr* req_genl;
    struct nlmsghdr* nlh;
    struct pollfd pfd;
    struct sockaddr_nl kernel;
    size_t name_len;
    ssize_t nread;
    size_t rem;
    int poll_ret;
    uint32_t seq;

    memset(req_buf, 0, sizeof(req_buf));
    req_nlh->nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
    req_nlh->nlmsg_type = GENL_ID_CTRL;
    req_nlh->nlmsg_flags = NLM_F_REQUEST;
    req_nlh->nlmsg_seq = mptcp_pm.seq++;
    seq = req_nlh->nlmsg_seq;

    req_genl = (struct genlmsghdr*)NLMSG_DATA(req_nlh);
    req_genl->cmd = CTRL_CMD_GETFAMILY;
    req_genl->version = 1;

    name_len = strlen(MPTCP_PM_NAME) + 1;
    if (mptcp_nlmsg_add_attr(req_nlh, sizeof(req_buf), CTRL_ATTR_FAMILY_NAME, MPTCP_PM_NAME, name_len) == -1)
        return -1;

    memset(&kernel, 0, sizeof(kernel));
    kernel.nl_family = AF_NETLINK;
    if (sendto(fd, req_buf, req_nlh->nlmsg_len, 0, (struct sockaddr*)&kernel, sizeof(kernel)) < 0)
        return -1;

    pfd.fd = fd;
    pfd.events = POLLIN;
    poll_ret = poll(&pfd, 1, 250);
    if (poll_ret <= 0)
        return -1;

    for (;;) {
        nread = recv(fd, resp_buf, sizeof(resp_buf), 0);
        if (nread < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }

        rem = (size_t)nread;
        for (nlh = (struct nlmsghdr*)resp_buf; rem >= sizeof(*nlh);) {
            size_t step;

            if (nlh->nlmsg_len < sizeof(*nlh) || nlh->nlmsg_len > rem)
                break;
            if (nlh->nlmsg_seq != seq)
                goto next_resolve_nlmsg;
            if (nlh->nlmsg_type == NLMSG_DONE)
                return -1;
            if (nlh->nlmsg_type == NLMSG_ERROR)
                return -1;
            if (mptcp_pm_parse_ctrl_getfamily(nlh, (size_t)nlh->nlmsg_len, MPTCP_PM_EV_GRP_NAME, family_id, group_id) ==
                0)
                return 0;

        next_resolve_nlmsg:
            step = NLMSG_ALIGN(nlh->nlmsg_len);
            if (step > rem)
                break;
            rem -= step;
            nlh = (struct nlmsghdr*)((char*)nlh + step);
        }

        poll_ret = poll(&pfd, 1, 100);
        if (poll_ret <= 0)
            break;
    }

    return -1;
}

static int mptcp_pm_ensure(void) {
    struct sockaddr_nl addr;
    int fd;

    if (!mptcp_netlink)
        return -1;
    if (mptcp_pm.unavailable)
        return -1;
    if (mptcp_pm.initialized && mptcp_pm.fd != -1)
        return 0;

    fd = bx_fd_socket_cloexec(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);
    if (fd == -1) {
        mptcp_pm.unavailable = 1;
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        close(fd);
        mptcp_pm.unavailable = 1;
        return -1;
    }

    if (mptcp_pm_resolve_family(fd, &mptcp_pm.family_id, &mptcp_pm.group_id) == -1) {
        close(fd);
        mptcp_pm.unavailable = 1;
        return -1;
    }

    if (setsockopt(fd, SOL_NETLINK, NETLINK_ADD_MEMBERSHIP, &mptcp_pm.group_id, sizeof(mptcp_pm.group_id)) == -1) {
        close(fd);
        mptcp_pm.unavailable = 1;
        return -1;
    }

    (void)bx_fd_set_nonblocking(fd, true);

    mptcp_pm.fd = fd;
    mptcp_pm.initialized = 1;
    return 0;
}

static void emit_mptcp_priority_transition(const struct mptcp_pm_event* event) {
    char local[NI_MAXHOST + NI_MAXSERV + 8];
    char remote[NI_MAXHOST + NI_MAXSERV + 8];
    const char* local_s = "unknown";
    const char* remote_s = "unknown";
    const char* role;
    const char* event_name;
    char loc_id[16];
    char rem_id[16];
    char if_idx[16];

    if (event == NULL)
        return;

    if (mptcp_pm_format_endpoints(event, local, sizeof(local), remote, sizeof(remote)) == 0) {
        local_s = local;
        remote_s = remote;
    }

    if (event->has_loc_id)
        snprintf(loc_id, sizeof(loc_id), "%u", (unsigned int)event->loc_id);
    else
        nc_strlcpy(loc_id, "null", sizeof(loc_id));

    if (event->has_rem_id)
        snprintf(rem_id, sizeof(rem_id), "%u", (unsigned int)event->rem_id);
    else
        nc_strlcpy(rem_id, "null", sizeof(rem_id));

    if (event->has_if_idx)
        snprintf(if_idx, sizeof(if_idx), "%u", (unsigned int)event->if_idx);
    else
        nc_strlcpy(if_idx, "null", sizeof(if_idx));

    role = event->has_backup ? (event->backup ? "backup" : "active") : "unknown";
    if (!event->has_backup)
        event_name = "mptcp_path_subflow_priority_changed";
    else if (event->backup)
        event_name = "mptcp_path_subflow_backup";
    else
        event_name = "mptcp_path_subflow_promoted";

    if (jflag) {
        json_event_begin(stderr, "info", event_name, "out", "tcp", "disabled", "disabled", NULL, NULL, NULL, NULL);
        fprintf(stderr,
                ",\"role\":\"%s\",\"loc_id\":%s,\"rem_id\":%s,\"if_index\":%s,\"local\":\"%s\",\"remote\":\"%s\"}\n",
                role, loc_id, rem_id, if_idx, local_s, remote_s);
    }
    else {
        fprintf(stderr, "MPTCP path transition: priority role=%s loc_id=%s rem_id=%s if_index=%s %s <-> %s\n", role,
                loc_id, rem_id, if_idx, local_s, remote_s);
    }
}

static void mptcp_pm_drain_events(uint32_t token) {
    unsigned char buf[8192];
    ssize_t nread;

    if (mptcp_pm_ensure() == -1)
        return;

    for (;;) {
        size_t rem;
        struct nlmsghdr* nlh;

        nread = recv(mptcp_pm.fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (nread < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            if (errno == EINTR)
                continue;
            mptcp_pm_close();
            return;
        }
        if (nread == 0)
            return;

        rem = (size_t)nread;
        for (nlh = (struct nlmsghdr*)buf; rem >= sizeof(*nlh);) {
            struct mptcp_pm_event event;
            size_t step;

            if (nlh->nlmsg_len < sizeof(*nlh) || nlh->nlmsg_len > rem)
                break;
            if (nlh->nlmsg_type == NLMSG_ERROR || nlh->nlmsg_type == NLMSG_DONE)
                goto next_drain_nlmsg;
            if (nlh->nlmsg_type != mptcp_pm.family_id)
                goto next_drain_nlmsg;
            if (mptcp_pm_parse_genl_event(nlh, (size_t)nlh->nlmsg_len, &event) == -1)
                goto next_drain_nlmsg;
            if (!event.has_token || event.token != token)
                goto next_drain_nlmsg;
            if (event.cmd != MPTCP_EVENT_SUB_PRIORITY)
                goto next_drain_nlmsg;

            emit_mptcp_priority_transition(&event);

        next_drain_nlmsg:
            step = NLMSG_ALIGN(nlh->nlmsg_len);
            if (step > rem)
                break;
            rem -= step;
            nlh = (struct nlmsghdr*)((char*)nlh + step);
        }
    }
}
#endif

static void mptcp_cache_reset(void) {
    free(mptcp_cache.subflows);
    mptcp_cache.subflows = NULL;
    mptcp_cache.subflow_count = 0;
    mptcp_cache.initialized = 0;
    mptcp_cache.fallback = 0;
    mptcp_cache.mode[0] = '\0';
}

static const char* mptcp_state_name(unsigned int state) {
    switch (state) {
        case TCP_ESTABLISHED:
            return "established";
        case TCP_SYN_SENT:
            return "syn_sent";
        case TCP_SYN_RECV:
            return "syn_recv";
        case TCP_FIN_WAIT1:
            return "fin_wait1";
        case TCP_FIN_WAIT2:
            return "fin_wait2";
        case TCP_TIME_WAIT:
            return "time_wait";
        case TCP_CLOSE:
            return "close";
        case TCP_CLOSE_WAIT:
            return "close_wait";
        case TCP_LAST_ACK:
            return "last_ack";
        case TCP_LISTEN:
            return "listen";
        case TCP_CLOSING:
            return "closing";
        default:
            return "unknown";
    }
}

static int format_subflow_tuple(const struct sockaddr* sa, socklen_t salen, char* dst, size_t dstlen) {
    char host[NI_MAXHOST];
    char serv[NI_MAXSERV];

    if (bx_sockaddr_format_numeric(
            sa, salen, host, sizeof(host), serv, sizeof(serv)) != 0)
        return -1;

    if (sa->sa_family == AF_INET6)
        snprintf(dst, dstlen, "[%s]:%s", host, serv);
    else
        snprintf(dst, dstlen, "%s:%s", host, serv);

    return 0;
}

static ssize_t find_subflow(const struct mptcp_subflow_view* subflows,
                            size_t subflow_count,
                            const struct mptcp_subflow_view* needle) {
    size_t i;

    for (i = 0; i < subflow_count; i++) {
        if (strcmp(subflows[i].local, needle->local) == 0 && strcmp(subflows[i].remote, needle->remote) == 0)
            return (ssize_t)i;
    }
    return -1;
}

static int collect_mptcp_subflows(int s,
                                  unsigned int fallback_subflows,
                                  struct mptcp_subflow_view** out_subflows,
                                  size_t* out_subflow_count,
                                  const char** mode) {
    struct mptcp_subflow_view* subflows = NULL;
    size_t subflow_count = 0;
    size_t i;

    *out_subflows = NULL;
    *out_subflow_count = 0;
    *mode = "baseline";

#ifdef MPTCP_FULL_INFO
    if (fallback_subflows > 0) {
        struct mptcp_full_info full;
        struct mptcp_subflow_info* sfinfo = NULL;
        struct tcp_info* tcpinfo = NULL;
        size_t cap = fallback_subflows;
        socklen_t full_len;

        if (cap == 0)
            cap = 1;
        sfinfo = calloc(cap, sizeof(*sfinfo));
        tcpinfo = calloc(cap, sizeof(*tcpinfo));
        if (sfinfo != NULL && tcpinfo != NULL) {
            memset(&full, 0, sizeof(full));
            full.size_sfinfo_user = sizeof(*sfinfo);
            full.size_tcpinfo_user = sizeof(*tcpinfo);
            full.size_arrays_user = cap;
            full.subflow_info = (uint64_t)(uintptr_t)sfinfo;
            full.tcp_info = (uint64_t)(uintptr_t)tcpinfo;
            full_len = sizeof(full);

            if (getsockopt(s, IPPROTO_MPTCP, MPTCP_FULL_INFO, &full, &full_len) == 0) {
                subflow_count = full.num_subflows;
                if (subflow_count > cap)
                    subflow_count = cap;
                subflows = calloc(subflow_count, sizeof(*subflows));
                if (subflows == NULL) {
                    free(sfinfo);
                    free(tcpinfo);
                    return -1;
                }

                for (i = 0; i < subflow_count; i++) {
                    subflows[i].id = sfinfo[i].id;
                    subflows[i].state = tcpinfo[i].tcpi_state;
                    if (format_subflow_tuple((struct sockaddr*)&sfinfo[i].addrs.ss_local,
                                             sizeof(sfinfo[i].addrs.ss_local), subflows[i].local,
                                             sizeof(subflows[i].local)) == -1)
                        nc_strlcpy(subflows[i].local, "unknown", sizeof(subflows[i].local));
                    if (format_subflow_tuple((struct sockaddr*)&sfinfo[i].addrs.ss_remote,
                                             sizeof(sfinfo[i].addrs.ss_remote), subflows[i].remote,
                                             sizeof(subflows[i].remote)) == -1)
                        nc_strlcpy(subflows[i].remote, "unknown", sizeof(subflows[i].remote));
                }

                *out_subflows = subflows;
                *out_subflow_count = subflow_count;
                *mode = "rich";
                free(sfinfo);
                free(tcpinfo);
                return 0;
            }
        }

        free(sfinfo);
        free(tcpinfo);
    }
#endif

#ifdef MPTCP_SUBFLOW_ADDRS
    if (fallback_subflows > 0) {
        struct mptcp_subflow_addrs* addrs;
        socklen_t addrlen;

        addrlen = fallback_subflows * sizeof(*addrs);
        addrs = calloc(fallback_subflows, sizeof(*addrs));
        if (addrs == NULL)
            return -1;
        if (getsockopt(s, IPPROTO_MPTCP, MPTCP_SUBFLOW_ADDRS, addrs, &addrlen) == -1) {
            free(addrs);
            return 0;
        }

        subflow_count = addrlen / sizeof(*addrs);
        subflows = calloc(subflow_count, sizeof(*subflows));
        if (subflows == NULL) {
            free(addrs);
            return -1;
        }

        for (i = 0; i < subflow_count; i++) {
            subflows[i].id = i;
            subflows[i].state = 0;
            if (format_subflow_tuple((struct sockaddr*)&addrs[i].ss_local, sizeof(addrs[i].ss_local), subflows[i].local,
                                     sizeof(subflows[i].local)) == -1)
                nc_strlcpy(subflows[i].local, "unknown", sizeof(subflows[i].local));
            if (format_subflow_tuple((struct sockaddr*)&addrs[i].ss_remote, sizeof(addrs[i].ss_remote),
                                     subflows[i].remote, sizeof(subflows[i].remote)) == -1)
                nc_strlcpy(subflows[i].remote, "unknown", sizeof(subflows[i].remote));
        }

        *out_subflows = subflows;
        *out_subflow_count = subflow_count;
        *mode = "baseline";
        free(addrs);
        return 0;
    }
#endif

    return 0;
}

static void emit_mptcp_transition_json(const char* event,
                                       const struct mptcp_subflow_view* sf,
                                       const char* old_state,
                                       const char* new_state) {
    if (sf != NULL && old_state != NULL && new_state != NULL) {
        json_event_begin(stderr, "info", event, "out", "tcp", "disabled", "disabled", NULL, NULL, NULL, NULL);
        fprintf(stderr, ",\"id\":%u,\"local\":\"%s\",\"remote\":\"%s\",\"old_state\":\"%s\",\"new_state\":\"%s\"}\n",
                sf->id, sf->local, sf->remote, old_state, new_state);
    }
    else if (sf != NULL) {
        json_event_begin(stderr, "info", event, "out", "tcp", "disabled", "disabled", NULL, NULL, NULL, NULL);
        fprintf(stderr, ",\"id\":%u,\"local\":\"%s\",\"remote\":\"%s\",\"state\":\"%s\"}\n", sf->id, sf->local,
                sf->remote, mptcp_state_name(sf->state));
    }
    else {
        json_event_begin(stderr, "info", event, "out", "tcp", "disabled", "disabled", NULL, NULL, NULL, NULL);
        fprintf(stderr, "}\n");
    }
}
#endif /* IPPROTO_MPTCP */

void report_mptcp_info(int s) {
#ifdef IPPROTO_MPTCP
    struct mptcp_info info;
    socklen_t len = sizeof(info);
    int ret;
    int fallback;
    int changed = 0;
    size_t i;
    struct mptcp_subflow_view* subflows = NULL;
    size_t subflow_count = 0;
    const char* mode = "baseline";

    /* Check if socket is MPTCP */
    ret = getsockopt(s, IPPROTO_MPTCP, MPTCP_INFO, &info, &len);
    if (ret == -1) {
        if (errno == ENOPROTOOPT) {
            if (mptcp_cache.fd == s)
                mptcp_cache_reset();
            /* Not an MPTCP socket */
            return;
        }
        if (mptcp_cache.fd == s)
            mptcp_cache_reset();
        /* Other error, ignore silently */
        return;
    }

    fallback = (info.mptcpi_flags & MPTCP_INFO_FLAG_FALLBACK) != 0;

    if (mptcp_cache.fd != s) {
        mptcp_cache_reset();
        mptcp_cache.fd = s;
    }

    if (vflag >= 2 && collect_mptcp_subflows(s, info.mptcpi_subflows, &subflows, &subflow_count, &mode) == -1)
        return;

#if defined(__linux__)
    if (vflag >= 2 && mptcp_netlink)
        mptcp_pm_drain_events(info.mptcpi_token);
#endif

    if (!mptcp_cache.initialized) {
        changed = 1;
    }
    else {
        if (mptcp_cache.fallback != fallback)
            changed = 1;
        if (strcmp(mptcp_cache.mode, mode) != 0)
            changed = 1;
        if (mptcp_cache.subflow_count != subflow_count)
            changed = 1;
        if (!changed) {
            for (i = 0; i < subflow_count; i++) {
                ssize_t old_idx = find_subflow(mptcp_cache.subflows, mptcp_cache.subflow_count, &subflows[i]);
                if (old_idx < 0 || mptcp_cache.subflows[old_idx].state != subflows[i].state) {
                    changed = 1;
                    break;
                }
            }
        }
    }

    if (mptcp_cache.initialized && vflag >= 2) {
        if (mptcp_cache.fallback != fallback) {
            if (jflag) {
                json_event_begin(stderr, "info", "mptcp_fallback_changed", "out", "tcp", "disabled", "disabled", NULL,
                                 NULL, NULL, NULL);
                fprintf(stderr, ",\"old_fallback\":%s,\"new_fallback\":%s}\n", mptcp_cache.fallback ? "true" : "false",
                        fallback ? "true" : "false");
            }
            else {
                fprintf(stderr, "MPTCP path transition: fallback %s -> %s\n", mptcp_cache.fallback ? "on" : "off",
                        fallback ? "on" : "off");
            }
        }

        for (i = 0; i < subflow_count; i++) {
            ssize_t old_idx = find_subflow(mptcp_cache.subflows, mptcp_cache.subflow_count, &subflows[i]);

            if (old_idx < 0) {
                if (jflag) {
                    emit_mptcp_transition_json("mptcp_path_subflow_added", &subflows[i], NULL, NULL);
                }
                else {
                    fprintf(stderr, "MPTCP path transition: added id=%u %s <-> %s state=%s\n", subflows[i].id,
                            subflows[i].local, subflows[i].remote, mptcp_state_name(subflows[i].state));
                }
            }
            else if (mptcp_cache.subflows[old_idx].state != subflows[i].state) {
                if (jflag) {
                    emit_mptcp_transition_json("mptcp_path_subflow_state_changed", &subflows[i],
                                               mptcp_state_name(mptcp_cache.subflows[old_idx].state),
                                               mptcp_state_name(subflows[i].state));
                }
                else {
                    fprintf(stderr, "MPTCP path transition: state id=%u %s <-> %s %s -> %s\n", subflows[i].id,
                            subflows[i].local, subflows[i].remote,
                            mptcp_state_name(mptcp_cache.subflows[old_idx].state), mptcp_state_name(subflows[i].state));
                }
            }
        }

        for (i = 0; i < mptcp_cache.subflow_count; i++) {
            ssize_t new_idx = find_subflow(subflows, subflow_count, &mptcp_cache.subflows[i]);

            if (new_idx >= 0)
                continue;
            if (jflag) {
                emit_mptcp_transition_json("mptcp_path_subflow_removed", &mptcp_cache.subflows[i], NULL, NULL);
            }
            else {
                fprintf(stderr, "MPTCP path transition: removed id=%u %s <-> %s state=%s\n", mptcp_cache.subflows[i].id,
                        mptcp_cache.subflows[i].local, mptcp_cache.subflows[i].remote,
                        mptcp_state_name(mptcp_cache.subflows[i].state));
            }
        }
    }

    if (changed) {
        if (jflag) {
            json_event_begin(stderr, "info", "mptcp_info", "out", "tcp", "disabled", "disabled", NULL, NULL, NULL,
                             NULL);
            fprintf(stderr, ",\"subflows\":%u,\"subflows_max\":%u,\"flags\":%u,\"fallback\":%s,\"mode\":\"%s\"}\n",
                    info.mptcpi_subflows, info.mptcpi_subflows_max, info.mptcpi_flags, fallback ? "true" : "false",
                    mode);

            if (vflag >= 2 && subflow_count > 0) {
                json_event_begin(stderr, "info", "mptcp_subflows", "out", "tcp", "disabled", "disabled", NULL, NULL,
                                 NULL, NULL);
                fprintf(stderr, ",\"mode\":\"%s\",\"subflows\":[", mode);
                for (i = 0; i < subflow_count; i++) {
                    fprintf(stderr, "%s{\"id\":%u,\"local\":\"%s\",\"remote\":\"%s\",\"state\":\"%s\"}",
                            i > 0 ? "," : "", subflows[i].id, subflows[i].local, subflows[i].remote,
                            mptcp_state_name(subflows[i].state));
                }
                fprintf(stderr, "]}\n");
            }
        }
        else {
            fprintf(stderr, "MPTCP subflows: %u/%u", info.mptcpi_subflows, info.mptcpi_subflows_max);
            if (fallback)
                fprintf(stderr, " (fallback)");
            fprintf(stderr, " mode=%s\n", mode);

            if (vflag >= 2) {
                for (i = 0; i < subflow_count; i++) {
                    fprintf(stderr, "  subflow %zu id=%u state=%s %s <-> %s\n", i, subflows[i].id,
                            mptcp_state_name(subflows[i].state), subflows[i].local, subflows[i].remote);
                }
            }
        }
    }

    free(mptcp_cache.subflows);
    mptcp_cache.subflows = subflows;
    mptcp_cache.subflow_count = subflow_count;
    mptcp_cache.fallback = fallback;
    nc_strlcpy(mptcp_cache.mode, mode, sizeof(mptcp_cache.mode));
    mptcp_cache.initialized = 1;
#endif /* IPPROTO_MPTCP */
}
