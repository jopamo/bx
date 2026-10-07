#define _DEFAULT_SOURCE

/*
    Copyright (c)  2006, 2007		Dmitry Butskoy
                                        <dmitry@butskoy.name>
    License:  GPL v2 or any later

    See COPYING for the status of this software.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <poll.h>
#include <netinet/icmp6.h>
#include <netinet/ip_icmp.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip6.h>
#include <netinet/tcp.h>

#include "lib/fd_ops.h"
#include "traceroute.h"

#ifndef IP_MTU
#define IP_MTU 14
#endif

#ifndef TCPOPT_FASTOPEN
#define TCPOPT_FASTOPEN 34
#define TCPOLEN_FASTOPEN_BASE 2
#endif

 /*  enough, enough...  */

#if defined(__GLIBC__)
#define TCPHDR_SPORT(TH) ((TH)->source)
#define TCPHDR_DPORT(TH) ((TH)->dest)
#define TCPHDR_SEQ(TH) ((TH)->seq)
#define TCPHDR_ACK(TH) ((TH)->ack_seq)
#define TCPHDR_DOFF(TH) ((TH)->doff)
#define TCPHDR_WIN(TH) ((TH)->window)
#define TCPHDR_SUM(TH) ((TH)->check)
#define TCPHDR_URG(TH) ((TH)->urg_ptr)
#else
#define TCPHDR_SPORT(TH) ((TH)->th_sport)
#define TCPHDR_DPORT(TH) ((TH)->th_dport)
#define TCPHDR_SEQ(TH) ((TH)->th_seq)
#define TCPHDR_ACK(TH) ((TH)->th_ack)
#define TCPHDR_DOFF(TH) ((TH)->th_off)
#define TCPHDR_WIN(TH) ((TH)->th_win)
#define TCPHDR_SUM(TH) ((TH)->th_sum)
#define TCPHDR_URG(TH) ((TH)->th_urp)
#endif

#define TH_FLAGS(TH) (((uint8_t*)(TH))[13])
#define TH_FIN 0x01
#define TH_SYN 0x02
#define TH_RST 0x04
#define TH_PSH 0x08
#define TH_ACK 0x10
#define TH_URG 0x20
#define TH_ECE 0x40
#define TH_CWR 0x80

 /*  & 0xff == tcp_flags ...  */

#define FL_FLAGS 0x0100
#define FL_ECN 0x0200
#define FL_SACK 0x0400
#define FL_TSTAMP 0x0800
#define FL_WSCALE 0x1000

static struct {
    const char* name;
    unsigned int flag;
} const tcp_flags[] = {
    {"fin", TH_FIN}, {"syn", TH_SYN}, {"rst", TH_RST}, {"psh", TH_PSH},
    {"ack", TH_ACK}, {"urg", TH_URG}, {"ece", TH_ECE}, {"cwr", TH_CWR},
};

static char* print_tcp_info( struct tcphdr* tcp, size_t len) {
    size_t i;
    char str[128]; /*  enough...  */
    char* curr = str;
    char* end = str + (sizeof(str) / sizeof(*str) - 1);
    const char* p;
    unsigned int tcp_reply_flags;
    uint8_t* ptr;

    if (len < sizeof(struct tcphdr) || len != (size_t)(TCPHDR_DOFF(tcp) << 2))
        return NULL;

    tcp_reply_flags = TH_FLAGS(tcp);

    for (i = 0; i < sizeof(tcp_flags) / sizeof(*tcp_flags); i++) {
        if (!(tcp_reply_flags & tcp_flags[i].flag))
            continue;

        if (curr > str && curr < end)
            *curr++ = ',';
        for (p = tcp_flags[i].name; *p && curr < end; *curr++ = *p++)
            ;
    }

    ptr = (uint8_t*)(tcp + 1);
    len -= sizeof(struct tcphdr);

    while (len > 1) {
        int op = *ptr;
        size_t oplen = ptr[1];
        char optbuf[16];
        const char* name = NULL;

        switch (op) {
            case TCPOPT_EOL:
                len = 0;
                continue; /*  no more...  */
            case TCPOPT_NOP:
                oplen = 1;
                break;
            case TCPOPT_MAXSEG:
                if (oplen == TCPOLEN_MAXSEG && oplen <= len) {
                    uint16_t rcv_mss = ntohs(*((uint16_t*)(ptr + 2)));
                    snprintf(optbuf, sizeof(optbuf), "mss=%u", rcv_mss);
                    name = optbuf;
                }
                break;
            case TCPOPT_SACK_PERMITTED:
                if (oplen == TCPOLEN_SACK_PERMITTED)
                    name = "sack";
                break;
            case TCPOPT_TIMESTAMP:
                if (oplen == TCPOLEN_TIMESTAMP)
                    name = "timestamps";
                break;
            case TCPOPT_WINDOW:
                if (oplen == TCPOLEN_WINDOW)
                    name = "window_scaling";
                break;
            case TCPOPT_FASTOPEN:
                if (oplen >= TCPOLEN_FASTOPEN_BASE)
                    name = "fastopen";
                break;
        }

        if (name) {
            if (curr > str && curr < end)
                *curr++ = ',';
            for (p = name; *p && curr < end; *curr++ = *p++)
                ;
        }

        if (len < oplen)
            break;
        len -= oplen;
        ptr += oplen;
    }

    *curr = '\0';

    return strdup(str);
}

#define SYSCTL_PREFIX "/proc/sys/net/ipv4/tcp_"
static int check_sysctl( const char* name) {
    int fd, res;
    char sysctl_path[sizeof(SYSCTL_PREFIX) + strlen(name) + 1];
    uint8_t ch;

    strcpy(sysctl_path, SYSCTL_PREFIX);
    strcat(sysctl_path, name);

    fd = bx_fd_open_cloexec(sysctl_path, O_RDONLY, 0);
    if (fd < 0)
        return 0;

    res = read(fd, &ch, sizeof(ch));
    close(fd);

    if (res != sizeof(ch))
        return 0;

    /*  since kernel 2.6.31 "tcp_ecn" can have value of '2'...  */
    if (ch == '1')
        return 1;

    return 0;
}

static int tcp_init(struct bx_traceroute_ctx* ctx, unsigned int port_seq, size_t* packet_len_p) {
    const sockaddr_any* dest = &ctx->destination;
    int af = dest->sa.sa_family;
    sockaddr_any src;
    int mtu;
    socklen_t len;
    uint8_t* ptr;
    uint16_t* lenp;

    ctx->method_state.tcp.dest_addr = *dest;
    ctx->method_state.tcp.dest_addr.sin.sin_port = 0; /*  raw sockets can be confused   */

    if (!port_seq)
        port_seq = DEF_TCP_PORT;
    ctx->method_state.tcp.dest_port = htons(port_seq);

    /*  Create raw socket for tcp   */

    ctx->method_state.tcp.raw_sk = bx_fd_socket_cloexec(af, SOCK_RAW, IPPROTO_TCP);
    if (ctx->method_state.tcp.raw_sk < 0)
        bx_traceroute_error_or_perm(ctx, "socket");

    bx_traceroute_tune_socket(ctx, ctx->method_state.tcp.raw_sk, NULL); /*  including bind, if any   */

    if (connect(ctx->method_state.tcp.raw_sk, &ctx->method_state.tcp.dest_addr.sa, sizeof(ctx->method_state.tcp.dest_addr)) < 0)
        bx_traceroute_error(ctx, "connect");

    len = sizeof(src);
    if (getsockname(ctx->method_state.tcp.raw_sk, &src.sa, &len) < 0)
        bx_traceroute_error(ctx, "getsockname");

    len = sizeof(mtu);
    if (getsockopt(ctx->method_state.tcp.raw_sk, af == AF_INET ? SOL_IP : SOL_IPV6, af == AF_INET ? IP_MTU : IPV6_MTU, &mtu, &len) < 0 ||
        mtu < 576)
        mtu = 576;

    /*  mss = mtu - headers   */
    mtu -= af == AF_INET ? sizeof(struct iphdr) : sizeof(struct ip6_hdr);
    mtu -= sizeof(struct tcphdr);

    if (ctx->method_state.tcp.mss < 0)
        ctx->method_state.tcp.mss = mtu;

    if (!bx_traceroute_raw_can_connect()) { /*  work-around for buggy kernels  */
        close(ctx->method_state.tcp.raw_sk);
        ctx->method_state.tcp.raw_sk = bx_fd_socket_cloexec(af, SOCK_RAW, IPPROTO_TCP);
        if (ctx->method_state.tcp.raw_sk < 0)
            bx_traceroute_error(ctx, "socket");
        bx_traceroute_tune_socket(ctx, ctx->method_state.tcp.raw_sk, NULL);
        /*  but do not connect it...  */
    }

    bx_traceroute_use_recverr(ctx, ctx->method_state.tcp.raw_sk);

    bx_traceroute_add_poll(ctx, ctx->method_state.tcp.raw_sk, POLLIN | POLLERR);

    /*  Now create the sample packet.  */

    if (!ctx->method_state.tcp.flags)
        ctx->method_state.tcp.sysctl = 1;

    if (ctx->method_state.tcp.sysctl) {
        if (check_sysctl("ecn"))
            ctx->method_state.tcp.flags |= FL_ECN;
        if (check_sysctl("sack"))
            ctx->method_state.tcp.flags |= FL_SACK;
        if (check_sysctl("timestamps"))
            ctx->method_state.tcp.flags |= FL_TSTAMP;
        if (check_sysctl("window_scaling"))
            ctx->method_state.tcp.flags |= FL_WSCALE;
    }

    if (!(ctx->method_state.tcp.flags & (FL_FLAGS | 0xff))) { /*  no any tcp flag set   */
        ctx->method_state.tcp.flags |= TH_SYN;
        if (ctx->method_state.tcp.flags & FL_ECN)
            ctx->method_state.tcp.flags |= TH_ECE | TH_CWR;
    }

    /*  For easy checksum computing:
        saddr
        daddr
        length
        protocol
        tcphdr
        tcpoptions
    */

    ptr = ctx->method_state.tcp.buf;

    if (af == AF_INET) {
        len = sizeof(src.sin.sin_addr);
        memcpy(ptr, &src.sin.sin_addr, len);
        ptr += len;
        memcpy(ptr, &ctx->method_state.tcp.dest_addr.sin.sin_addr, len);
        ptr += len;
    }
    else {
        len = sizeof(src.sin6.sin6_addr);
        memcpy(ptr, &src.sin6.sin6_addr, len);
        ptr += len;
        memcpy(ptr, &ctx->method_state.tcp.dest_addr.sin6.sin6_addr, len);
        ptr += len;
    }

    lenp = (uint16_t*)ptr;
    ptr += sizeof(uint16_t);
    *((uint16_t*)ptr) = htons((uint16_t)IPPROTO_TCP);
    ptr += sizeof(uint16_t);

    /*  Construct TCP header   */

    ctx->method_state.tcp.th = (struct tcphdr*)ptr;

    TCPHDR_SPORT(ctx->method_state.tcp.th) = 0; /*  temporary   */
    TCPHDR_DPORT(ctx->method_state.tcp.th) = ctx->method_state.tcp.dest_port;
    TCPHDR_SEQ(ctx->method_state.tcp.th) = 0; /*  temporary   */
    TCPHDR_ACK(ctx->method_state.tcp.th) = 0;
    TCPHDR_DOFF(ctx->method_state.tcp.th) = 0; /*  later...  */
    TH_FLAGS(ctx->method_state.tcp.th) = ctx->method_state.tcp.flags & 0xff;
    TCPHDR_WIN(ctx->method_state.tcp.th) = htons(4 * mtu);
    TCPHDR_SUM(ctx->method_state.tcp.th) = 0;
    TCPHDR_URG(ctx->method_state.tcp.th) = 0;

    /*  Build TCP options   */

    ptr = (uint8_t*)(ctx->method_state.tcp.th + 1);

    if (ctx->method_state.tcp.flags & TH_SYN) {
        *ptr++ = TCPOPT_MAXSEG;  /*  2   */
        *ptr++ = TCPOLEN_MAXSEG; /*  4   */
        *((uint16_t*)ptr) = htons(ctx->method_state.tcp.mss);
        ptr += sizeof(uint16_t);
    }

    if (ctx->method_state.tcp.flags & FL_TSTAMP) {
        if (ctx->method_state.tcp.flags & FL_SACK) {
            *ptr++ = TCPOPT_SACK_PERMITTED;  /*  4   */
            *ptr++ = TCPOLEN_SACK_PERMITTED; /*  2   */
        }
        else {
            *ptr++ = TCPOPT_NOP; /*  1   */
            *ptr++ = TCPOPT_NOP; /*  1   */
        }
        *ptr++ = TCPOPT_TIMESTAMP;  /*  8   */
        *ptr++ = TCPOLEN_TIMESTAMP; /*  10  */

        *((uint32_t*)ptr) = bx_traceroute_random_seq(ctx); /*  really!  */
        ptr += sizeof(uint32_t);
        *((uint32_t*)ptr) = (ctx->method_state.tcp.flags & TH_ACK) ? bx_traceroute_random_seq(ctx) : 0;
        ptr += sizeof(uint32_t);
    }
    else if (ctx->method_state.tcp.flags & FL_SACK) {
        *ptr++ = TCPOPT_NOP;             /*  1   */
        *ptr++ = TCPOPT_NOP;             /*  1   */
        *ptr++ = TCPOPT_SACK_PERMITTED;  /*  4   */
        *ptr++ = TCPOLEN_SACK_PERMITTED; /*  2   */
    }

    if (ctx->method_state.tcp.flags & FL_WSCALE) {
        *ptr++ = TCPOPT_NOP;     /*  1   */
        *ptr++ = TCPOPT_WINDOW;  /*  3   */
        *ptr++ = TCPOLEN_WINDOW; /*  3   */
        *ptr++ = 2;              /*  assume some corect value...  */
    }

    if (ctx->method_state.tcp.fastopen && (ctx->method_state.tcp.flags & TH_SYN)) {
        *ptr++ = TCPOPT_FASTOPEN; /*  34  */
        if (ctx->method_state.tcp.flags & TH_ACK) {
            /*  cookie size of 8 is defined in kernel's linux/tcp.h  */
            *ptr++ = TCPOLEN_FASTOPEN_BASE + 2 * sizeof(uint32_t);
            *((uint32_t*)ptr) = bx_traceroute_random_seq(ctx);
            ptr += sizeof(uint32_t);
            *((uint32_t*)ptr) = bx_traceroute_random_seq(ctx);
            ptr += sizeof(uint32_t);
        }
        else
            *ptr++ = TCPOLEN_FASTOPEN_BASE + 0; /*  2   */
        *ptr++ = TCPOPT_NOP;                    /*  1   */
        *ptr++ = TCPOPT_NOP;                    /*  1   */
    }

    ctx->method_state.tcp.csum_len = ptr - ctx->method_state.tcp.buf;

    if (ctx->method_state.tcp.csum_len > sizeof(ctx->method_state.tcp.buf))
        bx_traceroute_error(ctx, "impossible"); /*  paranoia   */

    len = ptr - (uint8_t*)ctx->method_state.tcp.th;
    if (len & 0x03)
        bx_traceroute_error(ctx, "impossible"); /*  as >>2 ...  */

    *lenp = htons(len);
    TCPHDR_DOFF(ctx->method_state.tcp.th) = len >> 2;

    *packet_len_p = len;

    return 0;
}

static void tcp_send_probe(struct bx_traceroute_ctx* ctx, probe* pb, int ttl) {
    int sk;
    int af = ctx->method_state.tcp.dest_addr.sa.sa_family;
    sockaddr_any addr;
    socklen_t len = sizeof(addr);

    /*  To make sure we have chosen a free unused "source port",
       just create, (auto)bind and hold a socket while the port is needed.
    */

    sk = bx_fd_socket_cloexec(af, SOCK_STREAM, 0);
    if (sk < 0)
        bx_traceroute_error(ctx, "socket");

    if (ctx->method_state.tcp.reuse && setsockopt(sk, SOL_SOCKET, SO_REUSEADDR, &ctx->method_state.tcp.reuse, sizeof(ctx->method_state.tcp.reuse)) < 0)
        bx_traceroute_error(ctx, "setsockopt SO_REUSEADDR");

    bx_traceroute_bind_socket(ctx, sk, pb);

    if (getsockname(sk, &addr.sa, &len) < 0)
        bx_traceroute_error(ctx, "getsockname");

    /*  When we reach the target host, it can send us either RST or SYN+ACK.
      For RST all is OK (we and kernel just answer nothing), but
      for SYN+ACK we should reply with our RST.
        It is well-known "half-open technique", used by port scanners etc.
      This way we do not touch remote applications at all, unlike
      the ordinary connect(2) call.
        As the port-holding socket neither connect() nor listen(),
      it means "no such port yet" for remote ends, and kernel always
      send RST in such a situation automatically (we have to do nothing).
    */

    TCPHDR_SPORT(ctx->method_state.tcp.th) = addr.sin.sin_port;

    TCPHDR_SEQ(ctx->method_state.tcp.th) = bx_traceroute_random_seq(ctx);

    TCPHDR_SUM(ctx->method_state.tcp.th) = 0;
    TCPHDR_SUM(ctx->method_state.tcp.th) = bx_traceroute_in_csum(ctx->method_state.tcp.buf, ctx->method_state.tcp.csum_len);

    if (ttl != ctx->method_state.tcp.last_ttl) {
        bx_traceroute_set_ttl(ctx, ctx->method_state.tcp.raw_sk, ttl);

        ctx->method_state.tcp.last_ttl = ttl;
    }

    pb->send_time = bx_traceroute_get_time();

    if (bx_traceroute_do_send(ctx, ctx->method_state.tcp.raw_sk, ctx->method_state.tcp.th, TCPHDR_DOFF(ctx->method_state.tcp.th) << 2, &ctx->method_state.tcp.dest_addr) < 0) {
        close(sk);
        pb->send_time = 0;
        return;
    }

    pb->seq = TCPHDR_SPORT(ctx->method_state.tcp.th);

    pb->sk = sk;

    return;
}

static probe* tcp_check_reply(struct bx_traceroute_ctx* ctx, int sk, int err, sockaddr_any* from, char* reply_buf, size_t len) {
    probe* pb;
    struct tcphdr* tcp = (struct tcphdr*)reply_buf;
    uint16_t sport, dport;

    (void)sk;

    if (len < 8)
        return NULL; /*  too short   */

    if (err) {
        sport = TCPHDR_SPORT(tcp);
        dport = TCPHDR_DPORT(tcp);
    }
    else {
        sport = TCPHDR_DPORT(tcp);
        dport = TCPHDR_SPORT(tcp);
    }

    if (dport != ctx->method_state.tcp.dest_port)
        return NULL;

    if (!bx_traceroute_equal_addr(&ctx->method_state.tcp.dest_addr, from))
        return NULL;

    pb = bx_traceroute_probe_by_seq(ctx, sport);
    if (!pb)
        return NULL;

    if (ctx->method_state.tcp.check_mss && err && len >= sizeof(*tcp) + TCPOLEN_MAXSEG) {
        uint8_t* ptr = (uint8_t*)(tcp + 1);

        if (ptr[0] == TCPOPT_MAXSEG && ptr[1] == TCPOLEN_MAXSEG) {
            uint16_t seen_mss = ntohs(*((uint16_t*)(ptr + 2)));
            if (ctx->method_state.tcp.mss != seen_mss) {
                bx_traceroute_put_err(pb, "M=%u", seen_mss);
                ctx->method_state.tcp.mss = seen_mss; /*  print just once   */
            }
        }
    }

    if (!err) {
        pb->final = 1;

        if (ctx->method_state.tcp.info)
            pb->ext = print_tcp_info(tcp, len);
    }

    return pb;
}

static void tcp_recv_probe(struct bx_traceroute_ctx* ctx, int sk, int revents) {
    if (!(revents & (POLLIN | POLLERR)))
        return;

    bx_traceroute_recv_reply(ctx, sk, !!(revents & POLLERR), tcp_check_reply);
}

static void tcp_expire_probe(struct bx_traceroute_ctx* ctx, probe* pb) {
    bx_traceroute_probe_done(ctx, pb);
}

static int tcp_parse_option(struct bx_traceroute_ctx* ctx, const char* option) {
    const char* value;
    unsigned int number;
    struct bx_traceroute_tcp_state* tcp = &ctx->method_state.tcp;
    for (size_t i = 0; i < sizeof(tcp_flags) / sizeof(*tcp_flags); i++) {
        if (!strcmp(option, tcp_flags[i].name)) {
            tcp->flags |= tcp_flags[i].flag;
            return 0;
        }
    }
    if (bx_traceroute_option_is(option, "flags", true, &value)) {
        if (!value)
            return BX_TRACEROUTE_OPTION_NEEDS_ARGUMENT;
        if (!bx_traceroute_parse_uint(value, &number))
            return BX_TRACEROUTE_OPTION_BAD_ARGUMENT;
        tcp->flags = (tcp->flags & ~0xff) | (number & 0xff) | FL_FLAGS;
        return 0;
    }
    if (!strcmp(option, "ecn")) tcp->flags |= FL_ECN;
    else if (!strcmp(option, "sack")) tcp->flags |= FL_SACK;
    else if (bx_traceroute_option_is(option, "timestamps", true, &value) && !value) tcp->flags |= FL_TSTAMP;
    else if (bx_traceroute_option_is(option, "window_scaling", true, &value) && !value) tcp->flags |= FL_WSCALE;
    else if (!strcmp(option, "sysctl")) tcp->sysctl = 1;
    else if (!strcmp(option, "fastopen")) tcp->fastopen = 1;
    else if (!strcmp(option, "reuse")) tcp->reuse = 1;
    else if (!strcmp(option, "info")) tcp->info = 1;
    else if (bx_traceroute_option_is(option, "mss", false, &value)) {
        tcp->check_mss = 1;
        if (value) {
            if (!bx_traceroute_parse_uint(value, &number))
                return BX_TRACEROUTE_OPTION_BAD_ARGUMENT;
            tcp->mss = (int)number;
        }
    }
    else return -1;
    return 0;
}

static void tcp_reset(struct bx_traceroute_ctx* ctx) {
    memset(&ctx->method_state.tcp, 0, sizeof(ctx->method_state.tcp));
    ctx->method_state.tcp.raw_sk = -1;
    ctx->method_state.tcp.mss = -1;
}

static void tcp_print_options(FILE* stream) {
    fputs(
        "  syn                         Set tcp flag SYN (default if no other tcp flags\n"
        "                              specified)\n"
        "  ack                         Set tcp flag ACK,\n"
        "  fin                         FIN,\n"
        "  rst                         RST,\n"
        "  psh                         PSH,\n"
        "  urg                         URG,\n"
        "  ece                         ECE,\n"
        "  cwr                         CWR\n"
        "  flags=NUM                   Set tcp flags exactly to value NUM\n"
        "  ecn                         Send syn packet with tcp flags ECE and CWR (for\n"
        "                              Explicit Congestion Notification, rfc3168)\n"
        "  sack                        Use sack,\n"
        "  timestamps                  timestamps,\n"
        "  window_scaling              window_scaling option for tcp\n"
        "  sysctl                      Use current sysctl (/proc/sys/net/*) setting for\n"
        "                              the tcp options above and ecn. Always set by\n"
        "                              default (with \"syn\") if nothing else specified\n"
        "  fastopen                    Use fastopen tcp option (when syn, cookie\n"
        "                              negotiation only)\n"
        "  reuse                       Allow to reuse local port numbers for the huge\n"
        "                              workloads (SO_REUSEADDR)\n"
        "  mss[=NUM]                   Use value of NUM (or unchanged) for maxseg tcp\n"
        "                              option (when syn), and discover its clamping\n"
        "                              along the path being traced\n"
        "  info                        Print tcp flags and options of final tcp replies\n"
        "                              when target host is reached. Useful to determine\n"
        "                              whether an application listens the port etc.\n"
        , stream);
}

static void tcp_destroy(struct bx_traceroute_ctx* ctx) {
    if (ctx->method_state.tcp.raw_sk >= 0) {
        bx_traceroute_del_poll(ctx, ctx->method_state.tcp.raw_sk);
        close(ctx->method_state.tcp.raw_sk);
        ctx->method_state.tcp.raw_sk = -1;
    }
}

const struct bx_traceroute_method bx_traceroute_method_tcp = {
    .reset = tcp_reset,
    .destroy = tcp_destroy,
    .parse_option = tcp_parse_option,
    .print_options = tcp_print_options,
    .id = BX_TRACEROUTE_METHOD_TCP,
    .name = "tcp",
    .init = tcp_init,
    .send_probe = tcp_send_probe,
    .recv_probe = tcp_recv_probe,
    .expire_probe = tcp_expire_probe,
};

