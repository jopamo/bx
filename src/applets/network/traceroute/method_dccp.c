#define _DEFAULT_SOURCE

/*
    Copyright (c)  2012		Samuel Jero <sj323707@ohio.edu>
    License:  GPL v2 or any later

    See COPYING for the status of this software.
*/

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <poll.h>
#include <netinet/ip.h>
#include <netinet/ip6.h>
#include <linux/dccp.h>

#include "lib/fd_ops.h"
#include "traceroute.h"

#define DEF_SERVICE_CODE 1885957735

#define DCCP_HEADER_LEN (sizeof(struct dccp_hdr) + sizeof(struct dccp_hdr_ext) + sizeof(struct dccp_hdr_request))

 /*  enough, enough...  */

static int dccp_init(struct bx_traceroute_ctx* ctx, unsigned int port_seq, size_t* packet_len_p) {
    const sockaddr_any* dest = &ctx->destination;
    int af = dest->sa.sa_family;
    sockaddr_any src;
    socklen_t len;
    uint8_t* ptr;
    uint16_t* lenp;

    ctx->method_state.dccp.dest_addr = *dest;
    ctx->method_state.dccp.dest_addr.sin.sin_port = 0; /*  raw sockets can be confused   */

    if (!port_seq)
        port_seq = DEF_DCCP_PORT;
    ctx->method_state.dccp.dest_port = htons(port_seq);

    /*  Create raw socket for DCCP   */
    ctx->method_state.dccp.raw_sk = bx_fd_socket_cloexec(af, SOCK_RAW, IPPROTO_DCCP);
    if (ctx->method_state.dccp.raw_sk < 0)
        bx_traceroute_error_or_perm(ctx, "socket");

    bx_traceroute_tune_socket(ctx, ctx->method_state.dccp.raw_sk, NULL); /*  including bind, if any   */

    if (connect(ctx->method_state.dccp.raw_sk, &ctx->method_state.dccp.dest_addr.sa, sizeof(ctx->method_state.dccp.dest_addr)) < 0)
        bx_traceroute_error(ctx, "connect");

    len = sizeof(src);
    if (getsockname(ctx->method_state.dccp.raw_sk, &src.sa, &len) < 0)
        bx_traceroute_error(ctx, "getsockname");

    if (!bx_traceroute_raw_can_connect()) { /*  work-around for buggy kernels  */
        close(ctx->method_state.dccp.raw_sk);
        ctx->method_state.dccp.raw_sk = bx_fd_socket_cloexec(af, SOCK_RAW, IPPROTO_DCCP);
        if (ctx->method_state.dccp.raw_sk < 0)
            bx_traceroute_error(ctx, "socket");
        bx_traceroute_tune_socket(ctx, ctx->method_state.dccp.raw_sk, NULL);
        /*  but do not connect it...  */
    }

    bx_traceroute_use_recverr(ctx, ctx->method_state.dccp.raw_sk);

    bx_traceroute_add_poll(ctx, ctx->method_state.dccp.raw_sk, POLLIN | POLLERR);

    /*  Now create the sample packet.  */

    /*  For easy checksum computing:
            saddr
            daddr
            length
            protocol
            dccphdr
    */

    ptr = ctx->method_state.dccp.buf;

    if (af == AF_INET) {
        len = sizeof(src.sin.sin_addr);
        memcpy(ptr, &src.sin.sin_addr, len);
        ptr += len;
        memcpy(ptr, &ctx->method_state.dccp.dest_addr.sin.sin_addr, len);
        ptr += len;
    }
    else {
        len = sizeof(src.sin6.sin6_addr);
        memcpy(ptr, &src.sin6.sin6_addr, len);
        ptr += len;
        memcpy(ptr, &ctx->method_state.dccp.dest_addr.sin6.sin6_addr, len);
        ptr += len;
    }

    lenp = (uint16_t*)ptr;
    ptr += sizeof(uint16_t);
    *((uint16_t*)ptr) = htons((uint16_t)IPPROTO_DCCP);
    ptr += sizeof(uint16_t);

    /*  Construct DCCP header   */
    ctx->method_state.dccp.dh = (struct dccp_hdr*)ptr;

    ctx->method_state.dccp.dh->dccph_ccval = 0;
    ctx->method_state.dccp.dh->dccph_checksum = 0;
    ctx->method_state.dccp.dh->dccph_cscov = 0;
    ctx->method_state.dccp.dh->dccph_dport = ctx->method_state.dccp.dest_port;
    ctx->method_state.dccp.dh->dccph_reserved = 0;
    ctx->method_state.dccp.dh->dccph_sport = 0; /*  temporary   */
    ctx->method_state.dccp.dh->dccph_x = 1;
    ctx->method_state.dccp.dh->dccph_type = DCCP_PKT_REQUEST;
    ctx->method_state.dccp.dh->dccph_seq2 = 0; /*  reserved if using 48 bit sequence numbers  */
    /*  high 16 bits of sequence number. Always make 0 for simplicity.  */
    ctx->method_state.dccp.dh->dccph_seq = 0;
    ptr += sizeof(struct dccp_hdr);

    ctx->method_state.dccp.dhe = (struct dccp_hdr_ext*)ptr;
    ctx->method_state.dccp.dhe->dccph_seq_low = 0; /*  temporary   */
    ptr += sizeof(struct dccp_hdr_ext);

    ctx->method_state.dccp.dhr = (struct dccp_hdr_request*)ptr;
    ctx->method_state.dccp.dhr->dccph_req_service = htonl(ctx->method_state.dccp.service_code);
    ptr += sizeof(struct dccp_hdr_request);

    ctx->method_state.dccp.csum_len = ptr - ctx->method_state.dccp.buf;

    if (ctx->method_state.dccp.csum_len > sizeof(ctx->method_state.dccp.buf))
        bx_traceroute_error(ctx, "impossible"); /*  paranoia   */

    len = ptr - (uint8_t*)ctx->method_state.dccp.dh;
    if (len & 0x03)
        bx_traceroute_error(ctx, "impossible"); /*  as >>2 ...  */

    *lenp = htons(len);
    ctx->method_state.dccp.dh->dccph_doff = len >> 2;

    *packet_len_p = len;

    return 0;
}

static void dccp_send_probe(struct bx_traceroute_ctx* ctx, probe* pb, int ttl) {
    int sk;
    int af = ctx->method_state.dccp.dest_addr.sa.sa_family;
    sockaddr_any addr;
    socklen_t len = sizeof(addr);

    /*  To make sure we have chosen a free unused "source port",
       just create, (auto)bind and hold a socket while the port is needed.
    */

    sk = bx_fd_socket_cloexec(af, SOCK_DCCP, IPPROTO_DCCP);
    if (sk < 0)
        bx_traceroute_error(ctx, "socket");

    bx_traceroute_bind_socket(ctx, sk, pb);

    if (getsockname(sk, &addr.sa, &len) < 0)
        bx_traceroute_error(ctx, "getsockname");

    /*  When we reach the target host, it can send us either Reset or Response.
      For Reset all is OK (we and kernel just answer nothing), but
      for Response we should reply with our Close.
        It is well-known "half-open technique", used by port scanners etc.
      This way we do not touch remote applications at all, unlike
      the ordinary connect(2) call.
        As the port-holding socket neither connect() nor listen(),
      it means "no such port yet" for remote ends, and kernel always
      send Reset in such a situation automatically (we have to do nothing).
    */

    ctx->method_state.dccp.dh->dccph_sport = addr.sin.sin_port;

    ctx->method_state.dccp.dhe->dccph_seq_low = bx_traceroute_random_seq(ctx);

    ctx->method_state.dccp.dh->dccph_checksum = 0;
    ctx->method_state.dccp.dh->dccph_checksum = bx_traceroute_in_csum(ctx->method_state.dccp.buf, ctx->method_state.dccp.csum_len);

    if (ttl != ctx->method_state.dccp.last_ttl) {
        bx_traceroute_set_ttl(ctx, ctx->method_state.dccp.raw_sk, ttl);
        ctx->method_state.dccp.last_ttl = ttl;
    }

    pb->send_time = bx_traceroute_get_time();

    if (bx_traceroute_do_send(ctx, ctx->method_state.dccp.raw_sk, ctx->method_state.dccp.dh, ctx->method_state.dccp.dh->dccph_doff << 2, &ctx->method_state.dccp.dest_addr) < 0) {
        close(sk);
        pb->send_time = 0;
        return;
    }

    pb->seq = ctx->method_state.dccp.dh->dccph_sport;

    pb->sk = sk;

    return;
}

static probe* dccp_check_reply(struct bx_traceroute_ctx* ctx, int sk, int err, sockaddr_any* from, char* reply_buf, size_t len) {
    probe* pb;
    struct dccp_hdr* ndh = (struct dccp_hdr*)reply_buf;
    uint16_t sport, dport;

    (void)sk;

    if (len < 8)
        return NULL; /*  too short   */

    if (err) {
        sport = ndh->dccph_sport;
        dport = ndh->dccph_dport;
    }
    else {
        sport = ndh->dccph_dport;
        dport = ndh->dccph_sport;
    }

    if (dport != ctx->method_state.dccp.dest_port)
        return NULL;

    if (!bx_traceroute_equal_addr(&ctx->method_state.dccp.dest_addr, from))
        return NULL;

    pb = bx_traceroute_probe_by_seq(ctx, sport);
    if (!pb)
        return NULL;

    if (!err)
        pb->final = 1;

    return pb;
}

static void dccp_recv_probe(struct bx_traceroute_ctx* ctx, int sk, int revents) {
    if (!(revents & (POLLIN | POLLERR)))
        return;

    bx_traceroute_recv_reply(ctx, sk, !!(revents & POLLERR), dccp_check_reply);
}

static void dccp_expire_probe(struct bx_traceroute_ctx* ctx, probe* pb) {
    bx_traceroute_probe_done(ctx, pb);
}

static int dccp_parse_option(struct bx_traceroute_ctx* ctx, const char* option) {
    const char* value;
    if (!bx_traceroute_option_is(option, "service", true, &value))
        return BX_TRACEROUTE_OPTION_UNKNOWN;
    if (!value)
        return BX_TRACEROUTE_OPTION_NEEDS_ARGUMENT;
    return bx_traceroute_parse_uint(value, &ctx->method_state.dccp.service_code)
               ? BX_TRACEROUTE_OPTION_OK : BX_TRACEROUTE_OPTION_BAD_ARGUMENT;
}

static void dccp_reset(struct bx_traceroute_ctx* ctx) {
    memset(&ctx->method_state.dccp, 0, sizeof(ctx->method_state.dccp));
    ctx->method_state.dccp.raw_sk = -1;
    ctx->method_state.dccp.service_code = DEF_SERVICE_CODE;
}

static void dccp_print_options(FILE* stream) {
    fputs(
        "  service=NUM                 Set DCCP service code to NUM (default is\n"
        "                              1885957735)\n"
        , stream);
}

static void dccp_destroy(struct bx_traceroute_ctx* ctx) {
    if (ctx->method_state.dccp.raw_sk >= 0) {
        bx_traceroute_del_poll(ctx, ctx->method_state.dccp.raw_sk);
        close(ctx->method_state.dccp.raw_sk);
        ctx->method_state.dccp.raw_sk = -1;
    }
}

const struct bx_traceroute_method bx_traceroute_method_dccp = {
    .reset = dccp_reset,
    .destroy = dccp_destroy,
    .parse_option = dccp_parse_option,
    .print_options = dccp_print_options,
    .id = BX_TRACEROUTE_METHOD_DCCP,
    .name = "dccp",
    .init = dccp_init,
    .send_probe = dccp_send_probe,
    .recv_probe = dccp_recv_probe,
    .expire_probe = dccp_expire_probe,
};

