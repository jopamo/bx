#define _DEFAULT_SOURCE

/*
    Copyright (c)  2006, 2007		Dmitry Butskoy
                                        <dmitry@butskoy.name>
    License:  GPL v2 or any later

    See COPYING for the status of this software.
*/

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <poll.h>
#include <netinet/icmp6.h>
#include <netinet/ip_icmp.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip6.h>

#include "lib/fd_ops.h"
#include "traceroute.h"

static int icmp_init(struct bx_traceroute_ctx* ctx, unsigned int port_seq, size_t* packet_len_p) {
    const sockaddr_any* dest = &ctx->destination;
    size_t i;
    int af = dest->sa.sa_family;
    int protocol;

    ctx->method_state.icmp.dest_addr = *dest;
    ctx->method_state.icmp.dest_addr.sin.sin_port = 0;

    if (port_seq)
        ctx->method_state.icmp.seq = port_seq;

    ctx->method_state.icmp.length_p = packet_len_p;
    if (*ctx->method_state.icmp.length_p < sizeof(struct icmphdr))
        *ctx->method_state.icmp.length_p = sizeof(struct icmphdr);

    ctx->method_state.icmp.data = malloc(*ctx->method_state.icmp.length_p);
    if (!ctx->method_state.icmp.data)
        bx_traceroute_error(ctx, "malloc");

    for (i = sizeof(struct icmphdr); i < *ctx->method_state.icmp.length_p; i++)
        ctx->method_state.icmp.data[i] = 0x40 + (i & 0x3f);

    protocol = (af == AF_INET) ? IPPROTO_ICMP : IPPROTO_ICMPV6;

    if (!ctx->method_state.icmp.raw) {
        ctx->method_state.icmp.icmp_sk = bx_fd_socket_cloexec(af, SOCK_DGRAM, protocol);
        if (ctx->method_state.icmp.icmp_sk < 0 && ctx->method_state.icmp.dgram)
            bx_traceroute_error(ctx, "socket");
    }

    if (!ctx->method_state.icmp.dgram) {
        int raw_sk = bx_fd_socket_cloexec(af, SOCK_RAW, protocol);
        if (raw_sk < 0) {
            if (ctx->method_state.icmp.raw || ctx->method_state.icmp.icmp_sk < 0)
                bx_traceroute_error_or_perm(ctx, "socket");
            ctx->method_state.icmp.dgram = 1;
        }
        else {
            /*  prefer the traditional "raw" way when possible   */
            close(ctx->method_state.icmp.icmp_sk);
            ctx->method_state.icmp.icmp_sk = raw_sk;
        }
    }

    bx_traceroute_tune_socket(ctx, ctx->method_state.icmp.icmp_sk, NULL);

    /*  Don't want to catch packets from another hosts   */
    if (bx_traceroute_raw_can_connect() && connect(ctx->method_state.icmp.icmp_sk, &ctx->method_state.icmp.dest_addr.sa, sizeof(ctx->method_state.icmp.dest_addr)) < 0)
        bx_traceroute_error(ctx, "connect");

    bx_traceroute_use_recverr(ctx, ctx->method_state.icmp.icmp_sk);

    if (ctx->method_state.icmp.dgram) {
        sockaddr_any addr;
        socklen_t len = sizeof(addr);

        if (getsockname(ctx->method_state.icmp.icmp_sk, &addr.sa, &len) < 0)
            bx_traceroute_error(ctx, "getsockname");
        ctx->method_state.icmp.ident = ntohs(addr.sin.sin_port); /*  both IPv4 and IPv6   */
    }
    else
        ctx->method_state.icmp.ident = getpid() & 0xffff;

    bx_traceroute_add_poll(ctx, ctx->method_state.icmp.icmp_sk, POLLIN | POLLERR);

    return 0;
}

static void icmp_send_probe(struct bx_traceroute_ctx* ctx, probe* pb, int ttl) {
    int af = ctx->method_state.icmp.dest_addr.sa.sa_family;

    if (ttl != ctx->method_state.icmp.last_ttl) {
        bx_traceroute_set_ttl(ctx, ctx->method_state.icmp.icmp_sk, ttl);

        ctx->method_state.icmp.last_ttl = ttl;
    }

    if (af == AF_INET) {
        struct icmp* icmp = (struct icmp*)ctx->method_state.icmp.data;

        icmp->icmp_type = ICMP_ECHO;
        icmp->icmp_code = 0;
        icmp->icmp_cksum = 0;
        icmp->icmp_id = htons(ctx->method_state.icmp.ident);
        icmp->icmp_seq = htons(ctx->method_state.icmp.seq);

        icmp->icmp_cksum = bx_traceroute_in_csum(ctx->method_state.icmp.data, *ctx->method_state.icmp.length_p);
    }
    else if (af == AF_INET6) {
        struct icmp6_hdr* icmp6 = (struct icmp6_hdr*)ctx->method_state.icmp.data;

        icmp6->icmp6_type = ICMP6_ECHO_REQUEST;
        icmp6->icmp6_code = 0;
        icmp6->icmp6_cksum = 0;
        icmp6->icmp6_id = htons(ctx->method_state.icmp.ident);
        icmp6->icmp6_seq = htons(ctx->method_state.icmp.seq);

        /*  icmp6->icmp6_cksum always computed by kernel internally   */
    }

    pb->send_time = bx_traceroute_get_time();

    if (bx_traceroute_do_send(ctx, ctx->method_state.icmp.icmp_sk, ctx->method_state.icmp.data, *ctx->method_state.icmp.length_p, &ctx->method_state.icmp.dest_addr) < 0) {
        pb->send_time = 0;
        return;
    }

    pb->seq = ctx->method_state.icmp.seq;

    ctx->method_state.icmp.seq++;

    return;
}

static probe* icmp_check_reply(struct bx_traceroute_ctx* ctx, int sk, int err, sockaddr_any* from, char* buf, size_t len) {
    int af = ctx->method_state.icmp.dest_addr.sa.sa_family;
    int type;
    uint16_t recv_id, recv_seq;
    probe* pb;

    (void)sk;
    (void)from;

    if (len < sizeof(struct icmphdr))
        return NULL;

    if (af == AF_INET) {
        struct icmp* icmp = (struct icmp*)buf;

        type = icmp->icmp_type;

        recv_id = ntohs(icmp->icmp_id);
        recv_seq = ntohs(icmp->icmp_seq);
    }
    else { /*  AF_INET6   */
        struct icmp6_hdr* icmp6 = (struct icmp6_hdr*)buf;

        type = icmp6->icmp6_type;

        recv_id = ntohs(icmp6->icmp6_id);
        recv_seq = ntohs(icmp6->icmp6_seq);
    }

    if (recv_id != ctx->method_state.icmp.ident)
        return NULL;

    pb = bx_traceroute_probe_by_seq(ctx, recv_seq);
    if (!pb)
        return NULL;

    if (!err) {
        if (!(af == AF_INET && type == ICMP_ECHOREPLY) && !(af == AF_INET6 && type == ICMP6_ECHO_REPLY))
            return NULL;

        pb->final = 1;
    }

    return pb;
}

static void icmp_recv_probe(struct bx_traceroute_ctx* ctx, int sk, int revents) {
    if (!(revents & (POLLIN | POLLERR)))
        return;

    bx_traceroute_recv_reply(ctx, sk, !!(revents & POLLERR), icmp_check_reply);
}

static void icmp_expire_probe(struct bx_traceroute_ctx* ctx, probe* pb) {
    bx_traceroute_probe_done(ctx, pb);
}

static int icmp_parse_option(struct bx_traceroute_ctx* ctx, const char* option) {
    if (!strcmp(option, "raw")) {
        if (ctx->method_state.icmp.dgram)
            return BX_TRACEROUTE_OPTION_EXCLUSIVE;
        ctx->method_state.icmp.raw = 1;
        return 0;
    }
    if (!strcmp(option, "dgram")) {
        if (ctx->method_state.icmp.raw)
            return BX_TRACEROUTE_OPTION_EXCLUSIVE;
        ctx->method_state.icmp.dgram = 1;
        return 0;
    }
    return -1;
}

static void icmp_reset(struct bx_traceroute_ctx* ctx) {
    memset(&ctx->method_state.icmp, 0, sizeof(ctx->method_state.icmp));
    ctx->method_state.icmp.seq = 1;
    ctx->method_state.icmp.icmp_sk = -1;
}

static void icmp_print_options(FILE* stream) {
    fputs(
        "  raw                         Use raw sockets way only. Default is try this way\n"
        "                              first (probably not allowed for unprivileged\n"
        "                              users), then try dgram\n"
        "  dgram                       Use dgram sockets way only. May be not\n"
        "                              implemented by old kernels or restricted by\n"
        "                              sysadmins\n"
        "Only one of these may be specified:\n"
        "    raw | dgram\n"
        , stream);
}

static void icmp_destroy(struct bx_traceroute_ctx* ctx) {
    if (ctx->method_state.icmp.icmp_sk >= 0) {
        bx_traceroute_del_poll(ctx, ctx->method_state.icmp.icmp_sk);
        close(ctx->method_state.icmp.icmp_sk);
        ctx->method_state.icmp.icmp_sk = -1;
    }
    free(ctx->method_state.icmp.data);
    ctx->method_state.icmp.data = NULL;
}

const struct bx_traceroute_method bx_traceroute_method_icmp = {
    .reset = icmp_reset,
    .destroy = icmp_destroy,
    .parse_option = icmp_parse_option,
    .print_options = icmp_print_options,
    .id = BX_TRACEROUTE_METHOD_ICMP,
    .name = "icmp",
    .init = icmp_init,
    .send_probe = icmp_send_probe,
    .recv_probe = icmp_recv_probe,
    .expire_probe = icmp_expire_probe,
};

