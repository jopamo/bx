/* $OpenBSD: netcat.c,v 1.237 2025/12/06 09:48:30 phessler Exp $ */
/*
 * Copyright (c) 2001 Eric Jackson <ericj@monkey.org>
 * Copyright (c) 2015 Bob Beck.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *   derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Re-written nc(1) for OpenBSD. Original implementation by
 * *Hobbit* <hobbit@avian.org>.
 */

#include "netcat.h"
#include "pcap.h"
#include "proxy_proto.h"
#include "quic.h"
#include "bpf.h"
#include "syscalls.h"
#include "version.h"
#include <getopt.h>
#include <sched.h>
#include <fcntl.h>
#include <strings.h>
#include <sys/stat.h>
#include "lib/args_common.h"
#ifdef USE_IO_URING
#include <liburing.h>
#endif

/* Command Line Options */
int dflag;          /* detached, no stdin */
int Fflag;          /* fdpass sock to stdout */
unsigned int iflag; /* Interval Flag */
int keepopen;       /* More than one connect (formerly -k) */
int lflag;          /* Bind to local port */
int jflag;          /* JSON output */
char* pcapfile;     /* PCAP file path */
unsigned int pcap_snaplen = 65535;
int pcap_filter = PCAP_FILTER_BOTH;
unsigned long long pcap_rotate_size;
unsigned int pcap_rotate_seconds;
int proxy_proto;        /* PROXY protocol server */
int send_proxy;         /* PROXY protocol client */
int quic_probe;         /* QUIC probing */
FILE* hex_fp;           /* Hex dump file pointer */
char* hex_path;         /* Hex dump file path */
char* bpf_prog_path;    /* BPF program path */
char* bpf_evasion_path; /* BPF evasion program path */
int Nflag;              /* shutdown() network socket */
int nflag;              /* Don't do name look up */
char* Pflag;            /* Proxy username */
char* pflag;            /* Localport flag */
int rflag;              /* Random ports flag */
char* sflag;            /* Source Address */
char* iface;            /* Interface to bind to */
int transparent;        /* IP_TRANSPARENT */
int uflag;              /* UDP - Default to TCP */
int vflag;              /* Verbosity */
int xflag;              /* Socks proxy */
int zflag;              /* Port Scan Flag */
int Dflag;              /* sodebug */
int Iflag;              /* TCP receive buffer size */
int Oflag;              /* TCP send buffer size */
int Tflag = -1;         /* IP Type of Service */

int fuzz_tcp; /* Fuzz TCP with random data */

int fuzz_udp; /* Fuzz UDP with random data */

int tfoflag; /* TCP Fast Open */

int mptcpflag;     /* Multipath TCP */
int mptcp_netlink; /* MPTCP PM netlink diagnostics */

int spliceflag; /* Zero-copy splice */

char* netns; /* Network namespace path */

int sockmark = -1; /* SO_MARK */

int sockpriority = -1; /* SO_PRIORITY */

int usetls;           /* use TLS */
int dtls;             /* use DTLS */
const char* Cflag;    /* Public cert file */
const char* Kflag;    /* Private key file */
const char* oflag;    /* OCSP stapling file */
const char* Rflag;    /* Root CA file */
int tls_cachanged;    /* Using non-default CA file */
int TLSopt;           /* TLS options */
char* exec_path;      /* program to exec */
char* tls_expectname; /* required name in peer cert */
char* tls_expecthash; /* required hash of peer cert */
char* tls_ciphers;    /* TLS ciphers */
char* tls_protocols;  /* TLS protocols */
char* tls_alpn;       /* TLS ALPN */
FILE* Zflag;          /* file to save peer cert */

int recvcount, recvlimit;
int timeout = -1;
int family = AF_UNSPEC;
char* portlist[PORT_MAX + 1];
char* unix_dg_tmp_socket;
int ttl = -1;
int minttl = -1;

char* vsock_cid;
char* vsock_port;

int jitter;
char* profile;
int quic_mask;
char* xdp_iface;
pid_t child_pid = -1;
static int io_uringflag;          /* require io_uring backend */
static int quietflag;             /* suppress non-error informational output */
static const char* log_file_path; /* append diagnostic output to this file */

static void do_readwrite(int nfd, struct tls* tls_ctx) {
#ifdef GAPING_SECURITY_HOLE
    if (exec_path) {
        spawn_exec(nfd);
    }
#endif
    readwrite(nfd, tls_ctx);
}

static const char* nc_progname(const char* argv0) {
    const char* base;

    if (argv0 == NULL || argv0[0] == '\0')
        return "nc";

    base = strrchr(argv0, '/');
    return base == NULL ? argv0 : base + 1;
}

static void enforce_io_backend_policy(void) {
    if (!io_uringflag)
        return;
#ifndef __linux__
    errx(EXIT_USAGE, "--io-uring is only supported on Linux");
#else
#ifndef USE_IO_URING
    errx(EXIT_USAGE,
         "--io-uring requested but this build was compiled without liburing; default poll backend remains available");
#else
    struct io_uring ring;
    int rc;

    rc = io_uring_queue_init(2, &ring, 0);
    if (rc < 0)
        errx(EXIT_RUNTIME, "--io-uring unavailable on this kernel/runtime: %s", strerror(-rc));
    io_uring_queue_exit(&ring);

    errx(EXIT_RUNTIME, "--io-uring requested, but this release only ships the poll backend");
#endif
#endif
}

static int parse_hex_tos_value(const char* text, int* value_out) {
    if (text == NULL || text[0] == '\0' || text[0] == '-')
        return 0;

    errno = 0;
    char* end = NULL;
    unsigned long value = strtoul(text, &end, 16);
    if (errno == ERANGE || end == text || end == NULL || *end != '\0' || value > 255ul)
        return 0;

    *value_out = (int)value;
    return 1;
}

int main(int argc, char* argv[]) {
    int ch, s = -1, ret, socksv;
    char *host, *uport;
    char ipaddr[NI_MAXHOST];
    struct addrinfo hints;
    socklen_t len;
    struct sockaddr_storage cliaddr;
    char *proxy = NULL, *proxy_alloc = NULL, *proxyport = NULL;
    const char* errstr;
    struct addrinfo proxyhints;
    char unix_dg_tmp_socket_buf[UNIX_DG_TMP_SOCKET_SIZE];
    struct tls_config* tls_cfg = NULL;
    struct tls* tls_ctx = NULL;
    uint32_t protocols;
    int option_index = 0;
    int pcap_path_explicit = 0;
    int pcap_path_default = 0;
    const char* progname = nc_progname(argc > 0 ? argv[0] : NULL);
    static struct option long_options[] = {{"mptcp", no_argument, NULL, 1001},
                                           {"help", no_argument, NULL, 'h'},
                                           {"tfo", no_argument, NULL, 1002},
                                           {"mark", required_argument, NULL, 1003},
                                           {"quic", no_argument, NULL, 1004},
                                           {"dtls", no_argument, NULL, 1005},
                                           {"proxy-proto", no_argument, NULL, 1006},
                                           {"send-proxy", no_argument, NULL, 1007},
                                           {"vsock", required_argument, NULL, 1008},
                                           {"namespace", required_argument, NULL, 1009},
                                           {"pcap", required_argument, NULL, 1010},
                                           {"pcap-snaplen", required_argument, NULL, 1029},
                                           {"pcap-filter", required_argument, NULL, 1030},
                                           {"pcap-rotate-size", required_argument, NULL, 1031},
                                           {"pcap-rotate-seconds", required_argument, NULL, 1032},
                                           {"pcap-default-path", no_argument, NULL, 1033},
                                           {"fuzz-tcp", no_argument, NULL, 1011},
                                           {"fuzz-udp", no_argument, NULL, 1012},
                                           {"splice", no_argument, NULL, 1013},
                                           {"io-uring", no_argument, NULL, 1014},
                                           {"hex-dump", required_argument, NULL, 1015},
                                           {"bpf-prog", required_argument, NULL, 1016},
                                           {"keep-open", no_argument, NULL, 1017},
                                           {"interface", required_argument, NULL, 1018},
                                           {"transparent", no_argument, NULL, 1019},
                                           {"bpf-evasion", required_argument, NULL, 1020},
                                           {"jitter", required_argument, NULL, 1021},
                                           {"profile", required_argument, NULL, 1022},
                                           {"quic-mask", no_argument, NULL, 1023},
                                           {"xdp-stealth", required_argument, NULL, 1024},
                                           {"version", no_argument, NULL, 1025},
                                           {"mptcp-netlink", no_argument, NULL, 1026},
                                           {"quiet", no_argument, NULL, 1027},
                                           {"log-file", required_argument, NULL, 1028},
#ifdef GAPING_SECURITY_HOLE
                                           {"exec", required_argument, NULL, 'e'},
#endif
                                           {NULL, 0, NULL, 0}};

    /* Fast-path for --help and --version to succeed despite malformed flags */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
            help();
        if (strcmp(argv[i], "--version") == 0) {
            printf("netcat %s (commit %s %s)\n", NC_VERSION, NC_GIT_HASH, NC_GIT_DATE);
            exit(EXIT_OK);
        }
    }

    ret = EXIT_RUNTIME;
    socksv = 5;
    host = NULL;
    uport = NULL;
    exec_path = NULL;
    Rflag = tls_default_ca_cert_file();

    signal(SIGPIPE, SIG_IGN);

    while ((ch = bx_args_getopt_long(argc, argv, "46C:cDde:FH:hI:i:jK:lM:m:NnO:o:P:p:R:rs:T:UuvW:w:X:x:Z:z", long_options,
                             &option_index)) != -1) {
        switch (ch) {
            case 1001:
                mptcpflag = 1;
                break;
            case 1002:
                tfoflag = 1;
                break;
            case 1003:
                sockmark = nc_strtonum(optarg, 0, INT_MAX, &errstr);
                if (errstr)
                    errx(EXIT_USAGE, "mark is %s", errstr);
                break;
            case 1004:
                quic_probe = 1;
                uflag = 1;
                break;
            case 1005:
                dtls = 1;
                usetls = 1;
                uflag = 1;
                break;
            case 1006:
                proxy_proto = 1;
                break;
            case 1007:
                send_proxy = 1;
                break;
            case 1008:
                if ((vsock_cid = strdup(optarg)) == NULL)
                    err(EXIT_USAGE, NULL);
                if ((vsock_port = strchr(vsock_cid, ':')) == NULL)
                    errx(EXIT_USAGE, "vsock: expected CID:PORT");
                *vsock_port++ = '\0';
                break;
            case 1009:
                netns = optarg;
                break;
            case 1010:
                pcapfile = optarg;
                pcap_path_explicit = 1;
                break;
            case 1029:
                pcap_snaplen = (unsigned int)nc_strtonum(optarg, 1, 65535, &errstr);
                if (errstr)
                    errx(EXIT_USAGE, "pcap snaplen is %s", errstr);
                break;
            case 1030:
                if (strcmp(optarg, "in") == 0 || strcmp(optarg, "rx") == 0)
                    pcap_filter = PCAP_FILTER_IN;
                else if (strcmp(optarg, "out") == 0 || strcmp(optarg, "tx") == 0)
                    pcap_filter = PCAP_FILTER_OUT;
                else if (strcmp(optarg, "both") == 0)
                    pcap_filter = PCAP_FILTER_BOTH;
                else
                    errx(EXIT_USAGE, "pcap filter must be one of: in, out, both");
                break;
            case 1031:
                pcap_rotate_size = (unsigned long long)nc_strtonum(optarg, 0, LLONG_MAX, &errstr);
                if (errstr)
                    errx(EXIT_USAGE, "pcap rotate size is %s", errstr);
                break;
            case 1032:
                pcap_rotate_seconds = (unsigned int)nc_strtonum(optarg, 0, INT_MAX, &errstr);
                if (errstr)
                    errx(EXIT_USAGE, "pcap rotate seconds is %s", errstr);
                break;
            case 1033:
                pcap_path_default = 1;
                pcapfile = "";
                break;
            case 1011:
                fuzz_tcp = 1;
                break;
            case 1012:
                fuzz_udp = 1;
                break;
            case 1013:
                spliceflag = 1;
                break;
            case 1014:
                io_uringflag = 1;
                break;
            case 1015:
                hex_path = optarg;
                break;
            case 1016:
                bpf_prog_path = optarg;
                break;
            case 1017:
                keepopen = 1;
                break;
            case 1018:
                iface = optarg;
                break;
            case 1019:
                transparent = 1;
                break;
            case 1020:
                bpf_evasion_path = optarg;
                if (load_bpf_tracepoint(bpf_evasion_path) == -1)
                    errx(EXIT_USAGE, "bpf evasion load failed");
                break;
            case 1021:
                jitter = nc_strtonum(optarg, 0, INT_MAX, &errstr);
                if (errstr)
                    errx(EXIT_USAGE, "jitter is %s", errstr);
                break;
            case 1022:
                profile = optarg;
                break;
            case 1023:
                quic_mask = 1;
                break;
            case 1024:
                xdp_iface = optarg;
                break;
            case 1025:
                printf("netcat %s (commit %s %s)\n", NC_VERSION, NC_GIT_HASH, NC_GIT_DATE);
                exit(EXIT_OK);
            case 1026:
                mptcp_netlink = 1;
                break;
            case 1027:
                quietflag = 1;
                break;
            case 1028:
                log_file_path = optarg;
                break;
            case '4':
                family = AF_INET;
                break;
            case '6':
                fprintf(stderr, "%s: IPv6 is not supported in this IPv4-only build\n", progname);
                fprintf(stderr, "Try '%s --help' for more information.\n", progname);
                exit(EXIT_USAGE);
            case 'j':
                jflag = 1;
                break;
            case 'U':
                family = AF_UNIX;
                break;
            case 'X':
                if (strcasecmp(optarg, "connect") == 0)
                    socksv = -1; /* HTTP proxy CONNECT */
                else if (strcmp(optarg, "5") == 0)
                    socksv = 5; /* SOCKS v.5 */
                else
                    errx(EXIT_USAGE, "unsupported proxy protocol");
                break;
            case 'C':
                Cflag = optarg;
                break;
            case 'c':
                usetls = 1;
                break;
            case 'd':
                dflag = 1;
                break;
            case 'e':
#ifdef GAPING_SECURITY_HOLE
                exec_path = optarg;
#else
                tls_expectname = optarg;
#endif
                break;
            case 'F':
                Fflag = 1;
                break;
            case 'H':
                tls_expecthash = optarg;
                break;
            case 'h':
                help();
                break;
            case 'i':
                iflag = nc_strtonum(optarg, 0, UINT_MAX, &errstr);
                if (errstr)
                    errx(EXIT_USAGE, "interval %s: %s", errstr, optarg);
                break;
            case 'K':
                Kflag = optarg;
                break;
            case 'l':
                lflag = 1;
                break;
            case 'M':
                ttl = nc_strtonum(optarg, 0, 255, &errstr);
                if (errstr)
                    errx(EXIT_USAGE, "ttl is %s", errstr);
                break;
            case 'm':
                minttl = nc_strtonum(optarg, 0, 255, &errstr);
                if (errstr)
                    errx(EXIT_USAGE, "minttl is %s", errstr);
                break;
            case 'N':
                Nflag = 1;
                break;
            case 'n':
                nflag = 1;
                break;
            case 'P':
                Pflag = optarg;
                break;
            case 'p':
                pflag = optarg;
                break;
            case 'R':
                tls_cachanged = 1;
                Rflag = optarg;
                break;
            case 'r':
                rflag = 1;
                break;
            case 's':
                sflag = optarg;
                break;
            case 'u':
                uflag = 1;
                break;
            case 'v':
                vflag++;
                break;
            case 'W':
                recvlimit = nc_strtonum(optarg, 1, INT_MAX, &errstr);
                if (errstr)
                    errx(EXIT_USAGE, "receive limit %s: %s", errstr, optarg);
                break;
            case 'w':
                timeout = nc_strtonum(optarg, 0, INT_MAX / 1000, &errstr);
                if (errstr)
                    errx(EXIT_USAGE, "timeout %s: %s", errstr, optarg);
                timeout *= 1000;
                break;
            case 'x':
                xflag = 1;
                free(proxy_alloc);
                if ((proxy_alloc = strdup(optarg)) == NULL)
                    err(EXIT_USAGE, NULL);
                proxy = proxy_alloc;
                break;
            case 'Z':
                if (strcmp(optarg, "-") == 0)
                    Zflag = stderr;
                else if ((Zflag = fopen(optarg, "w")) == NULL)
                    err(EXIT_USAGE, "can't open %s", optarg);
                break;
            case 'z':
                zflag = 1;
                break;
            case 'D':
                Dflag = 1;
                break;
            case 'I':
                Iflag = nc_strtonum(optarg, 1, 65536 << 14, &errstr);
                if (errstr != NULL)
                    errx(EXIT_USAGE, "TCP receive window %s: %s", errstr, optarg);
                break;
            case 'O':
                Oflag = nc_strtonum(optarg, 1, 65536 << 14, &errstr);
                if (errstr != NULL)
                    errx(EXIT_USAGE, "TCP send window %s: %s", errstr, optarg);
                break;
            case 'o':
                oflag = optarg;
                break;
            case 'T':
                errstr = NULL;
                errno = 0;
                if (process_tls_opt(optarg, &TLSopt))
                    break;
                if (process_tos_opt(optarg, &Tflag))
                    break;
                if (strlen(optarg) > 1 && optarg[0] == '0' && optarg[1] == 'x') {
                    if (!parse_hex_tos_value(optarg, &Tflag))
                        errstr = "invalid";
                }
                else
                    Tflag = (int)nc_strtonum(optarg, 0, 255, &errstr);
                if (Tflag < 0 || Tflag > 255 || errstr || errno) {
                    fprintf(stderr, "%s: illegal tos/tls value %s\n", progname, optarg);
                    exit(EXIT_USAGE);
                }
                break;
            default:
                usage(1);
        }
    }
    argc -= optind;
    argv += optind;

    if (quietflag) {
        vflag = 0;
        jflag = 0;
    }

    if (log_file_path != NULL) {
        int logfd;

        logfd = open(log_file_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
        if (logfd == -1)
            err(EXIT_USAGE, "cannot open log file %s", log_file_path);
        if (dup2(logfd, STDERR_FILENO) == -1) {
            close(logfd);
            err(EXIT_RUNTIME, "dup2 log file");
        }
        if (logfd != STDERR_FILENO)
            close(logfd);
        if (setvbuf(stderr, NULL, _IOLBF, 0) != 0)
            err(EXIT_RUNTIME, "setvbuf stderr");
    }

    if (xdp_iface) {
        if (!bpf_prog_path)
            errx(EXIT_USAGE, "must specify --bpf-prog with --xdp-stealth");
        if (load_xdp_stealth(bpf_prog_path, xdp_iface) == -1)
            errx(EXIT_USAGE, "xdp stealth failed");
        exit(EXIT_OK);
    }

    if (netns) {
#ifdef CLONE_NEWNET
        int fd;
        if ((fd = open(netns, O_RDONLY)) == -1)
            err(EXIT_USAGE, "open namespace %s", netns);
        if (setns(fd, CLONE_NEWNET) == -1)
            err(EXIT_USAGE, "setns %s", netns);
        close(fd);
#else
        errx(EXIT_USAGE, "Namespaces not supported on this platform");
#endif
    }

    if (vsock_cid) {
        if (family != AF_UNSPEC)
            errx(EXIT_USAGE, "cannot use -4, -6 or -U with --vsock");
        family = AF_VSOCK;
    }

    /* Cruft to make sure options are clean, and used properly. */
    if (family == AF_VSOCK) {
        if (argc != 0)
            usage(1);
    }
    else if (family == AF_UNIX) {
        if (argc != 1)
            usage(1);
        host = argv[0];
    }
    else if (lflag) {
        if (argc == 1) {
            if (is_address(argv[0]))
                errx(EXIT_USAGE, "port number required for listen on %s", argv[0]);
            uport = argv[0];
        }
        else if (argc == 2) {
            host = argv[0];
            uport = argv[1];
        }
        else {
            errx(EXIT_USAGE, "listen mode requires a port number (and optional host)");
        }
    }
    else {
        if (argc == 2) {
            host = argv[0];
            uport = argv[1];
        }
        else {
            usage(1);
        }
    }

    /* No filesystem visibility restrictions. */

    /*
     * Flag precedence and conflict matrix:
     * - QUIC, DTLS, TLS are mutually exclusive (QUIC uses UDP, DTLS uses UDP,
     *   TLS uses TCP). QUIC and DTLS automatically enable UDP mode.
     * - Proxy (SOCKS/HTTP) requires TCP and cannot be used with QUIC, DTLS,
     *   listen mode, UNIX sockets, or local source address.
     * - PCAP capture (-pcap) and JSON output (-j) are orthogonal and can be
     *   combined with any protocol.
     * - UDP mode (-u) is required for QUIC and DTLS, incompatible with TLS.
     * - Keep-open requires listen mode.
     * - Splice cannot be used with TLS, UDP, port scanning, or FD passing.
     * - TLS options require -c.
     */

    if (lflag && sflag)
        errx(EXIT_USAGE, "cannot use -s and -l");
    if (lflag && pflag)
        errx(EXIT_USAGE, "cannot use -p and -l");
    if (lflag && zflag)
        errx(EXIT_USAGE, "cannot use -z and -l");
    if (!lflag && keepopen)
        errx(EXIT_USAGE, "must use -l with --keep-open");
    if (quic_probe && lflag)
        errx(EXIT_USAGE, "cannot use --quic with -l");
    if (quic_probe && family == AF_UNIX)
        errx(EXIT_USAGE, "cannot use --quic with -U");
    if (quic_probe && family == AF_VSOCK)
        errx(EXIT_USAGE, "cannot use --quic with --vsock");
    if (quic_probe && usetls)
        errx(EXIT_USAGE, "cannot use --quic with TLS/DTLS");
    if (xflag && quic_probe)
        errx(EXIT_USAGE, "no proxy support for QUIC");
    if (xflag && dtls)
        errx(EXIT_USAGE, "no proxy support for DTLS");
    if (quic_mask && !quic_probe)
        errx(EXIT_USAGE, "must use --quic with --quic-mask");
    if (mptcp_netlink && !mptcpflag)
        errx(EXIT_USAGE, "must use --mptcp with --mptcp-netlink");
    if (pcap_path_explicit && pcap_path_default)
        errx(EXIT_USAGE, "cannot use --pcap and --pcap-default-path together");
    if ((pcap_snaplen != 65535 || pcap_filter != PCAP_FILTER_BOTH || pcap_rotate_size != 0 ||
         pcap_rotate_seconds != 0) &&
        pcapfile == NULL)
        errx(EXIT_USAGE, "pcap options require --pcap or --pcap-default-path");
    if (uflag && usetls && !dtls)
        errx(EXIT_USAGE, "cannot use -c and -u");
    if (uflag && dtls && keepopen)
        errx(EXIT_USAGE, "--dtls cannot be used with --keep-open");
    if (spliceflag && (usetls || uflag || zflag || Fflag))
        errx(EXIT_USAGE, "cannot use --splice with TLS, UDP, port scanning or FD passing");
    if ((family == AF_UNIX) && usetls)
        errx(EXIT_USAGE, "cannot use -c and -U");
    if ((family == AF_UNIX) && Fflag)
        errx(EXIT_USAGE, "cannot use -F and -U");
    if (Fflag && usetls)
        errx(EXIT_USAGE, "cannot use -c and -F");
    if (TLSopt && !usetls)
        errx(EXIT_USAGE, "you must specify -c to use TLS options");
    if (Cflag && !usetls)
        errx(EXIT_USAGE, "you must specify -c to use -C");
    if (Kflag && !usetls)
        errx(EXIT_USAGE, "you must specify -c to use -K");
    if (Zflag && !usetls)
        errx(EXIT_USAGE, "you must specify -c to use -Z");
    if (oflag && !Cflag)
        errx(EXIT_USAGE, "you must specify -C to use -o");
    if (tls_cachanged && !usetls)
        errx(EXIT_USAGE, "you must specify -c to use -R");
    if (tls_expecthash && !usetls)
        errx(EXIT_USAGE, "you must specify -c to use -H");
    if (tls_expectname && !usetls)
        errx(EXIT_USAGE, "you must specify -c to use -e");
#ifdef BX_NC_TLS_DISABLED
    if (usetls || dtls)
        errx(EXIT_USAGE, "TLS/DTLS support is disabled in this build");
#endif
    enforce_io_backend_policy();

    /* Get name of temporary socket for unix datagram client */
    if ((family == AF_UNIX) && uflag && !lflag) {
        if (sflag) {
            unix_dg_tmp_socket = sflag;
        }
        else {
            int tmpfd;

            nc_strlcpy(unix_dg_tmp_socket_buf, "/tmp/nc.XXXXXXXXXX", UNIX_DG_TMP_SOCKET_SIZE);
            if ((tmpfd = mkstemp(unix_dg_tmp_socket_buf)) == -1)
                err(EXIT_USAGE, "mkstemp");
            close(tmpfd);
            unlink(unix_dg_tmp_socket_buf);
            unix_dg_tmp_socket = unix_dg_tmp_socket_buf;
        }
    }

    /* Initialize addrinfo structure. */
    if (family != AF_UNIX) {
        memset(&hints, 0, sizeof(struct addrinfo));
        hints.ai_family = family;
        hints.ai_socktype = uflag ? SOCK_DGRAM : SOCK_STREAM;
        hints.ai_protocol = uflag ? IPPROTO_UDP : IPPROTO_TCP;
        if (nflag)
            hints.ai_flags |= AI_NUMERICHOST;
    }

    if (xflag) {
        if (uflag)
            errx(EXIT_USAGE, "no proxy support for UDP mode");

        if (lflag)
            errx(EXIT_USAGE, "no proxy support for listen");

        if (family == AF_UNIX)
            errx(EXIT_USAGE, "no proxy support for unix sockets");

        if (sflag)
            errx(EXIT_USAGE, "no proxy support for local source address");

        if (*proxy == '[') {
            ++proxy;
            proxyport = strchr(proxy, ']');
            if (proxyport == NULL)
                errx(EXIT_USAGE, "missing closing bracket in proxy");
            *proxyport++ = '\0';
            if (*proxyport == '\0')
                /* Use default proxy port. */
                proxyport = NULL;
            else {
                if (*proxyport == ':')
                    ++proxyport;
                else
                    errx(EXIT_USAGE, "garbage proxy port delimiter");
            }
        }
        else {
            proxyport = strrchr(proxy, ':');
            if (proxyport != NULL)
                *proxyport++ = '\0';
        }

        memset(&proxyhints, 0, sizeof(struct addrinfo));
        proxyhints.ai_family = family;
        proxyhints.ai_socktype = SOCK_STREAM;
        proxyhints.ai_protocol = IPPROTO_TCP;
        if (nflag)
            proxyhints.ai_flags |= AI_NUMERICHOST;
    }

    if (usetls) {
        if ((tls_cfg = tls_config_new()) == NULL)
            errx(EXIT_RUNTIME, "unable to allocate TLS config");
        if (dtls && tls_config_set_dgram(tls_cfg, 1) == -1) {
            tls_config_free(tls_cfg);
            errx(EXIT_RUNTIME, "%s", tls_config_error(tls_cfg));
        }
        if (Rflag && tls_config_set_ca_file(tls_cfg, Rflag) == -1) {
            tls_config_free(tls_cfg);
            errx(EXIT_RUNTIME, "%s", tls_config_error(tls_cfg));
        }
        if (Cflag && tls_config_set_cert_file(tls_cfg, Cflag) == -1) {
            tls_config_free(tls_cfg);
            errx(EXIT_RUNTIME, "%s", tls_config_error(tls_cfg));
        }
        if (Kflag && tls_config_set_key_file(tls_cfg, Kflag) == -1) {
            tls_config_free(tls_cfg);
            errx(EXIT_RUNTIME, "%s", tls_config_error(tls_cfg));
        }
        if (oflag && tls_config_set_ocsp_staple_file(tls_cfg, oflag) == -1) {
            tls_config_free(tls_cfg);
            errx(EXIT_RUNTIME, "%s", tls_config_error(tls_cfg));
        }
        if (tls_protocols) {
            if (tls_config_parse_protocols(&protocols, tls_protocols) == -1) {
                tls_config_free(tls_cfg);
                errx(EXIT_USAGE, "invalid TLS protocols `%s'", tls_protocols);
            }
            if (tls_config_set_protocols(tls_cfg, protocols) == -1) {
                tls_config_free(tls_cfg);
                errx(EXIT_RUNTIME, "%s", tls_config_error(tls_cfg));
            }
        }
        if (tls_ciphers && tls_config_set_ciphers(tls_cfg, tls_ciphers) == -1) {
            tls_config_free(tls_cfg);
            errx(EXIT_RUNTIME, "%s", tls_config_error(tls_cfg));
        }
        if (tls_alpn != NULL && tls_config_set_alpn(tls_cfg, tls_alpn) == -1) {
            tls_config_free(tls_cfg);
            errx(EXIT_RUNTIME, "%s", tls_config_error(tls_cfg));
        }
        if (!lflag && (TLSopt & TLS_CCERT)) {
            tls_config_free(tls_cfg);
            errx(EXIT_USAGE, "clientcert is only valid with -l");
        }
        if (TLSopt & TLS_NONAME)
            tls_config_insecure_noverifyname(tls_cfg);
        if (TLSopt & TLS_NOVERIFY) {
            if (tls_expecthash != NULL) {
                tls_config_free(tls_cfg);
                errx(EXIT_USAGE,
                     "-H and -T noverify may not be used "
                     "together");
            }
            tls_config_insecure_noverifycert(tls_cfg);
        }
        if (TLSopt & TLS_MUSTSTAPLE)
            tls_config_ocsp_require_stapling(tls_cfg);
    }
    if (lflag) {
        ret = 0;

        if (uport) {
            /* Validate port number (allow 0 for ephemeral listen ports). */
            if (strcmp(uport, "0") != 0)
                strtoport(uport, uflag);
        }
        else if (family != AF_UNIX && family != AF_VSOCK) {
            /* Port required for TCP/UDP listen unless random port feature is explicit */
            errx(EXIT_USAGE, "missing port number");
        }

        if (family == AF_UNIX) {
            if (uflag)
                s = unix_bind(host, 0);
            else
                s = unix_listen(host);
        }
        else if (family == AF_VSOCK) {
            s = vsock_listen(vsock_cid, vsock_port);
        }

        if (usetls) {
            tls_config_verify_client_optional(tls_cfg);
            if ((tls_ctx = tls_server()) == NULL) {
                tls_config_free(tls_cfg);
                errx(EXIT_RUNTIME, "tls server creation failed");
            }
            if (tls_configure(tls_ctx, tls_cfg) == -1) {
                tls_free(tls_ctx);
                tls_config_free(tls_cfg);
                errx(EXIT_RUNTIME, "tls configuration failed (%s)", tls_error(tls_ctx));
            }
        }
        /* Allow only one connection at a time, but stay alive. */
        for (;;) {
            if (family != AF_UNIX && family != AF_VSOCK) {
                if (s != -1)
                    close(s);
                s = local_listen(host, uport, hints);
                if (s != -1 && bpf_prog_path) {
                    if (attach_bpf_prog(s, bpf_prog_path) == -1) {
                        close(s);
                        if (tls_ctx)
                            tls_free(tls_ctx);
                        if (tls_cfg)
                            tls_config_free(tls_cfg);
                        errx(EXIT_RUNTIME, "bpf attach failed");
                    }
                }
            }
            if (s == -1)
                err(EXIT_RUNTIME, NULL);
            if (uflag && keepopen) {
                if (family == AF_UNIX) {
                }
                /*
                 * For UDP and --keep-open, don't connect the socket,
                 * let it receive datagrams from multiple
                 * socket pairs.
                 */
                do_readwrite(s, NULL);
            }
            else if (uflag && !keepopen) {
                /*
                 * For UDP and not --keep-open, we will use recvfrom()
                 * initially to wait for a caller, then use
                 * the regular functions to talk to the caller.
                 */
                int rv;
                char buf[2048];
                struct sockaddr_storage z;

                len = sizeof(z);
                rv = recvfrom(s, buf, sizeof(buf), MSG_PEEK, (struct sockaddr*)&z, &len);
                if (rv == -1) {
                    if (usetls) {
                        tls_free(tls_ctx);
                        tls_config_free(tls_cfg);
                    }
                    close(s);
                    err(EXIT_RUNTIME, "recvfrom");
                }

                rv = direct_connect(s, (struct sockaddr*)&z, len);
                if (rv == -1) {
                    if (usetls) {
                        tls_free(tls_ctx);
                        tls_config_free(tls_cfg);
                    }
                    close(s);
                    err(EXIT_RUNTIME, "connect");
                }

                if (family == AF_UNIX) {
                }
                if (vflag)
                    report_sock("Connection received", (struct sockaddr*)&z, len, family == AF_UNIX ? host : NULL);

                if (usetls) {
                    struct tls* tls_cctx = NULL;
                    if ((tls_cctx = tls_setup_server(tls_ctx, s, host)))
                        do_readwrite(s, tls_cctx);
                    if (!tls_cctx)
                        do_readwrite(s, NULL);
                    if (tls_cctx) {
                        timeout_tls(s, tls_cctx, tls_close);
                        tls_free(tls_cctx);
                    }
                }
                else {
                    do_readwrite(s, NULL);
                }
            }
            else {
                struct tls* tls_cctx = NULL;
                int connfd;

                len = sizeof(cliaddr);
                connfd = accept4(s, (struct sockaddr*)&cliaddr, &len, SOCK_NONBLOCK);
                if (connfd == -1) {
                    /* For now, all errnos are fatal */
                    if (usetls) {
                        tls_free(tls_ctx);
                        tls_config_free(tls_cfg);
                    }
                    close(s);
                    err(EXIT_RUNTIME, "accept");
                }
                if (proxy_proto)
                    recv_proxy(connfd);
                if (vflag)
                    report_sock("Connection received", (struct sockaddr*)&cliaddr, len,
                                family == AF_UNIX ? host : NULL);
                if ((usetls) && (tls_cctx = tls_setup_server(tls_ctx, connfd, host)))
                    do_readwrite(connfd, tls_cctx);
                if (!usetls)
                    do_readwrite(connfd, NULL);
                if (tls_cctx)
                    timeout_tls(s, tls_cctx, tls_close);
                close(connfd);
                tls_free(tls_cctx);
            }

            if (!keepopen)
                break;
        }
    }
    else if (family == AF_UNIX) {
        ret = 0;

        if ((s = unix_connect(host)) > 0) {
            if (!zflag)
                do_readwrite(s, NULL);
            close(s);
        }
        else {
            warn("%s", host);
            ret = 1;
        }

        if (uflag)
            unlink(unix_dg_tmp_socket);
        free(proxy_alloc);
        return ret;
    }
    else if (family == AF_VSOCK) {
        ret = 0;

        if ((s = vsock_connect(vsock_cid, vsock_port)) > 0) {
            if (!zflag)
                do_readwrite(s, NULL);
            close(s);
        }
        else {
            warn("vsock %s:%s", vsock_cid, vsock_port);
            ret = 1;
        }
        free(proxy_alloc);
        return ret;
    }
    else {
        int i = 0;

        /* Construct the portlist[] array. */
        build_ports(uport);

        /* Cycle through portlist, connecting to each port. */
        for (s = -1, i = 0; portlist[i] != NULL; i++) {
            if (s != -1)
                close(s);
            tls_free(tls_ctx);
            tls_ctx = NULL;

            if (usetls) {
                if ((tls_ctx = tls_client()) == NULL) {
                    tls_config_free(tls_cfg);
                    errx(EXIT_RUNTIME, "tls client creation failed");
                }
                if (tls_configure(tls_ctx, tls_cfg) == -1) {
                    tls_free(tls_ctx);
                    tls_config_free(tls_cfg);
                    errx(EXIT_RUNTIME, "tls configuration failed (%s)", tls_error(tls_ctx));
                }
            }
            if (xflag)
                s = socks_connect(host, portlist[i], hints, proxy, proxyport, proxyhints, socksv, Pflag);
            else
                s = remote_connect(host, portlist[i], hints, ipaddr);

            if (s == -1)
                continue;

            if (bpf_prog_path) {
                if (attach_bpf_prog(s, bpf_prog_path) == -1) {
                    close(s);
                    if (tls_ctx)
                        tls_free(tls_ctx);
                    if (tls_cfg)
                        tls_config_free(tls_cfg);
                    errx(EXIT_RUNTIME, "bpf attach failed");
                }
            }

            if (send_proxy)
                send_proxy_v2(s);

            ret = 0;
            if (vflag || zflag || quic_probe) {
                int print_info = 1;

                /* For UDP, make sure we are connected. */
                if (uflag) {
                    if (quic_probe) {
                        if (quic_test(s, host, portlist[i]) == 1) {
                            if (!zflag && !quietflag)
                                fprintf(stderr, "QUIC Connection to %s %s succeeded!\n", host, portlist[i]);
                            print_info = 0; /* Already printed */
                        }
                        else {
                            ret = 1;
                            print_info = 0;
                            if (vflag)
                                warnx("QUIC probe to %s %s failed", host, portlist[i]);
                        }
                    }
                    /* No info on failed or skipped test. */
                    else if ((print_info = udptest(s)) == -1) {
                        ret = 1;
                        continue;
                    }
                }
                if (print_info == 1 && !quietflag)
                    connection_info(s, host, portlist[i], uflag ? "udp" : "tcp", ipaddr);
            }
            if (quic_probe) {
                close(s);
                continue;
            }
            if (Fflag)
                fdpass(s);
            else {
                if (usetls)
                    tls_setup_client(tls_ctx, s, host);
                if (!zflag)
                    do_readwrite(s, tls_ctx);
                if (tls_ctx)
                    timeout_tls(s, tls_ctx, tls_close);
            }
        }
    }

    if (s != -1)
        close(s);

#ifdef GAPING_SECURITY_HOLE
    if (child_pid != -1) {
        int status;
        waitpid(child_pid, &status, 0);
        if (WIFEXITED(status))
            ret = WEXITSTATUS(status);
        else if (WIFSIGNALED(status))
            ret = 128 + WTERMSIG(status);
    }
#endif

    tls_free(tls_ctx);
    tls_config_free(tls_cfg);
    free(proxy_alloc);

    return ret;
}
