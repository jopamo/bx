/*
    Copyright (c)  2006, 2007		Dmitry Butskoy
                                        <dmitry@butskoy.name>
    License:  GPL v2 or any later

    See COPYING for the status of this software.
*/

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/icmp6.h>
#include <netinet/ip_icmp.h>
#include <netinet/ip6.h>
#include <poll.h>
#include <time.h>
#include <linux/errqueue.h>
#include <linux/net_tstamp.h>

#include "flowlabel.h"
#include "lib/fd_ops.h"
#include "lib/time_parse.h"
#include "traceroute.h"

#ifndef ICMP6_DST_UNREACH_BEYONDSCOPE
#ifdef ICMP6_DST_UNREACH_NOTNEIGHBOR
#define ICMP6_DST_UNREACH_BEYONDSCOPE ICMP6_DST_UNREACH_NOTNEIGHBOR
#else
#define ICMP6_DST_UNREACH_BEYONDSCOPE 2
#endif
#endif

#ifndef IPV6_RECVHOPLIMIT
#define IPV6_RECVHOPLIMIT IPV6_HOPLIMIT
#endif

#ifndef IP_PMTUDISC_PROBE
#define IP_PMTUDISC_PROBE 3
#endif

#ifndef IPV6_PMTUDISC_PROBE
#define IPV6_PMTUDISC_PROBE 3
#endif


static int ecmp_rotates_source_port(struct bx_traceroute_ctx* ctx) {
    const char* mod_name = ctx->method ? ctx->method->name : ctx->options.method_name;

    if (ctx->options.source_port)
        return 1;

    if (!mod_name)
        return 0;

    return !strcmp(mod_name, "default") || !strcmp(mod_name, "udp") || !strcmp(mod_name, "tcp");
}

static unsigned int ecmp_flow_slot(struct bx_traceroute_ctx* ctx, const probe* pb) {
    unsigned int np = (unsigned int)(pb - ctx->probes) % ctx->options.probes_per_hop;
    return np % ctx->options.ecmp;
}

int bx_traceroute_ecmp_flow_inflight(struct bx_traceroute_ctx* ctx, const probe* pb) {
    unsigned int n;
    unsigned int slot;

    if (!ctx->options.ecmp || !pb || !ecmp_rotates_source_port(ctx))
        return 0;

    slot = ecmp_flow_slot(ctx, pb);

    for (n = 0; n < ctx->probe_count; n++) {
        probe* p = &ctx->probes[n];

        if (p == pb || p->done || !p->send_time)
            continue;

        if (ecmp_flow_slot(ctx, p) == slot)
            return 1;
    }

    return 0;
}

void bx_traceroute_tune_socket(struct bx_traceroute_ctx* ctx, int sk, probe* pb) {
    int i = 0;

    if (ctx->options.debug) {
        i = 1;
        if (setsockopt(sk, SOL_SOCKET, SO_DEBUG, &i, sizeof(i)) < 0)
            bx_traceroute_error(ctx, "setsockopt SO_DEBUG");
    }

#ifdef SO_MARK
    if (ctx->options.fwmark) {
        if (setsockopt(sk, SOL_SOCKET, SO_MARK, &ctx->options.fwmark, sizeof(ctx->options.fwmark)) < 0)
            bx_traceroute_error(ctx, "setsockopt SO_MARK");
    }
#endif

    if (ctx->rtbuf && ctx->rtbuf_len) {
        if (ctx->options.address_family == AF_INET) {
            if (setsockopt(sk, IPPROTO_IP, IP_OPTIONS, ctx->rtbuf, ctx->rtbuf_len) < 0)
                bx_traceroute_error(ctx, "setsockopt IP_OPTIONS");
        }
        else if (ctx->options.address_family == AF_INET6) {
            if (setsockopt(sk, IPPROTO_IPV6, IPV6_RTHDR, ctx->rtbuf, ctx->rtbuf_len) < 0)
                bx_traceroute_error(ctx, "setsockopt IPV6_RTHDR");
        }
    }

    bx_traceroute_bind_socket(ctx, sk, pb);

    if (ctx->options.address_family == AF_INET) {
        i = ctx->options.dont_fragment ? IP_PMTUDISC_PROBE : IP_PMTUDISC_DONT;
        if (setsockopt(sk, SOL_IP, IP_MTU_DISCOVER, &i, sizeof(i)) < 0 &&
            (!ctx->options.dont_fragment || (i = IP_PMTUDISC_DO, setsockopt(sk, SOL_IP, IP_MTU_DISCOVER, &i, sizeof(i)) < 0)))
            bx_traceroute_error(ctx, "setsockopt IP_MTU_DISCOVER");

        if (ctx->options.tos) {
            i = ctx->options.tos;
            if (setsockopt(sk, SOL_IP, IP_TOS, &i, sizeof(i)) < 0)
                bx_traceroute_error(ctx, "setsockopt IP_TOS");
        }
    }
    else if (ctx->options.address_family == AF_INET6) {
        i = ctx->options.dont_fragment ? IPV6_PMTUDISC_PROBE : IPV6_PMTUDISC_DONT;
        if (setsockopt(sk, SOL_IPV6, IPV6_MTU_DISCOVER, &i, sizeof(i)) < 0 &&
            (!ctx->options.dont_fragment || (i = IPV6_PMTUDISC_DO, setsockopt(sk, SOL_IPV6, IPV6_MTU_DISCOVER, &i, sizeof(i)) < 0)))
            bx_traceroute_error(ctx, "setsockopt IPV6_MTU_DISCOVER");

        if (ctx->options.flow_label) {
            struct in6_flowlabel_req flr;
            unsigned int label = ctx->options.flow_label;

            if (ctx->options.ecmp && pb) {
                unsigned int np = (pb - ctx->probes) % ctx->options.probes_per_hop;
                unsigned int flow_idx = np % ctx->options.ecmp;
                label += flow_idx;
            }

            memset(&flr, 0, sizeof(flr));
            flr.flr_label = htonl(label & 0x000fffff);
            flr.flr_action = IPV6_FL_A_GET;
            flr.flr_flags = IPV6_FL_F_CREATE;
            flr.flr_share = IPV6_FL_S_ANY;
            memcpy(&flr.flr_dst, &ctx->destination.sin6.sin6_addr, sizeof(flr.flr_dst));

            if (setsockopt(sk, IPPROTO_IPV6, IPV6_FLOWLABEL_MGR, &flr, sizeof(flr)) < 0)
                bx_traceroute_error(ctx, "setsockopt IPV6_FLOWLABEL_MGR");
        }

        if (ctx->options.tos) {
            i = ctx->options.tos;
            if (setsockopt(sk, IPPROTO_IPV6, IPV6_TCLASS, &i, sizeof(i)) < 0)
                bx_traceroute_error(ctx, "setsockopt IPV6_TCLASS");
        }

        if (ctx->options.tos || ctx->options.flow_label) {
            i = 1;
            if (setsockopt(sk, IPPROTO_IPV6, IPV6_FLOWINFO_SEND, &i, sizeof(i)) < 0)
                bx_traceroute_error(ctx, "setsockopt IPV6_FLOWINFO_SEND");
        }
    }

    if (ctx->options.noroute) {
        i = ctx->options.noroute;
        if (setsockopt(sk, SOL_SOCKET, SO_DONTROUTE, &i, sizeof(i)) < 0)
            bx_traceroute_error(ctx, "setsockopt SO_DONTROUTE");
    }

    bx_traceroute_use_timestamp(ctx, sk);

    bx_traceroute_use_recv_ttl(ctx, sk);

    (void)bx_fd_set_nonblocking(sk, true);

    return;
}

void bx_traceroute_parse_icmp_res(struct bx_traceroute_ctx* ctx, probe* pb, int type, int code, int info) {
    if (ctx->options.address_family == AF_INET) {
        if (type == ICMP_TIME_EXCEEDED) {
            if (code == ICMP_EXC_TTL)
                return;
        }

        if (type == ICMP_DEST_UNREACH) {
            switch (code) {
                case ICMP_UNREACH_NET:
                case ICMP_UNREACH_NET_UNKNOWN:
                case ICMP_UNREACH_ISOLATED:
                case ICMP_UNREACH_TOSNET:
                    bx_traceroute_put_err(pb, "!N");
                    break;

                case ICMP_UNREACH_HOST:
                case ICMP_UNREACH_HOST_UNKNOWN:
                case ICMP_UNREACH_TOSHOST:
                    bx_traceroute_put_err(pb, "!H");
                    break;

                case ICMP_UNREACH_NET_PROHIB:
                case ICMP_UNREACH_HOST_PROHIB:
                case ICMP_UNREACH_FILTER_PROHIB:
                    bx_traceroute_put_err(pb, "!X");
                    break;

                case ICMP_UNREACH_PORT:
                    /*  dest host is reached   */
                    break;

                case ICMP_UNREACH_PROTOCOL:
                    bx_traceroute_put_err(pb, "!P");
                    break;

                case ICMP_UNREACH_NEEDFRAG:
                    bx_traceroute_put_err(pb, "!F-%d", info);
                    pb->mtu = info;
                    break;

                case ICMP_UNREACH_SRCFAIL:
                    bx_traceroute_put_err(pb, "!S");
                    break;

                case ICMP_UNREACH_HOST_PRECEDENCE:
                    bx_traceroute_put_err(pb, "!V");
                    break;

                case ICMP_UNREACH_PRECEDENCE_CUTOFF:
                    bx_traceroute_put_err(pb, "!C");
                    break;

                default:
                    bx_traceroute_put_err(pb, "!<%u>", code);
                    break;
            }
        }
        else
            bx_traceroute_put_err(pb, "!<%u-%u>", type, code);
    }
    else if (ctx->options.address_family == AF_INET6) {
        if (type == ICMP6_TIME_EXCEEDED) {
            if (code == ICMP6_TIME_EXCEED_TRANSIT)
                return;
        }

        if (type == ICMP6_DST_UNREACH) {
            switch (code) {
                case ICMP6_DST_UNREACH_NOROUTE:
                    bx_traceroute_put_err(pb, "!N");
                    break;

                case ICMP6_DST_UNREACH_BEYONDSCOPE:
                case ICMP6_DST_UNREACH_ADDR:
                    bx_traceroute_put_err(pb, "!H");
                    break;

                case ICMP6_DST_UNREACH_ADMIN:
                    bx_traceroute_put_err(pb, "!X");
                    break;

                case ICMP6_DST_UNREACH_NOPORT:
                    /*  dest host is reached   */
                    break;

                default:
                    bx_traceroute_put_err(pb, "!<%u>", code);
                    break;
            }
        }
        else if (type == ICMP6_PACKET_TOO_BIG) {
            bx_traceroute_put_err(pb, "!F-%d", info);
            pb->mtu = info;
        }
        else
            bx_traceroute_put_err(pb, "!<%u-%u>", type, code);
    }

    pb->final = 1;

    return;
}

static void parse_local_res(struct bx_traceroute_ctx* ctx, probe* pb, int ee_errno, int info) {
    if (ee_errno == EMSGSIZE && info != 0) {
        bx_traceroute_put_err(pb, "!F-%d", info);
        pb->final = 1;
        return;
    }

    errno = ee_errno;
    bx_traceroute_error(ctx, "local recverr");
}

void bx_traceroute_probe_done(struct bx_traceroute_ctx* ctx, probe* pb) {
    if (pb->sk) {
        bx_traceroute_del_poll(ctx, pb->sk);
        close(pb->sk);
        pb->sk = 0;
    }

    pb->seq = 0;

    pb->done = 1;
}

static struct cmsghdr* next_cmsg(struct bx_traceroute_ctx* ctx, struct msghdr* msg, struct cmsghdr* current) {
    (void)ctx;
    unsigned char* control = msg->msg_control;
    unsigned char* cursor = (unsigned char*)current;
    size_t offset = (size_t)(cursor - control);
    size_t cmsg_header_len = CMSG_LEN(0);
    size_t remaining;
    size_t step;

    if (offset > msg->msg_controllen)
        return NULL;
    remaining = msg->msg_controllen - offset;
    if (current->cmsg_len < cmsg_header_len || current->cmsg_len > remaining)
        return NULL;

    step = CMSG_SPACE(current->cmsg_len - cmsg_header_len);
    if (step > remaining || remaining - step < sizeof(struct cmsghdr))
        return NULL;
    return (struct cmsghdr*)(cursor + step);
}

void bx_traceroute_recv_reply(struct bx_traceroute_ctx* ctx, int sk, int err, check_reply_t check_reply) {
    struct msghdr msg;
    sockaddr_any from;
    struct iovec iov;
    int n;
    probe* pb;
    char buf[1280]; /*  min mtu for ipv6 ( >= 576 for ipv4)  */
    char* bufp = buf;
    union {
        struct cmsghdr align;
        unsigned char data[1024];
    } control;
    struct cmsghdr* cm;
    double recv_time = 0;
    int recv_ttl = 0;
    int ifindex_in = 0;
    int ifindex_out = 0;
    struct sock_extended_err* ee = NULL;

    memset(&msg, 0, sizeof(msg));
    msg.msg_name = &from;
    msg.msg_namelen = sizeof(from);
    msg.msg_control = control.data;
    msg.msg_controllen = sizeof(control);
    iov.iov_base = buf;
    iov.iov_len = sizeof(buf);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    n = recvmsg(sk, &msg, err ? MSG_ERRQUEUE : 0);
    if (n < 0)
        return;

    /*  when not MSG_ERRQUEUE, AF_INET returns full ipv4 header
        on raw sockets...
    */

    if (!err && ctx->options.address_family == AF_INET &&
        /*  XXX: Assume that the presence of an extra header means
            that it is not a raw socket...
        */
        ctx->method->header_len == 0) {
        struct iphdr* ip = (struct iphdr*)bufp;
        int hlen;

        if (n < (int)sizeof(struct iphdr))
            return;

        hlen = ip->ihl << 2;
        if (n < hlen)
            return;

        bufp += hlen;
        n -= hlen;
    }

    pb = check_reply(ctx, sk, err, &from, bufp, n);
    if (!pb) {
        /*  for `frag needed' case at the local host,
            kernel >= 3.13 sends local bx_traceroute_error (no more icmp)
        */
        if (!n && err && ctx->options.dont_fragment) {
            pb = &ctx->probes[(ctx->options.first_hop - 1) * ctx->options.probes_per_hop];
            if (pb->done)
                return;
        }
        else
            return;
    }

    /*  Parse CMSG stuff   */

    for (cm = CMSG_FIRSTHDR(&msg); cm; cm = next_cmsg(ctx, &msg, cm)) {
        void* ptr = CMSG_DATA(cm);

        if (cm->cmsg_level == SOL_SOCKET) {
            if (cm->cmsg_type == SCM_TIMESTAMPNS) {
                struct timespec* ts = (struct timespec*)ptr;
                double timestamp = 0.0;

                if (bx_time_timespec_to_seconds_double(ts, &timestamp))
                    recv_time = timestamp;
            }
            else if (cm->cmsg_type == SO_TIMESTAMP) {
                struct timeval* tv = (struct timeval*)ptr;
                double timestamp = 0.0;

                if (bx_time_timeval_to_seconds_double(tv, &timestamp))
                    recv_time = timestamp;
            }
            else if (cm->cmsg_type == SCM_TIMESTAMPING) {
                struct timespec* ts = (struct timespec*)ptr;
                const struct timespec* selected_ts = &ts[0];
                double timestamp = 0.0;
                /* ts[0] is software, ts[1] is transformed hardware, ts[2] is raw hardware */
                if (ctx->options.ts_mode == TS_KERNEL_HW && (ts[2].tv_sec || ts[2].tv_nsec))
                    selected_ts = &ts[2];
                if (bx_time_timespec_to_seconds_double(selected_ts, &timestamp))
                    recv_time = timestamp;
            }
        }
        else if (cm->cmsg_level == SOL_IP) {
            if (cm->cmsg_type == IP_TTL)
                recv_ttl = *((int*)ptr);
            else if (cm->cmsg_type == IP_PKTINFO) {
                struct in_pktinfo* pkt = (struct in_pktinfo*)ptr;
                ifindex_in = pkt->ipi_ifindex;
            }
            else if (cm->cmsg_type == IP_RECVERR) {
                ee = (struct sock_extended_err*)ptr;

                if (ee->ee_origin != SO_EE_ORIGIN_ICMP && ee->ee_origin != SO_EE_ORIGIN_LOCAL &&
                    ee->ee_origin != SO_EE_ORIGIN_TIMESTAMPING)
                    return;

                /*  dgram icmp sockets might return extra things...  */
                if (ee->ee_origin == SO_EE_ORIGIN_ICMP &&
                    (ee->ee_type == ICMP_SOURCE_QUENCH || ee->ee_type == ICMP_REDIRECT))
                    return;
            }
        }
        else if (cm->cmsg_level == SOL_IPV6) {
            if (cm->cmsg_type == IPV6_HOPLIMIT)
                recv_ttl = *((int*)ptr);
            else if (cm->cmsg_type == IPV6_PKTINFO) {
                struct in6_pktinfo* pkt = (struct in6_pktinfo*)ptr;
                ifindex_in = pkt->ipi6_ifindex;
            }
            else if (cm->cmsg_type == IPV6_RECVERR) {
                ee = (struct sock_extended_err*)ptr;

                if (ee->ee_origin != SO_EE_ORIGIN_ICMP6 && ee->ee_origin != SO_EE_ORIGIN_LOCAL &&
                    ee->ee_origin != SO_EE_ORIGIN_TIMESTAMPING)
                    return;
            }
        }
    }

    if (!recv_time)
        recv_time = bx_traceroute_get_time();

    if (!err)
        memcpy(&pb->res, &from, sizeof(pb->res));

    pb->recv_time = recv_time;

    pb->recv_ttl = recv_ttl;

    if (ee) {
        ifindex_out = ee->ee_data;
    }

    pb->ifindex_in = ifindex_in;
    pb->ifindex_out = ifindex_out;

    if (ee && ee->ee_origin == SO_EE_ORIGIN_TIMESTAMPING) {
        pb->send_time = recv_time;
        return;
    }

    if (ee && (ee->ee_origin == SO_EE_ORIGIN_ICMP || ee->ee_origin == SO_EE_ORIGIN_ICMP6)) {
        memcpy(&pb->res, SO_EE_OFFENDER(ee), sizeof(pb->res));
        bx_traceroute_parse_icmp_res(ctx, pb, ee->ee_type, ee->ee_code, ee->ee_info);
    }

    if (ee && ee->ee_origin == SO_EE_ORIGIN_LOCAL)
        parse_local_res(ctx, pb, ee->ee_errno, ee->ee_info);

    if (ee && ctx->options.mtu_discovery && ee->ee_info >= ctx->header_len && ee->ee_info < ctx->header_len + ctx->data_len) {
        ctx->data_len = ee->ee_info - ctx->header_len;

        bx_traceroute_probe_done(ctx, pb);

        /*  clear this probe (as actually the previous hop answers here)
          but fill its `err_str' by the info obtained. Ugly, but easy...
        */
        memset(pb, 0, sizeof(*pb));
        pb->mtu = ee->ee_info;
        bx_traceroute_put_err(pb, "F=%d", ee->ee_info);

        return;
    }

    if (ee && ctx->options.extension && ctx->header_len + n >= (128 + 8) && /*  at least... (rfc4884)  */
        ctx->header_len <= 128 &&                              /*  paranoia   */
        ((ctx->options.address_family == AF_INET && (ee->ee_type == ICMP_TIME_EXCEEDED || ee->ee_type == ICMP_DEST_UNREACH ||
                            ee->ee_type == ICMP_PARAMETERPROB)) ||
         (ctx->options.address_family == AF_INET6 && (ee->ee_type == ICMP6_TIME_EXCEEDED || ee->ee_type == ICMP6_DST_UNREACH)))) {
        int step;
        int offs = 128 - ctx->header_len;

        if ((size_t)n > ctx->data_len)
            step = 0; /*  guaranteed at 128 ...  */
        else
            step = ctx->options.address_family == AF_INET ? 4 : 8;

        bx_traceroute_handle_extensions(ctx, pb, bufp + offs, n - offs, step);
    }

    bx_traceroute_probe_done(ctx, pb);
}

int bx_traceroute_equal_addr(const sockaddr_any* a, const sockaddr_any* b) {
    if (!a->sa.sa_family)
        return 0;

    if (a->sa.sa_family != b->sa.sa_family)
        return 0;

    if (a->sa.sa_family == AF_INET6)
        return !memcmp(&a->sin6.sin6_addr, &b->sin6.sin6_addr, sizeof(a->sin6.sin6_addr));
    else
        return !memcmp(&a->sin.sin_addr, &b->sin.sin_addr, sizeof(a->sin.sin_addr));
    return 0; /*  not reached   */
}

void bx_traceroute_bind_socket(struct bx_traceroute_ctx* ctx, int sk, probe* pb) {
    sockaddr_any *addr, tmp;

    if (ctx->options.interface) {
        if (setsockopt(sk, SOL_SOCKET, SO_BINDTODEVICE, ctx->options.interface, strlen(ctx->options.interface) + 1) < 0)
            bx_traceroute_error(ctx, "setsockopt SO_BINDTODEVICE");
    }

    if (!ctx->source.sa.sa_family) {
        memset(&tmp, 0, sizeof(tmp));
        tmp.sa.sa_family = ctx->options.address_family;
        addr = &tmp;
    }
    else
        addr = &ctx->source;

    if (ctx->options.ecmp && pb && ecmp_rotates_source_port(ctx)) {
        unsigned int flow_idx = ecmp_flow_slot(ctx, pb);
        uint16_t port = ntohs(addr->sin.sin_port); /* same offset for sin6 */

        if (!port)
            port = DEF_START_PORT; /* arbitrary base for rotation if not specified */
        port += flow_idx;
        if (addr->sa.sa_family == AF_INET6)
            addr->sin6.sin6_port = htons(port);
        else
            addr->sin.sin_port = htons(port);
    }

    if (bind(sk, &addr->sa, sizeof(*addr)) < 0)
        bx_traceroute_error(ctx, "bind");

    return;
}

void bx_traceroute_use_timestamp(struct bx_traceroute_ctx* ctx, int sk) {
    int n = 1;
    int flags;

    if (ctx->options.ts_mode == TS_USERSPACE)
        return;

    if (ctx->options.ts_mode == TS_KERNEL_SW) {
        flags = SOF_TIMESTAMPING_RX_SOFTWARE | SOF_TIMESTAMPING_SOFTWARE | SOF_TIMESTAMPING_TX_SOFTWARE;
        if (setsockopt(sk, SOL_SOCKET, SO_TIMESTAMPING, &flags, sizeof(flags)) < 0) {
            /*  fallback to SO_TIMESTAMPNS if SO_TIMESTAMPING not supported   */
            if (setsockopt(sk, SOL_SOCKET, SO_TIMESTAMPNS, &n, sizeof(n)) < 0)
                setsockopt(sk, SOL_SOCKET, SO_TIMESTAMP, &n, sizeof(n));
        }
    }
    else if (ctx->options.ts_mode == TS_KERNEL_HW) {
        flags = SOF_TIMESTAMPING_RX_HARDWARE | SOF_TIMESTAMPING_RAW_HARDWARE | SOF_TIMESTAMPING_RX_SOFTWARE |
                SOF_TIMESTAMPING_SOFTWARE | SOF_TIMESTAMPING_TX_HARDWARE | SOF_TIMESTAMPING_TX_SOFTWARE;
        if (setsockopt(sk, SOL_SOCKET, SO_TIMESTAMPING, &flags, sizeof(flags)) < 0) {
            bx_traceroute_error(ctx, "setsockopt SO_TIMESTAMPING (kernel-hw)");
        }
    }
}

void bx_traceroute_use_recv_ttl(struct bx_traceroute_ctx* ctx, int sk) {
    int n = 1;

    if (ctx->options.address_family == AF_INET) {
        setsockopt(sk, SOL_IP, IP_RECVTTL, &n, sizeof(n));
        setsockopt(sk, SOL_IP, IP_PKTINFO, &n, sizeof(n));
    }
    else if (ctx->options.address_family == AF_INET6) {
        setsockopt(sk, SOL_IPV6, IPV6_RECVHOPLIMIT, &n, sizeof(n));
        setsockopt(sk, SOL_IPV6, IPV6_RECVPKTINFO, &n, sizeof(n));
    }
    /*  foo on errors   */
}

void bx_traceroute_use_recverr(struct bx_traceroute_ctx* ctx, int sk) {
    int val = 1;

    if (ctx->options.address_family == AF_INET) {
        if (setsockopt(sk, SOL_IP, IP_RECVERR, &val, sizeof(val)) < 0)
            bx_traceroute_error(ctx, "setsockopt IP_RECVERR");
    }
    else if (ctx->options.address_family == AF_INET6) {
        if (setsockopt(sk, SOL_IPV6, IPV6_RECVERR, &val, sizeof(val)) < 0)
            bx_traceroute_error(ctx, "setsockopt IPV6_RECVERR");
    }
}

void bx_traceroute_set_ttl(struct bx_traceroute_ctx* ctx, int sk, int ttl) {
    if (ctx->options.address_family == AF_INET) {
        if (setsockopt(sk, SOL_IP, IP_TTL, &ttl, sizeof(ttl)) < 0)
            bx_traceroute_error(ctx, "setsockopt IP_TTL");
    }
    else if (ctx->options.address_family == AF_INET6) {
        if (setsockopt(sk, SOL_IPV6, IPV6_UNICAST_HOPS, &ttl, sizeof(ttl)) < 0)
            bx_traceroute_error(ctx, "setsockopt IPV6_UNICAST_HOPS");
    }
}

int bx_traceroute_do_send(struct bx_traceroute_ctx* ctx, int sk, const void* data, size_t len, const sockaddr_any* addr) {
    int res;

    if (!addr || bx_traceroute_raw_can_connect())
        res = send(sk, data, len, 0);
    else
        res = sendto(sk, data, len, 0, &addr->sa, sizeof(*addr));

    if (res < 0) {
        if (errno == ENOBUFS || errno == EAGAIN)
            return res;
        if (errno == EMSGSIZE || errno == EHOSTUNREACH)
            return 0;  /*  recverr will say more...  */
        bx_traceroute_error(ctx, "send"); /*  not recoverable   */
    }

    return res;
}

int bx_traceroute_raw_can_connect(void) {
    return 1;
}

