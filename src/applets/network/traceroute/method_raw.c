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
#include <netdb.h>

#include "lib/fd_ops.h"
#include "traceroute.h"

static int raw_init(struct bx_traceroute_ctx* ctx, unsigned int port_seq, size_t* packet_len_p) {
    const sockaddr_any* dest = &ctx->destination;
    size_t i;
    int af = dest->sa.sa_family;

    ctx->method_state.raw.dest_addr = *dest;
    ctx->method_state.raw.dest_addr.sin.sin_port = 0;

    if (port_seq)
        ctx->method_state.raw.protocol = port_seq;

    ctx->method_state.raw.length_p = packet_len_p;

    if (*ctx->method_state.raw.length_p && !(ctx->method_state.raw.data = malloc(*ctx->method_state.raw.length_p)))
        bx_traceroute_error(ctx, "malloc");

    for (i = 0; i < *ctx->method_state.raw.length_p; i++)
        ctx->method_state.raw.data[i] = 0x40 + (i & 0x3f);

    ctx->method_state.raw.raw_sk = bx_fd_socket_cloexec(af, SOCK_RAW, ctx->method_state.raw.protocol);
    if (ctx->method_state.raw.raw_sk < 0)
        bx_traceroute_error_or_perm(ctx, "socket");

    bx_traceroute_tune_socket(ctx, ctx->method_state.raw.raw_sk, NULL);

    /*  Don't want to catch packets from another hosts   */
    if (bx_traceroute_raw_can_connect() && connect(ctx->method_state.raw.raw_sk, &ctx->method_state.raw.dest_addr.sa, sizeof(ctx->method_state.raw.dest_addr)) < 0)
        bx_traceroute_error(ctx, "connect");

    bx_traceroute_use_recverr(ctx, ctx->method_state.raw.raw_sk);

    bx_traceroute_add_poll(ctx, ctx->method_state.raw.raw_sk, POLLIN | POLLERR);

    return 0;
}

static void raw_send_probe(struct bx_traceroute_ctx* ctx, probe* pb, int ttl) {
    if (ttl != ctx->method_state.raw.last_ttl) {
        bx_traceroute_set_ttl(ctx, ctx->method_state.raw.raw_sk, ttl);

        ctx->method_state.raw.last_ttl = ttl;
    }

    pb->send_time = bx_traceroute_get_time();

    if (bx_traceroute_do_send(ctx, ctx->method_state.raw.raw_sk, ctx->method_state.raw.data, *ctx->method_state.raw.length_p, &ctx->method_state.raw.dest_addr) < 0) {
        pb->send_time = 0;
        return;
    }

    pb->seq = ++ctx->method_state.raw.seq;

    return;
}

static probe* raw_check_reply(struct bx_traceroute_ctx* ctx, int sk, int err, sockaddr_any* from, char* buf, size_t len) {
    probe* pb;

    (void)sk;
    (void)buf;
    (void)len;

    if (!bx_traceroute_equal_addr(&ctx->method_state.raw.dest_addr, from))
        return NULL;

    pb = bx_traceroute_probe_by_seq(ctx, ctx->method_state.raw.seq);
    if (!pb)
        return NULL;

    if (!err)
        pb->final = 1;

    return pb;
}

static void raw_recv_probe(struct bx_traceroute_ctx* ctx, int sk, int revents) {
    if (!(revents & (POLLIN | POLLERR)))
        return;

    bx_traceroute_recv_reply(ctx, sk, !!(revents & POLLERR), raw_check_reply);
}

static void raw_expire_probe(struct bx_traceroute_ctx* ctx, probe* pb) {
    bx_traceroute_probe_done(ctx, pb);
}

static int raw_parse_option(struct bx_traceroute_ctx* ctx, const char* option) {
    const char* value;
    if (!bx_traceroute_option_is(option, "protocol", true, &value))
        return BX_TRACEROUTE_OPTION_UNKNOWN;
    if (!value)
        return BX_TRACEROUTE_OPTION_NEEDS_ARGUMENT;
    char* end;
    ctx->method_state.raw.protocol = (int)strtoul(value, &end, 0);
    if (end == value) {
        struct protoent* protocol = getprotobyname(value);
        if (!protocol)
            return BX_TRACEROUTE_OPTION_BAD_ARGUMENT;
        ctx->method_state.raw.protocol = protocol->p_proto;
    }
    return 0;
}

static void raw_reset(struct bx_traceroute_ctx* ctx) {
    memset(&ctx->method_state.raw, 0, sizeof(ctx->method_state.raw));
    ctx->method_state.raw.protocol = DEF_RAW_PROT;
    ctx->method_state.raw.raw_sk = -1;
}

static void raw_print_options(FILE* stream) {
    fputs(
        "  protocol=PROT               Use protocol PROT (default is 253)\n"
        , stream);
}

static void raw_destroy(struct bx_traceroute_ctx* ctx) {
    if (ctx->method_state.raw.raw_sk >= 0) {
        bx_traceroute_del_poll(ctx, ctx->method_state.raw.raw_sk);
        close(ctx->method_state.raw.raw_sk);
        ctx->method_state.raw.raw_sk = -1;
    }
    free(ctx->method_state.raw.data);
    ctx->method_state.raw.data = NULL;
}

const struct bx_traceroute_method bx_traceroute_method_raw = {
    .reset = raw_reset,
    .destroy = raw_destroy,
    .parse_option = raw_parse_option,
    .print_options = raw_print_options,
    .id = BX_TRACEROUTE_METHOD_RAW,
    .name = "raw",
    .init = raw_init,
    .send_probe = raw_send_probe,
    .recv_probe = raw_recv_probe,
    .expire_probe = raw_expire_probe,
    .one_per_time = 1,
};

