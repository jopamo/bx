/*
    Copyright (c)  2006, 2007		Dmitry Butskoy
                                        <dmitry@butskoy.name>
    License:  GPL v2 or any later

    See COPYING for the status of this software.
*/

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <poll.h>
#include <sched.h>
#ifndef CLONE_NEWNET
#define CLONE_NEWNET 0x40000000
#endif
#include <netinet/icmp6.h>
#include <netinet/ip_icmp.h>
#include <netinet/in.h>
#include <netinet/ip6.h>
#include <netdb.h>
#include <errno.h>
#include <locale.h>
#include <strings.h>
#include <time.h>
#include <linux/types.h>
#include <linux/errqueue.h>
#include <linux/net_tstamp.h>

/*  XXX: Remove this when things will be defined properly in netinet/ ...  */
#include "flowlabel.h"

#include "dispatch/applets.h"
#include "lib/cli_common.h"
#include "lib/random_bytes.h"
#include "lib/fd_ops.h"
#include "lib/sockaddr_format.h"
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

#ifndef AI_IDN
#define AI_IDN 0
#endif

#ifndef NI_IDN
#define NI_IDN 0
#endif

#define ttl2hops(X) (((X) <= 64 ? 65 : ((X) <= 128 ? 129 : 256)) - (X))

static void ex_error(struct bx_traceroute_ctx* ctx, const char* format, ...) {
    va_list ap;

    va_start(ap, format);
    bx_vdiag(&ctx->diag, format, ap);
    va_end(ap);

    ctx->diag.exit_status = 2;
    bx_traceroute_ctx_destroy(ctx);
    exit(2);
}

void bx_traceroute_error(struct bx_traceroute_ctx* ctx, const char* str) {
    int saved_errno = errno;
    fprintf(stderr, "\n");

    bx_diag(&ctx->diag, "%s: %s", str, bx_strerror(saved_errno));

    bx_traceroute_ctx_destroy(ctx);
    exit(1);
}

void bx_traceroute_error_or_perm(struct bx_traceroute_ctx* ctx, const char* str) {
    if (errno == EPERM)
        fprintf(stderr,
                "You do not have enough privileges to use "
                "this traceroute method.");
    bx_traceroute_error(ctx, str);
}

void bx_traceroute_put_err(probe* pb, const char* format, ...) {
    va_list ap;
    char* curr = pb->err_str;
    char* end = pb->err_str + sizeof(pb->err_str) - 1;

    /*  It can already contain something when `--mtu' or `-T -O mss'   */
    while (curr < end && *curr)
        curr++;

    va_start(ap, format);
    vsnprintf(curr, end - curr, format, ap);
    va_end(ap);
}

int bx_traceroute_getaddr(struct bx_traceroute_ctx* ctx, const char* name, sockaddr_any* addr) {
    int ret;
    struct addrinfo hints, *ai, *res = NULL;

    if (!name || !addr) {
        fprintf(stderr, "Invalid arguments\n");
        return -1;
    }

    // Clear out hints and set defaults
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = ctx->options.address_family;
    hints.ai_flags = AI_IDN;  // For International Domain Names

    // Get address info
    ret = getaddrinfo(name, NULL, &hints, &res);
    if (ret) {
        fprintf(stderr, "%s: %s\n", name, gai_strerror(ret));
        return -1;
    }

    // Find the first matching address family (or use the first available)
    for (ai = res; ai; ai = ai->ai_next) {
        if (!ctx->options.address_family || ai->ai_family == ctx->options.address_family) {
            break;
        }
    }

    // If no matching address family is found, just use the first available
    if (!ai) {
        ai = res;  // anything, as a fallback
    }

    // Check if the address length is valid
    if (ai->ai_addrlen > sizeof(*addr)) {
        freeaddrinfo(res);  // Cleanup before returning
        return -1;          // Paranoia check
    }

    // Copy the address into the provided sockaddr_any structure
    memcpy(addr, ai->ai_addr, ai->ai_addrlen);

    freeaddrinfo(res);  // Cleanup after processing

    // If the address is IPv6 and is a mapped IPv4 address, handle it as IPv4
    if (addr->sa.sa_family == AF_INET6 && IN6_IS_ADDR_V4MAPPED(&addr->sin6.sin6_addr)) {
        if (ctx->options.address_family == AF_INET6) {
            return -1;  // If IPv6 is requested, return an error for v4mapped addresses
        }

        // Convert the v4mapped address to IPv4
        addr->sa.sa_family = AF_INET;
        addr->sin.sin_addr.s_addr =
            addr->sin6.sin6_addr.s6_addr32[3];  // Extract IPv4 address from the v4mapped address
    }

    return 0;  // Success
}

static void make_fd_used(struct bx_traceroute_ctx* ctx, int fd) {
    int nfd;

    if (fcntl(fd, F_GETFL) != -1)
        return;

    if (errno != EBADF)
        bx_traceroute_error(ctx, "fcntl F_GETFL");

    nfd = bx_fd_open_cloexec("/dev/null", O_RDONLY, 0);
    if (nfd < 0)
        bx_traceroute_error(ctx, "open /dev/null");

    if (nfd != fd) {
        bx_fd_dup2_exact(nfd, fd);
        close(nfd);
    }

    return;
}

const char* bx_traceroute_addr2str(struct bx_traceroute_ctx* ctx, const sockaddr_any* addr) {
    (void)bx_sockaddr_format_numeric(
        &addr->sa,
        sizeof(*addr),
        ctx->addr2str_buf,
        sizeof(ctx->addr2str_buf),
        NULL,
        0);

    return ctx->addr2str_buf;
}

/*	IP  options  stuff	    */

static void init_ip_options(struct bx_traceroute_ctx* ctx) {
    sockaddr_any* gates;
    int i, max;

    if (!ctx->num_gateways)
        return;

    /* Check for TYPE, ADDR, ADDR... form for IPv6 gateways */
    if (ctx->options.address_family == AF_INET6 && ctx->num_gateways > 1 && ctx->gateways[0]) {
        char* q;
        unsigned int value = strtoul(ctx->gateways[0], &q, 0);

        if (!*q) {
            ctx->options.ipv6_rthdr_type = value;
            free(ctx->gateways[0]);
            ctx->num_gateways--;
            for (i = 0; i < ctx->num_gateways; i++)
                ctx->gateways[i] = ctx->gateways[i + 1];
        }
    }

    max = (ctx->options.address_family == AF_INET) ? MAX_GATEWAYS_4 : MAX_GATEWAYS_6;
    if (ctx->num_gateways > max)
        ex_error(ctx, "Too many gateways specified. No more than %d", max);

    // Dynamically allocate memory for gates
    gates = malloc(ctx->num_gateways * sizeof(*gates));
    if (!gates)
        bx_traceroute_error(ctx, "malloc");

    for (i = 0; i < ctx->num_gateways; i++) {
        if (!ctx->gateways[i])
            bx_traceroute_error(ctx, "Invalid gateway address");

        if (bx_traceroute_getaddr(ctx, ctx->gateways[i], &gates[i]) < 0) {
            free(gates);
            ex_error(ctx, "Failed to resolve gateway address");  // Error already reported by getaddr
        }

        if (gates[i].sa.sa_family != ctx->options.address_family) {
            free(gates);
            ex_error(ctx, "IP version mismatch in gateway addresses");
        }

        free(ctx->gateways[i]);  // Free the original gateway string
        ctx->gateways[i] = NULL;
    }

    free(ctx->gateways);   // Free the gateways array itself
    ctx->gateways = NULL;  // Set to NULL to avoid dangling pointers

    if (ctx->options.address_family == AF_INET) {
        struct in_addr* in;

        // Allocate space for the routing buffer
        ctx->rtbuf_len = 4 + (ctx->num_gateways + 1) * sizeof(*in);
        ctx->rtbuf = malloc(ctx->rtbuf_len);
        if (!ctx->rtbuf)
            bx_traceroute_error(ctx, "malloc");

        in = (struct in_addr*)&ctx->rtbuf[4];
        for (i = 0; i < ctx->num_gateways; i++)
            memcpy(&in[i], &gates[i].sin.sin_addr, sizeof(*in));

        // Final hop (destination address)
        memcpy(&in[i], &ctx->destination.sin.sin_addr, sizeof(*in));
        i++;

        ctx->rtbuf[0] = IPOPT_NOP;
        ctx->rtbuf[1] = IPOPT_LSRR;
        ctx->rtbuf[2] = (i * sizeof(*in)) + 3;
        ctx->rtbuf[3] = IPOPT_MINOFF;
    }
    else if (ctx->options.address_family == AF_INET6) {
        struct in6_addr* in6;
        struct ip6_rthdr* rth;

        // IPV6_RTHDR_TYPE_0 length is 8
        ctx->rtbuf_len = 8 + ctx->num_gateways * sizeof(*in6);
        ctx->rtbuf = malloc(ctx->rtbuf_len);
        if (!ctx->rtbuf)
            bx_traceroute_error(ctx, "malloc");

        rth = (struct ip6_rthdr*)ctx->rtbuf;
        rth->ip6r_nxt = 0;
        rth->ip6r_len = 2 * ctx->num_gateways;
        rth->ip6r_type = ctx->options.ipv6_rthdr_type;
        rth->ip6r_segleft = ctx->num_gateways;

        *((uint32_t*)(rth + 1)) = 0;  // Padding for the routing header

        in6 = (struct in6_addr*)(ctx->rtbuf + 8);
        for (i = 0; i < ctx->num_gateways; i++)
            memcpy(&in6[i], &gates[i].sin6.sin6_addr, sizeof(*in6));
    }

    // Clean up allocated memory for gateways array
    free(gates);
}

static void do_it(struct bx_traceroute_ctx* ctx);

static int bx_traceroute_run(struct bx_traceroute_ctx* ctx) {
    if (ctx->options.ecmp > ctx->options.probes_per_hop)
        ctx->options.probes_per_hop = ctx->options.ecmp;

    if (ctx->options.netns) {
        int fd = bx_fd_open_cloexec(ctx->options.netns, O_RDONLY, 0);
        if (fd < 0) {
            fprintf(stderr, "open %s: %s\n", ctx->options.netns, strerror(errno));
            bx_traceroute_ctx_destroy(ctx);
            exit(2);
        }
        if (setns(fd, CLONE_NEWNET) < 0) {
            int saved_errno = errno;
            close(fd);
            fprintf(stderr, "setns %s: %s\n", ctx->options.netns, strerror(saved_errno));
            bx_traceroute_ctx_destroy(ctx);
            exit(2);
        }
        close(fd);
    }

    ctx->method = bx_traceroute_method_by_id(ctx->options.method);
    if (ctx->method)
        ctx->method->reset(ctx);
    if (!ctx->method)
        ex_error(ctx, "Unknown traceroute method %s", ctx->options.method_name);

    if (!ctx->options.first_hop || ctx->options.first_hop > ctx->options.max_hops)
        ex_error(ctx, "first hop out of range");
    if (ctx->options.max_hops > MAX_HOPS)
        ex_error(ctx, "max hops cannot be more than " _TEXT(MAX_HOPS));
    if (!ctx->options.probes_per_hop || ctx->options.probes_per_hop > MAX_PROBES)
        ex_error(ctx, "no more than " _TEXT(MAX_PROBES) " probes per hop");
    if (ctx->options.simultaneous_probes > MAX_SIM_PROBES)
        ex_error(ctx, "sim-queries cannot be more than " _TEXT(MAX_SIM_PROBES));
    if (ctx->options.wait_secs < 0 || ctx->options.here_factor < 0 || ctx->options.near_factor < 0)
        ex_error(ctx, "bad wait specifications `%g,%g,%g' used", ctx->options.wait_secs, ctx->options.here_factor, ctx->options.near_factor);
    if (ctx->options.packet_len > MAX_PACKET_LEN)
        ex_error(ctx, "too big packetlen %d specified", ctx->options.packet_len);
    if (ctx->source.sa.sa_family && ctx->source.sa.sa_family != ctx->options.address_family)
        ex_error(ctx, "IP version mismatch in addresses specified");
    if (ctx->options.send_secs < 0)
        ex_error(ctx, "bad sendtime `%g' specified", ctx->options.send_secs);
    if (ctx->options.send_secs >= 10) { /*  it is milliseconds   */
        double send_seconds = 0.0;

        if (!bx_time_milliseconds_double_to_seconds_double(ctx->options.send_secs, &send_seconds))
            ex_error(ctx, "bad sendtime `%g' specified", ctx->options.send_secs);
        ctx->options.send_secs = send_seconds;
    }

    if (ctx->options.address_family == AF_INET6 && (ctx->options.tos || ctx->options.flow_label))
        ctx->destination.sin6.sin6_flowinfo = htonl(((ctx->options.tos & 0xff) << 20) | (ctx->options.flow_label & 0x000fffff));

    if (ctx->options.source_port) {
        ctx->source.sin.sin_port = htons((uint16_t)ctx->options.source_port);
        ctx->source.sa.sa_family = ctx->options.address_family;
    }

    if (ctx->options.source_port || ctx->method->one_per_time) {
        ctx->options.simultaneous_probes = 1;
        ctx->options.here_factor = ctx->options.near_factor = 0;
    }

    /*  make sure we don't std{in,out,err} to open sockets  */
    make_fd_used(ctx, 0);
    make_fd_used(ctx, 1);
    make_fd_used(ctx, 2);

    init_ip_options(ctx);

    ctx->header_len = (ctx->options.address_family == AF_INET ? sizeof(struct iphdr) : sizeof(struct ip6_hdr)) + ctx->rtbuf_len + ctx->method->header_len;

    if (ctx->options.mtu_discovery) {
        ctx->options.dont_fragment = 1;
        ctx->options.simultaneous_probes = 1;
        if (ctx->options.packet_len < 0)
            ctx->options.packet_len = MAX_PACKET_LEN;
    }

    if (ctx->options.packet_len < 0) {
        if (DEF_DATA_LEN >= ctx->method->header_len)
            ctx->data_len = DEF_DATA_LEN - ctx->method->header_len;
    }
    else {
        if (ctx->options.packet_len >= 0 && (size_t)ctx->options.packet_len >= ctx->header_len)
            ctx->data_len = (size_t)ctx->options.packet_len - ctx->header_len;
    }

    ctx->probe_count = ctx->options.max_hops * ctx->options.probes_per_hop;
    ctx->probes = calloc(ctx->probe_count, sizeof(*ctx->probes));
    if (!ctx->probes)
        bx_traceroute_error(ctx, "calloc");

    if (!bx_traceroute_parse_method_options(ctx))
        return ctx->diag.exit_status;

    if (ctx->method->init(ctx, ctx->options.destination_port, &ctx->data_len) < 0)
        ex_error(ctx, "trace method's init failed");

    if (ctx->options.bpf_mode != 2) {
        const char* bpf_objs[] = {"probe.bpf.o", "bpf/probe.bpf.o", "/usr/share/traceroute/probe.bpf.o", NULL};
        int i;
        for (i = 0; bpf_objs[i]; i++) {
            if (access(bpf_objs[i], R_OK) == 0) {
                if (bx_traceroute_bpf_init(bpf_objs[i]) == 0) {
                    if (ctx->options.debug)
                        fprintf(stderr, "BPF initialized using %s\n", bpf_objs[i]);
                    break;
                }
            }
        }
        if (!bpf_objs[i] && ctx->options.bpf_mode == 1)
            ex_error(ctx, "BPF initialization failed");
    }

    if (ctx->options.interface) {
        bx_traceroute_xdp_init(ctx->options.interface, "xdp_probe.bpf.o");
    }

    do_it(ctx);

    return 0;
}

/*	PRINT  STUFF	    */

static void print_header(struct bx_traceroute_ctx* ctx) {
    /*  Note, without ending new-line!  */
    printf("traceroute to %s (%s), %u hops max, %zu byte packets", ctx->options.dst_name, bx_traceroute_addr2str(ctx, &ctx->destination), ctx->options.max_hops,
           ctx->header_len + ctx->data_len);
    fflush(stdout);
}

static void print_addr(struct bx_traceroute_ctx* ctx, sockaddr_any* res) {
    const char* str;

    if (!res->sa.sa_family)
        return;

    str = bx_traceroute_addr2str(ctx, res);

    if (ctx->options.noresolve)
        printf(" %s", str);
    else {
        char buf[1024];

        buf[0] = '\0';
        getnameinfo(&res->sa, sizeof(*res), buf, sizeof(buf), 0, 0, NI_IDN);
        printf(" %s (%s)", buf[0] ? buf : str, str);
    }

    if (ctx->options.as_lookups)
        printf(" [%s]", bx_traceroute_get_as_path(ctx, str));
}

static void print_probe(struct bx_traceroute_ctx* ctx, probe* pb) {
    unsigned int idx = (pb - ctx->probes);
    unsigned int ttl = idx / ctx->options.probes_per_hop + 1;
    unsigned int np = idx % ctx->options.probes_per_hop;

    if (np == 0)
        printf("\n%2u ", ttl);

    if (!pb->res.sa.sa_family)
        printf(" *");
    else {
        int prn = !np; /*  print if the first...  */

        if (np) { /*  ...and if differs with previous   */
            probe* p;

            /*  skip expired   */
            for (p = pb - 1; np && !p->res.sa.sa_family; p--, np--)
                ;

            if (!np || !bx_traceroute_equal_addr(&p->res, &pb->res) ||
                (p->ext != pb->ext && !(p->ext && pb->ext && !strcmp(p->ext, pb->ext))) ||
                (ctx->options.backward && p->recv_ttl != pb->recv_ttl))
                prn = 1;
        }

        if (prn) {
            print_addr(ctx, &pb->res);

            if (pb->ext)
                printf(" <%s>", pb->ext);

            if (ctx->options.backward && pb->recv_ttl) {
                int hops = ttl2hops(pb->recv_ttl);
                if (hops != (int)ttl)
                    printf(" '-%d'", hops);
            }
        }
    }

    if (pb->recv_time) {
        double diff = pb->recv_time - pb->send_time;
        double diff_ms = 0.0;

        if (bx_time_seconds_to_milliseconds_double(diff, &diff_ms)) {
            printf("  %.3f ms", diff_ms);
        }
    }

    if (pb->err_str[0])
        printf(" %s", pb->err_str);

    fflush(stdout);

    return;
}

static void print_end(void) {
    bx_traceroute_bpf_print_histograms();
    printf("\n");
}

void bx_traceroute_report_header(struct bx_traceroute_ctx* ctx, const char* report_dst_name,
                      const sockaddr_any* report_dst_addr,
                      unsigned int report_max_hops,
                      size_t report_packet_len) {
    (void)ctx;
    if (ctx->options.jsonl)
        bx_traceroute_export_jsonl_header(ctx, report_dst_name, report_dst_addr, report_max_hops, report_packet_len);
    if (!ctx->options.quiet)
        print_header(ctx);
}

void bx_traceroute_report_probe(struct bx_traceroute_ctx* ctx, probe* pb) {
    if (ctx->options.jsonl)
        bx_traceroute_export_jsonl_probe(ctx, pb);
    if (!ctx->options.quiet)
        print_probe(ctx, pb);
}

void bx_traceroute_report_end(struct bx_traceroute_ctx* ctx) {
    if (ctx->options.jsonl)
        bx_traceroute_export_jsonl_end();
    if (!ctx->options.quiet)
        print_end();
}

/*	Compute  timeout  stuff		*/

static double get_timeout(struct bx_traceroute_ctx* ctx, probe* pb) {
    double value;

    if (ctx->options.here_factor) {
        /*  check for already replied from the same hop   */
        unsigned int i;
        int idx = (pb - ctx->probes);
        probe* p = &ctx->probes[idx - (idx % ctx->options.probes_per_hop)];

        for (i = 0; i < ctx->options.probes_per_hop; i++, p++) {
            /*   `p == pb' skipped since  !pb->done   */

            if (p->done && (value = p->recv_time - p->send_time) > 0) {
                value += DEF_WAIT_PREC;
                value *= ctx->options.here_factor;
                return value < ctx->options.wait_secs ? value : ctx->options.wait_secs;
            }
        }
    }

    if (ctx->options.near_factor) {
        /*  check forward for already replied   */
        probe *p, *endp = ctx->probes + ctx->probe_count;

        for (p = pb + 1; p < endp && p->send_time; p++) {
            if (p->done && (value = p->recv_time - p->send_time) > 0) {
                value += DEF_WAIT_PREC;
                value *= ctx->options.near_factor;
                return value < ctx->options.wait_secs ? value : ctx->options.wait_secs;
            }
        }
    }

    return ctx->options.wait_secs;
}

/*	Check  expiration  stuff	*/

static void check_expired(probe* pb) {
    (void)pb;

    /*
     * Correctness beats cleverness: never “guess” a hop; always report
     * “unknown/no reply” explicitly.
     * We no longer try to pull back "final" responses from later probes or hops.
     */
    return;
}

probe* bx_traceroute_probe_by_seq(struct bx_traceroute_ctx* ctx, int seq) {
    unsigned int n;

    if (seq <= 0)
        return NULL;

    for (n = 0; n < ctx->probe_count; n++) {
        if (ctx->probes[n].seq == seq)
            return &ctx->probes[n];
    }

    return NULL;
}

probe* bx_traceroute_probe_by_sk(struct bx_traceroute_ctx* ctx, int sk) {
    unsigned int n;

    if (sk <= 0)
        return NULL;

    for (n = 0; n < ctx->probe_count; n++) {
        if (ctx->probes[n].sk == sk)
            return &ctx->probes[n];
    }

    return NULL;
}

static void poll_callback(struct bx_traceroute_ctx* ctx, int fd, int revents) {
    bx_traceroute_bpf_poll(fd, revents);
    bx_traceroute_xdp_poll(fd, revents);
    ctx->method->recv_probe(ctx, fd, revents);
}

static void do_it(struct bx_traceroute_ctx* ctx) {
    unsigned int start = (ctx->options.first_hop - 1) * ctx->options.probes_per_hop;
    unsigned int end = ctx->probe_count;
    double last_send = 0;
    double start_time = bx_traceroute_get_time();
    int consecutive_losses = 0;

    bx_traceroute_report_header(ctx, ctx->options.dst_name, &ctx->destination, ctx->options.max_hops, ctx->header_len + ctx->data_len);

    while (start < end) {
        unsigned int n, num = 0;
        double next_time = 0;
        double now_time = bx_traceroute_get_time();

        if (ctx->options.deadline > 0 && now_time - start_time > ctx->options.deadline) {
            /* Deadline reached - terminate immediately */
            break;
        }

        for (n = start; n < end; n++) {
            probe* pb = &ctx->probes[n];

            if (n == start &&              /*  probably time to print...  */
                !pb->done && pb->send_time /*  ...but yet not replied   */
            ) {
                double expire_time = pb->send_time + get_timeout(ctx, pb);

                if (expire_time > now_time)
                    next_time = expire_time;
                else {
                    ctx->method->expire_probe(ctx, pb);
                    check_expired(pb);
                }
            }

            if (pb->done) {
                if (n == start) { /*  can print it now   */
                    bx_traceroute_report_probe(ctx, pb);
                    start++;

                    if (start % ctx->options.probes_per_hop == 0) {
                        /* Check if the whole hop failed */
                        int hop_failed = 1;
                        unsigned int i;
                        for (i = start - ctx->options.probes_per_hop; i < start; i++) {
                            if (ctx->probes[i].res.sa.sa_family) {
                                hop_failed = 0;
                                break;
                            }
                        }

                        if (hop_failed)
                            consecutive_losses++;
                        else
                            consecutive_losses = 0;

                        if (ctx->options.auto_fallback && consecutive_losses >= 3 && strcmp(ctx->method->name, "tcp") != 0) {
                            const struct bx_traceroute_method* next_ops = bx_traceroute_method_find("tcp");
                            if (next_ops) {
                                for (unsigned int pending_idx = start; pending_idx < ctx->probe_count; pending_idx++) {
                                    probe* pending = &ctx->probes[pending_idx];
                                    if (pending->send_time && !pending->done)
                                        ctx->method->expire_probe(ctx, pending);
                                }
                                ctx->method->destroy(ctx);
                                ctx->method = next_ops;
                                ctx->method->reset(ctx);
                                ctx->header_len = (ctx->options.address_family == AF_INET
                                                       ? sizeof(struct iphdr)
                                                       : sizeof(struct ip6_hdr)) +
                                                  ctx->rtbuf_len + ctx->method->header_len;
                                if (next_ops->init(ctx, 0, &ctx->data_len) < 0)
                                    ex_error(ctx, "trace method's init failed");
                                if (!ctx->options.quiet)
                                    printf("\n[Fallback to TCP SYN probes at hop %u]", start / ctx->options.probes_per_hop + 1);
                            }
                        }
                    }
                }

                if (pb->final)
                    end = (n / ctx->options.probes_per_hop + 1) * ctx->options.probes_per_hop;

                continue;
            }

            if (!pb->send_time) {
                int ttl;
                double next;

                if (bx_traceroute_ecmp_flow_inflight(ctx, pb))
                    continue;

                if (ctx->options.send_secs && (next = last_send + ctx->options.send_secs) > now_time) {
                    next_time = next;
                    break;
                }

                ttl = (int)(n / ctx->options.probes_per_hop + 1);

                ctx->method->send_probe(ctx, pb, ttl);

                if (!pb->send_time) {
                    if (next_time)
                        break; /*  have chances later   */
                    else
                        bx_traceroute_error(ctx, "send probe");
                }

                last_send = pb->send_time;
            }

            if (!next_time)
                next_time = pb->send_time + get_timeout(ctx, pb);

            num++;
            if (num >= ctx->options.simultaneous_probes)
                break;
        }

        if (next_time) {
            double now = bx_traceroute_get_time();
            double timeout = next_time - now;

            if (ctx->options.deadline > 0) {
                double remaining = ctx->options.deadline - (now - start_time);
                if (remaining < 0)
                    remaining = 0;
                if (remaining < timeout)
                    timeout = remaining;
            }

            if (timeout < 0)
                timeout = 0;

            bx_traceroute_do_poll(ctx, timeout, poll_callback);
        }
    }

    bx_traceroute_report_end(ctx);

    return;
}

static void bx_traceroute_emit(void* user, const char* progname, const char* message) {
    (void)user;
    (void)progname;
    fprintf(stderr, "%s\n", message);
}

void bx_traceroute_ctx_init(struct bx_traceroute_ctx* ctx) {
    ctx->diag.emit = bx_traceroute_emit;
    ctx->options.first_hop = 1;
    ctx->options.max_hops = DEF_HOPS;
    ctx->options.simultaneous_probes = DEF_SIM_PROBES;
    ctx->options.probes_per_hop = DEF_NUM_PROBES;
    ctx->options.ipv6_rthdr_type = 2;
    ctx->options.packet_len = -1;
    ctx->options.wait_secs = DEF_WAIT_SECS;
    ctx->options.here_factor = DEF_HERE_FACTOR;
    ctx->options.near_factor = DEF_NEAR_FACTOR;
    ctx->options.send_secs = DEF_SEND_SECS;
    ctx->options.method_name = "default";
    ctx->options.ts_mode = TS_KERNEL_SW;
    if (!bx_random_bytes_nonblocking(&ctx->random_state, sizeof(ctx->random_state)))
        ctx->random_state = (uint32_t)time(NULL) ^ (uint32_t)getpid();
    if (!ctx->random_state)
        ctx->random_state = 1;
}

void bx_traceroute_ctx_destroy(struct bx_traceroute_ctx* ctx) {
    bx_traceroute_xdp_cleanup();
    bx_traceroute_bpf_cleanup();
    if (ctx->probes) {
        for (unsigned int i = 0; i < ctx->probe_count; i++) {
            if (ctx->probes[i].sk > 0)
                close(ctx->probes[i].sk);
            free(ctx->probes[i].ext);
        }
    }
    if (ctx->method)
        ctx->method->destroy(ctx);
    free(ctx->probes);
    free(ctx->poll.pfd);
    free(ctx->rtbuf);
    if (ctx->gateways) {
        for (int i = 0; i < ctx->num_gateways; i++)
            free(ctx->gateways[i]);
        free(ctx->gateways);
    }
    for (unsigned int i = 0; i < ctx->options.method_option_count; i++)
        free(ctx->options.method_options[i]);
}

int bx_traceroute_main(int argc, char** argv) {
    struct bx_traceroute_ctx ctx = {0};
    bx_traceroute_ctx_init(&ctx);
    setlocale(LC_ALL, "");
    setlocale(LC_NUMERIC, "C");
    int status;
    if (!bx_traceroute_parse_options(&ctx, argc, argv))
        status = ctx.diag.exit_status;
    else
        status = bx_traceroute_run(&ctx);
    bx_traceroute_ctx_destroy(&ctx);
    return status;
}
