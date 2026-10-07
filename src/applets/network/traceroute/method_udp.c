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
#include <netinet/in.h>
#include <netinet/udp.h>

#include "lib/fd_ops.h"
#include "traceroute.h"

#ifndef IPPROTO_UDPLITE
#define IPPROTO_UDPLITE 136
#endif

#ifndef UDPLITE_SEND_CSCOV
#define UDPLITE_SEND_CSCOV 10
#define UDPLITE_RECV_CSCOV 11
#endif

static void fill_data(struct bx_traceroute_ctx* ctx, size_t* packet_len_p) {
    size_t i;

    ctx->method_state.udp.length_p = packet_len_p;

    if (*ctx->method_state.udp.length_p && !(ctx->method_state.udp.data = malloc(*ctx->method_state.udp.length_p)))
        bx_traceroute_error(ctx, "malloc");

    for (i = 0; i < *ctx->method_state.udp.length_p; i++)
        ctx->method_state.udp.data[i] = 0x40 + (i & 0x3f);

    return;
}

static int udp_default_init(struct bx_traceroute_ctx* ctx, unsigned int port_seq, size_t* packet_len_p) {
    const sockaddr_any* dest = &ctx->destination;
    ctx->method_state.udp.curr_port = port_seq ? port_seq : DEF_START_PORT;

    ctx->method_state.udp.dest_addr = *dest;
    ctx->method_state.udp.dest_addr.sin.sin_port = htons(ctx->method_state.udp.curr_port);

    fill_data(ctx, packet_len_p);

    return 0;
}

static int udp_init(struct bx_traceroute_ctx* ctx, unsigned int port_seq, size_t* packet_len_p) {
    const sockaddr_any* dest = &ctx->destination;
    ctx->method_state.udp.dest_addr = *dest;

    if (!port_seq)
        port_seq = DEF_UDP_PORT;
    ctx->method_state.udp.dest_addr.sin.sin_port = htons((uint16_t)port_seq);

    fill_data(ctx, packet_len_p);

    return 0;
}

#define MIN_COVERAGE (sizeof(struct udphdr))

static void set_coverage(struct bx_traceroute_ctx* ctx, int sk) {
    int val = MIN_COVERAGE;

    if (setsockopt(sk, IPPROTO_UDPLITE, UDPLITE_SEND_CSCOV, &ctx->method_state.udp.coverage, sizeof(ctx->method_state.udp.coverage)) < 0)
        bx_traceroute_error(ctx, "UDPLITE_SEND_CSCOV");

    if (setsockopt(sk, IPPROTO_UDPLITE, UDPLITE_RECV_CSCOV, &val, sizeof(val)) < 0)
        bx_traceroute_error(ctx, "UDPLITE_RECV_CSCOV");
}

static int udplite_init(struct bx_traceroute_ctx* ctx, unsigned int port_seq, size_t* packet_len_p) {
    const sockaddr_any* dest = &ctx->destination;
    ctx->method_state.udp.dest_addr = *dest;

    if (!port_seq)
        port_seq = DEF_UDP_PORT; /*  XXX: Hmmm...   */
    ctx->method_state.udp.dest_addr.sin.sin_port = htons((uint16_t)port_seq);

    ctx->method_state.udp.protocol = IPPROTO_UDPLITE;

    if (!ctx->method_state.udp.coverage)
        ctx->method_state.udp.coverage = MIN_COVERAGE;

    fill_data(ctx, packet_len_p);

    return 0;
}

static void udp_send_probe(struct bx_traceroute_ctx* ctx, probe* pb, int ttl) {
    int sk;
    int af = ctx->method_state.udp.dest_addr.sa.sa_family;

    sk = bx_fd_socket_cloexec(af, SOCK_DGRAM, ctx->method_state.udp.protocol);
    if (sk < 0)
        bx_traceroute_error(ctx, "socket");

    bx_traceroute_tune_socket(ctx, sk, pb); /*  common stuff   */

    if (ctx->method_state.udp.coverage)
        set_coverage(ctx, sk); /*  udplite case   */

    bx_traceroute_set_ttl(ctx, sk, ttl);

    if (connect(sk, &ctx->method_state.udp.dest_addr.sa, sizeof(ctx->method_state.udp.dest_addr)) < 0)
        bx_traceroute_error(ctx, "connect");

    bx_traceroute_use_recverr(ctx, sk);

    pb->send_time = bx_traceroute_get_time();

    if (bx_traceroute_do_send(ctx, sk, ctx->method_state.udp.data, *ctx->method_state.udp.length_p, NULL) < 0) {
        close(sk);
        pb->send_time = 0;
        return;
    }

    pb->sk = sk;

    bx_traceroute_add_poll(ctx, sk, POLLIN | POLLERR);

    pb->seq = ctx->method_state.udp.dest_addr.sin.sin_port;

    if (ctx->method_state.udp.curr_port) { /*  traditional udp method   */
        ctx->method_state.udp.curr_port++;
        ctx->method_state.udp.dest_addr.sin.sin_port = htons(ctx->method_state.udp.curr_port); /* both ipv4 and ipv6 */
    }

    return;
}

static probe* udp_check_reply(struct bx_traceroute_ctx* ctx, int sk, int err, sockaddr_any* from, char* buf, size_t len) {
    probe* pb;

    (void)buf;
    (void)len;

    pb = bx_traceroute_probe_by_sk(ctx, sk);
    if (!pb)
        return NULL;

    if (pb->seq != from->sin.sin_port)
        return NULL;

    if (!err)
        pb->final = 1;

    return pb;
}

static void udp_recv_probe(struct bx_traceroute_ctx* ctx, int sk, int revents) {
    if (!(revents & (POLLIN | POLLERR)))
        return;

    bx_traceroute_recv_reply(ctx, sk, !!(revents & POLLERR), udp_check_reply);
}

static void udp_expire_probe(struct bx_traceroute_ctx* ctx, probe* pb) {
    bx_traceroute_probe_done(ctx, pb);
}

/* The UDP methods share probe handling. */

static int udplite_parse_option(struct bx_traceroute_ctx* ctx, const char* option) {
    const char* value;
    if (!bx_traceroute_option_is(option, "coverage", true, &value))
        return BX_TRACEROUTE_OPTION_UNKNOWN;
    if (!value)
        return BX_TRACEROUTE_OPTION_NEEDS_ARGUMENT;
    return bx_traceroute_parse_uint(value, &ctx->method_state.udp.coverage)
               ? BX_TRACEROUTE_OPTION_OK : BX_TRACEROUTE_OPTION_BAD_ARGUMENT;
}

static void udp_reset(struct bx_traceroute_ctx* ctx) {
    memset(&ctx->method_state.udp, 0, sizeof(ctx->method_state.udp));
    ctx->method_state.udp.protocol = IPPROTO_UDP;
}

static void udp_print_options(FILE* stream) {
    fputs(
        "  coverage=NUM                Set udplite send coverage to NUM (default is\n"
        "                              (sizeof(struct udphdr)))\n"
        , stream);
}

static void udp_destroy(struct bx_traceroute_ctx* ctx) {
    free(ctx->method_state.udp.data);
    ctx->method_state.udp.data = NULL;
}

const struct bx_traceroute_method bx_traceroute_method_default = {
    .reset = udp_reset,
    .destroy = udp_destroy,
    .id = BX_TRACEROUTE_METHOD_DEFAULT,
    .name = "default",
    .init = udp_default_init,
    .send_probe = udp_send_probe,
    .recv_probe = udp_recv_probe,
    .expire_probe = udp_expire_probe,
    .header_len = sizeof(struct udphdr),
};

const struct bx_traceroute_method bx_traceroute_method_udp = {
    .reset = udp_reset,
    .destroy = udp_destroy,
    .id = BX_TRACEROUTE_METHOD_UDP,
    .name = "udp",
    .init = udp_init,
    .send_probe = udp_send_probe,
    .recv_probe = udp_recv_probe,
    .expire_probe = udp_expire_probe,
    .header_len = sizeof(struct udphdr),
};

const struct bx_traceroute_method bx_traceroute_method_udplite = {
    .reset = udp_reset,
    .destroy = udp_destroy,
    .parse_option = udplite_parse_option,
    .print_options = udp_print_options,
    .id = BX_TRACEROUTE_METHOD_UDPLITE,
    .name = "udplite",
    .init = udplite_init,
    .send_probe = udp_send_probe,
    .recv_probe = udp_recv_probe,
    .expire_probe = udp_expire_probe,
    .header_len = sizeof(struct udphdr),
};

