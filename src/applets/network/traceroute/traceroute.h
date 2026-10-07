/*
    Copyright (c)  2006, 2007		Dmitry Butskoy
                                        <dmitry@butskoy.name>
    License:  GPL v2 or any later

    See COPYING for the status of this software.
*/

#ifndef TRACEROUTE_TRACEROUTE_H
#define TRACEROUTE_TRACEROUTE_H

#include <netinet/in.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <poll.h>
#include "bx/diag.h"

union common_sockaddr {
    struct sockaddr sa;
    struct sockaddr_in sin;
    struct sockaddr_in6 sin6;
};
typedef union common_sockaddr sockaddr_any;

struct bx_traceroute_probe {
    int done;
    int final;
    sockaddr_any res;
    double send_time;
    double recv_time;
    int recv_ttl;
    int mtu;
    int ifindex_in;
    int ifindex_out;
    int sk;
    int seq;
    char* ext;
    char err_str[32]; /*  assume enough   */
};
typedef struct bx_traceroute_probe probe;

enum bx_traceroute_method_id {
    BX_TRACEROUTE_METHOD_DEFAULT,
    BX_TRACEROUTE_METHOD_UDP,
    BX_TRACEROUTE_METHOD_UDPLITE,
    BX_TRACEROUTE_METHOD_ICMP,
    BX_TRACEROUTE_METHOD_TCP,
    BX_TRACEROUTE_METHOD_TCPCONN,
    BX_TRACEROUTE_METHOD_DCCP,
    BX_TRACEROUTE_METHOD_RAW,
    BX_TRACEROUTE_METHOD_UNKNOWN
};

enum bx_traceroute_option_result {
    BX_TRACEROUTE_OPTION_OK = 0,
    BX_TRACEROUTE_OPTION_UNKNOWN = -1,
    BX_TRACEROUTE_OPTION_BAD_ARGUMENT = -2,
    BX_TRACEROUTE_OPTION_NEEDS_ARGUMENT = -3,
    BX_TRACEROUTE_OPTION_EXCLUSIVE = -4,
};

struct bx_traceroute_ctx;
struct bx_traceroute_method {
    enum bx_traceroute_method_id id;
    const char* name;
    void (*reset)(struct bx_traceroute_ctx* ctx);
    void (*destroy)(struct bx_traceroute_ctx* ctx);
    int (*init)(struct bx_traceroute_ctx* ctx, unsigned int port, size_t* packet_len);
    void (*send_probe)(struct bx_traceroute_ctx* ctx, probe* pb, int ttl);
    void (*recv_probe)(struct bx_traceroute_ctx* ctx, int fd, int revents);
    void (*expire_probe)(struct bx_traceroute_ctx* ctx, probe* pb);
    int (*parse_option)(struct bx_traceroute_ctx* ctx, const char* option);
    void (*print_options)(FILE* stream);
    bool one_per_time;
    size_t header_len;
};

#define __TEXT(X) #X
#define _TEXT(X) __TEXT(X)

#define DEF_START_PORT 33434         /*  start for traditional udp method   */
#define DEF_UDP_PORT 53              /*  dns   */
#define DEF_TCP_PORT 80              /*  web   */
#define DEF_DCCP_PORT DEF_START_PORT /*  is it a good choice?...  */
#define DEF_RAW_PROT 253             /*  for experimentation and testing, rfc3692  */

#define MAX_HOPS 255
#define MAX_PROBES 10
#define MAX_GATEWAYS_4 8
#define MAX_GATEWAYS_6 127
#define DEF_HOPS 30
#define MAX_SIM_PROBES 1024
#define DEF_SIM_PROBES 16 /*  including several hops   */
#define DEF_NUM_PROBES 3
#define DEF_WAIT_SECS 5.0
#define DEF_HERE_FACTOR 3
#define DEF_NEAR_FACTOR 10
#ifndef DEF_WAIT_PREC
#define DEF_WAIT_PREC 0.001 /*  +1 ms  to avoid precision issues   */
#endif
#define DEF_SEND_SECS 0
#define DEF_DATA_LEN 40 /*  all but IP header...  */
#define MAX_PACKET_LEN 65000

typedef enum { TS_USERSPACE = 0, TS_KERNEL_SW, TS_KERNEL_HW } ts_mode_t;

struct bx_traceroute_options {
    const char* progname;
    bool show_method_help;
    int debug;
    int jsonl;
    int quiet;
    int bpf_mode;
    unsigned int first_hop;
    unsigned int max_hops;
    unsigned int simultaneous_probes;
    unsigned int probes_per_hop;
    unsigned int ecmp;
    unsigned int ipv6_rthdr_type;
    int dont_fragment;
    int noresolve;
    int extension;
    int as_lookups;
    unsigned int destination_port;
    unsigned int tos;
    unsigned int flow_label;
    int noroute;
    unsigned int fwmark;
    int packet_len;
    double wait_secs;
    double deadline;
    double here_factor;
    double near_factor;
    double send_secs;
    int mtu_discovery;
    int backward;
    const char* dst_name;
    const char* interface;
    unsigned int source_port;
    int auto_fallback;
    const char* netns;
    enum bx_traceroute_method_id method;
    const char* method_name;
    char* method_options[15];
    unsigned int method_option_count;
    int address_family;
    ts_mode_t ts_mode;
};

struct bx_traceroute_udp_state {
    sockaddr_any dest_addr;
    unsigned int curr_port;
    unsigned int protocol;
    char* data;
    size_t* length_p;
    unsigned int coverage;
};

struct bx_traceroute_icmp_state {
    sockaddr_any dest_addr;
    uint16_t seq;
    uint16_t ident;
    char* data;
    size_t* length_p;
    int icmp_sk;
    int last_ttl;
    int raw;
    int dgram;
};

struct bx_traceroute_tcp_state {
    sockaddr_any dest_addr;
    unsigned int dest_port;
    int raw_sk;
    int last_ttl;
    uint8_t buf[1024];
    size_t csum_len;
    struct tcphdr* th;
    int flags;
    int sysctl;
    int reuse;
    int mss;
    int check_mss;
    int info;
    int fastopen;
};

struct bx_traceroute_tcpconn_state {
    sockaddr_any dest_addr;
    int icmp_sk;
};

struct bx_traceroute_dccp_state {
    sockaddr_any dest_addr;
    unsigned int dest_port;
    int raw_sk;
    int last_ttl;
    uint8_t buf[1024];
    size_t csum_len;
    struct dccp_hdr* dh;
    struct dccp_hdr_ext* dhe;
    struct dccp_hdr_request* dhr;
    unsigned int service_code;
};

struct bx_traceroute_raw_state {
    sockaddr_any dest_addr;
    int protocol;
    char* data;
    size_t* length_p;
    int raw_sk;
    int last_ttl;
    int seq;
};

struct bx_traceroute_poll {
    struct pollfd* pfd;
    unsigned int num_polls;
    unsigned int max_polls;
};

struct bx_traceroute_ctx {
    struct bx_diag_ctx diag;
    struct bx_traceroute_options options;
    char** gateways;
    int num_gateways;
    unsigned char* rtbuf;
    size_t rtbuf_len;
    size_t header_len;
    size_t data_len;
    sockaddr_any destination;
    sockaddr_any source;
    const struct bx_traceroute_method* method;
    probe* probes;
    unsigned int probe_count;
    char addr2str_buf[INET6_ADDRSTRLEN];
    struct bx_traceroute_poll poll;
    sockaddr_any as_address;
    char as_buffer[1024];
    uint32_t random_state;
    union {
        struct bx_traceroute_udp_state udp;
        struct bx_traceroute_icmp_state icmp;
        struct bx_traceroute_tcp_state tcp;
        struct bx_traceroute_tcpconn_state tcpconn;
        struct bx_traceroute_dccp_state dccp;
        struct bx_traceroute_raw_state raw;
    } method_state;
};

void bx_traceroute_ctx_init(struct bx_traceroute_ctx* ctx);
void bx_traceroute_ctx_destroy(struct bx_traceroute_ctx* ctx);
bool bx_traceroute_parse_method_options(struct bx_traceroute_ctx* ctx);
bool bx_traceroute_parse_options(struct bx_traceroute_ctx* ctx, int argc, char** argv);
int bx_traceroute_getaddr(struct bx_traceroute_ctx* ctx, const char* name, sockaddr_any* addr);
bool bx_traceroute_parse_uint(const char* text, unsigned int* value);
bool bx_traceroute_option_is(const char* option, const char* name, bool abbrev, const char** value);

void bx_traceroute_error(struct bx_traceroute_ctx* ctx, const char* str) __attribute__((noreturn));
void bx_traceroute_error_or_perm(struct bx_traceroute_ctx* ctx, const char* str) __attribute__((noreturn));
void bx_traceroute_put_err(probe* pb, const char* format, ...) __attribute__((format(printf, 2, 3)));
const char* bx_traceroute_addr2str(struct bx_traceroute_ctx* ctx, const sockaddr_any* addr);

double bx_traceroute_get_time(void);
int bx_traceroute_ecmp_flow_inflight(struct bx_traceroute_ctx* ctx, const probe* pb);
void bx_traceroute_tune_socket(struct bx_traceroute_ctx* ctx, int sk, probe* pb);
void bx_traceroute_parse_icmp_res(struct bx_traceroute_ctx* ctx, probe* pb, int type, int code, int info);
void bx_traceroute_probe_done(struct bx_traceroute_ctx* ctx, probe* pb);

typedef probe* (*check_reply_t)(struct bx_traceroute_ctx* ctx, int sk, int err, sockaddr_any* from, char* buf, size_t len);
void bx_traceroute_recv_reply(struct bx_traceroute_ctx* ctx, int sk, int err, check_reply_t check_reply);

int bx_traceroute_equal_addr(const sockaddr_any* a, const sockaddr_any* b);

probe* bx_traceroute_probe_by_seq(struct bx_traceroute_ctx* ctx, int seq);
probe* bx_traceroute_probe_by_sk(struct bx_traceroute_ctx* ctx, int sk);

void bx_traceroute_bind_socket(struct bx_traceroute_ctx* ctx, int sk, probe* pb);
void bx_traceroute_use_timestamp(struct bx_traceroute_ctx* ctx, int sk);
void bx_traceroute_use_recv_ttl(struct bx_traceroute_ctx* ctx, int sk);
void bx_traceroute_use_recverr(struct bx_traceroute_ctx* ctx, int sk);
void bx_traceroute_set_ttl(struct bx_traceroute_ctx* ctx, int sk, int ttl);
int bx_traceroute_do_send(struct bx_traceroute_ctx* ctx, int sk, const void* data, size_t len, const sockaddr_any* addr);

void bx_traceroute_add_poll(struct bx_traceroute_ctx* ctx, int fd, int events);
void bx_traceroute_del_poll(struct bx_traceroute_ctx* ctx, int fd);
void bx_traceroute_do_poll(struct bx_traceroute_ctx* ctx, double timeout, void (*callback)(struct bx_traceroute_ctx* ctx, int fd, int revents));

void bx_traceroute_handle_extensions(struct bx_traceroute_ctx* ctx, probe* pb, char* buf, int len, int step);
const char* bx_traceroute_get_as_path(struct bx_traceroute_ctx* ctx, const char* query);

int bx_traceroute_raw_can_connect(void);

unsigned int bx_traceroute_random_seq(struct bx_traceroute_ctx* ctx);
uint16_t bx_traceroute_in_csum(const void* ptr, size_t len);

const struct bx_traceroute_method* bx_traceroute_method_find(const char* name);
const struct bx_traceroute_method* bx_traceroute_method_by_id(enum bx_traceroute_method_id id);

void bx_traceroute_report_header(struct bx_traceroute_ctx* ctx, const char* dst_name, const sockaddr_any* dst_addr, unsigned int max_hops, size_t packet_len);
void bx_traceroute_report_probe(struct bx_traceroute_ctx* ctx, probe* pb);
void bx_traceroute_report_end(struct bx_traceroute_ctx* ctx);

void bx_traceroute_export_jsonl_header(struct bx_traceroute_ctx* ctx, const char* dst_name,
                            const sockaddr_any* dst_addr,
                            unsigned int max_hops,
                            size_t packet_len);
void bx_traceroute_export_jsonl_probe(struct bx_traceroute_ctx* ctx, probe* pb);
void bx_traceroute_export_jsonl_end(void);

int bx_traceroute_bpf_init(const char* obj_path);
int bx_traceroute_bpf_decode_event(void* data, size_t data_sz);
void bx_traceroute_bpf_poll(int fd, int revents);
void bx_traceroute_bpf_print_histograms(void);
void bx_traceroute_bpf_cleanup(void);

int bx_traceroute_xdp_init(const char* ifname, const char* obj_path);
void bx_traceroute_xdp_poll(int fd, int revents);
void bx_traceroute_xdp_cleanup(void);

#endif /* TRACEROUTE_TRACEROUTE_H */
