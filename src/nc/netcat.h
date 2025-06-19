/* $OpenBSD: netcat.c,v 1.237 2025/12/06 09:48:30 phessler Exp $ */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/wait.h>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netinet/ip.h>

#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <tls.h>
#include <unistd.h>

#include "atomicio.h"

#ifndef AF_VSOCK
#define AF_VSOCK 40
#endif

#define PORT_MAX 65535
#define UNIX_DG_TMP_SOCKET_SIZE 19

#define POLL_STDIN 0
#define POLL_NETOUT 1
#define POLL_NETIN 2
#define POLL_STDOUT 3
#define BUFSIZE 16384
#define UDP_IO_BUFSIZE 65535
#define UDP_MAX_WRITE_PAYLOAD 65487

/* Exit codes */
/*
 * EXIT_OK (0): Success
 * EXIT_RUNTIME (1): Runtime error (connection failed, I/O error, etc.)
 * EXIT_USAGE (2): Usage error (invalid flag, missing argument, conflict)
 * EXIT_EXEC_FAILED (126): Exec failure (not executable, permission denied)
 * EXIT_COMMAND_NOT_FOUND (127): Exec failure (command not found)
 */
#define EXIT_OK 0
#define EXIT_RUNTIME 1
#define EXIT_USAGE 2
#define EXIT_EXEC_FAILED 126
#define EXIT_COMMAND_NOT_FOUND 127

#define TLS_NOVERIFY (1 << 1)
#define TLS_NONAME (1 << 2)
#define TLS_CCERT (1 << 3)
#define TLS_MUSTSTAPLE (1 << 4)

#define PCAP_FILTER_IN 0x01
#define PCAP_FILTER_OUT 0x02
#define PCAP_FILTER_BOTH (PCAP_FILTER_IN | PCAP_FILTER_OUT)

/* Command Line Options */
extern int dflag;          /* detached, no stdin */
extern int Fflag;          /* fdpass sock to stdout */
extern unsigned int iflag; /* Interval Flag */
extern int kflag;          /* More than one connect */
extern int lflag;          /* Bind to local port */
extern int jflag;          /* JSON output */
extern char* pcapfile;     /* PCAP file path */
extern unsigned int pcap_snaplen;
extern int pcap_filter;
extern unsigned long long pcap_rotate_size;
extern unsigned int pcap_rotate_seconds;
extern int proxy_proto;   /* PROXY protocol server */
extern int send_proxy;    /* PROXY protocol client */
extern FILE* hex_fp;      /* Hex dump file pointer */
extern char* hex_path;    /* Hex dump file path */
extern int Nflag;         /* shutdown() network socket */
extern int fuzz_tcp;      /* Fuzz TCP with random data */
extern int fuzz_udp;      /* Fuzz UDP with random data */
extern int tfoflag;       /* TCP Fast Open */
extern int mptcpflag;     /* Multipath TCP */
extern int mptcp_netlink; /* MPTCP PM netlink diagnostics */
extern int spliceflag;    /* Zero-copy splice */
extern int sockmark;      /* SO_MARK */
extern int sockpriority;  /* SO_PRIORITY */
extern int nflag;         /* Don't do name look up */
extern char* Pflag;       /* Proxy username */
extern char* pflag;       /* Localport flag */
extern int rflag;         /* Random ports flag */
extern char* sflag;       /* Source Address */
extern char* iface;       /* Interface to bind to */
extern int transparent;   /* IP_TRANSPARENT */
extern int uflag;         /* UDP - Default to TCP */
extern int vflag;         /* Verbosity */
extern int xflag;         /* Socks proxy */
extern int zflag;         /* Port Scan Flag */
extern int Dflag;         /* sodebug */
extern int Iflag;         /* TCP receive buffer size */
extern int Oflag;         /* TCP send buffer size */
extern int Tflag;         /* IP Type of Service */

extern int usetls;           /* use TLS */
extern int dtls;             /* use DTLS */
extern const char* Cflag;    /* Public cert file */
extern const char* Kflag;    /* Private key file */
extern const char* oflag;    /* OCSP stapling file */
extern const char* Rflag;    /* Root CA file */
extern int tls_cachanged;    /* Using non-default CA file */
extern int TLSopt;           /* TLS options */
extern char* exec_path;      /* program to exec */
extern char* tls_expectname; /* required name in peer cert */
extern char* tls_expecthash; /* required hash of peer cert */
extern char* tls_ciphers;    /* TLS ciphers */
extern char* tls_protocols;  /* TLS protocols */
extern char* tls_alpn;       /* TLS ALPN */
extern FILE* Zflag;          /* file to save peer cert */

extern int recvcount, recvlimit;
extern int timeout;
extern int family;
extern char* portlist[PORT_MAX + 1];
extern char* unix_dg_tmp_socket;
extern int ttl;
extern int minttl;

extern char* vsock_cid;
extern char* vsock_port;

extern int jitter;
extern char* profile;
extern int quic_mask;
extern pid_t child_pid;

/* RNG hook for testing */
extern uint32_t (*nc_random)(void);
extern ssize_t (*nc_sendfile_fn)(int out_fd, int in_fd, off_t* offset, size_t count);
double gaussian_random(double mean, double stddev);

long long nc_strtonum(const char* numstr, long long minval, long long maxval, const char** errstrp);
size_t nc_strlcpy(char* dst, const char* src, size_t dsize);
uint32_t nc_random_u32(void);
void nc_random_buf(void* buf, size_t len);
uint32_t nc_random_uniform(uint32_t upper_bound);
int nc_sleep_monotonic(double seconds);
int nc_wait_fd_events_monotonic(int fd, short events, int timeout_ms);
int is_address(const char*);

int strtoport(char* portstr, int udp);
void build_ports(char*);
void help(void) __attribute__((noreturn));
int local_listen(const char*, const char*, struct addrinfo);
void readwrite(int, struct tls*);
void fdpass(int nfd) __attribute__((noreturn));
int remote_connect(const char*, const char*, struct addrinfo, char*);
int timeout_tls(int, struct tls*, int (*)(struct tls*));
int timeout_connect(int, const struct sockaddr*, socklen_t);
int socks_connect(const char*,
                  const char*,
                  struct addrinfo,
                  const char*,
                  const char*,
                  struct addrinfo,
                  int,
                  const char*);
int udptest(int);
void connection_info(int, const char*, const char*, const char*, const char*);
int unix_bind(char*, int);
int unix_connect(char*);
int unix_listen(char*);
int vsock_listen(const char*, const char*);
int vsock_connect(const char*, const char*);
void set_common_sockopts(int, int);
int process_tos_opt(char*, int*);
int process_tls_opt(char*, int*);
void save_peer_cert(struct tls* _tls_ctx, FILE* _fp);
void report_sock(const char*, const struct sockaddr*, socklen_t, char*);
void report_tls(struct tls* tls_ctx, char* host, int fd);
void vsock_report(const char*, const char*, int);
void report_mptcp_info(int s);
#ifdef GAPING_SECURITY_HOLE
void spawn_exec(int);
#endif
void usage(int);
void json_timestamp_now(char* tbuf, size_t tbufsz);
void json_event_begin(FILE* fp,
                      const char* level,
                      const char* event,
                      const char* direction,
                      const char* protocol,
                      const char* tls_state,
                      const char* quic_state,
                      const char* src_addr,
                      const char* src_port,
                      const char* dst_addr,
                      const char* dst_port);
int json_socket_tuple_from_fd(int fd,
                              char* src_addr,
                              size_t src_addr_len,
                              char* src_port,
                              size_t src_port_len,
                              char* dst_addr,
                              size_t dst_addr_len,
                              char* dst_port,
                              size_t dst_port_len);
void hexdump(FILE* fp, const char* prefix, const unsigned char* buf, size_t len, size_t total);
ssize_t drainbuf(int, unsigned char*, size_t*, size_t, struct tls*, int);
ssize_t fillbuf(int, unsigned char*, size_t*, size_t, struct tls*, int);
void tls_setup_client(struct tls*, int, char*);
struct tls* tls_setup_server(struct tls*, int, char*);
