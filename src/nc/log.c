#include "netcat.h"
#ifdef __linux__
#include <linux/vm_sockets.h>
#endif

static void canonicalize_event_name(const char* msg, char* out, size_t outsz) {
    size_t w = 0;
    int last_sep = 0;
    const unsigned char* p;

    if (out == NULL || outsz == 0)
        return;
    out[0] = '\0';
    if (msg == NULL)
        return;

    for (p = (const unsigned char*)msg; *p != '\0'; p++) {
        if (isalnum(*p)) {
            if (w + 1 >= outsz)
                break;
            out[w++] = (char)tolower(*p);
            last_sep = 0;
        }
        else if (!last_sep && w > 0) {
            if (w + 1 >= outsz)
                break;
            out[w++] = '_';
            last_sep = 1;
        }
    }

    while (w > 0 && out[w - 1] == '_')
        w--;
    if (w == 0) {
        if (outsz > 5) {
            out[0] = 'e';
            out[1] = 'v';
            out[2] = 'e';
            out[3] = 'n';
            out[4] = 't';
            out[5] = '\0';
        }
        else {
            out[outsz - 1] = '\0';
        }
        return;
    }
    out[w] = '\0';
}

void json_timestamp_now(char* tbuf, size_t tbufsz) {
    time_t now;
    struct tm tm_info;

    if (tbuf == NULL || tbufsz == 0)
        return;
    tbuf[0] = '\0';

    time(&now);
#if defined(_POSIX_THREAD_SAFE_FUNCTIONS)
    if (gmtime_r(&now, &tm_info) == NULL)
        return;
    strftime(tbuf, tbufsz, "%Y-%m-%dT%H:%M:%SZ", &tm_info);
#else
    {
        struct tm* tm_ptr;
        tm_ptr = gmtime(&now);
        if (tm_ptr == NULL)
            return;
        strftime(tbuf, tbufsz, "%Y-%m-%dT%H:%M:%SZ", tm_ptr);
    }
#endif
}

static void json_print_string_or_null(FILE* fp, const char* value) {
    if (value == NULL)
        fprintf(fp, "null");
    else
        fprintf(fp, "\"%s\"", value);
}

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
                      const char* dst_port) {
    char tbuf[32];

    if (fp == NULL || event == NULL)
        return;

    if (level == NULL)
        level = "info";
    if (tls_state == NULL)
        tls_state = "disabled";
    if (quic_state == NULL)
        quic_state = "disabled";

    json_timestamp_now(tbuf, sizeof(tbuf));

    fprintf(fp,
            "{\"timestamp\":\"%s\",\"level\":\"%s\",\"schema_version\":\"1.0\","
            "\"pid\":%ld,\"event\":\"%s\",\"direction\":",
            tbuf, level, (long)getpid(), event);
    json_print_string_or_null(fp, direction);
    fprintf(fp, ",\"protocol\":");
    json_print_string_or_null(fp, protocol);
    fprintf(fp, ",\"tls_state\":");
    json_print_string_or_null(fp, tls_state);
    fprintf(fp, ",\"quic_state\":");
    json_print_string_or_null(fp, quic_state);
    fprintf(fp, ",\"five_tuple\":{\"src_addr\":");
    json_print_string_or_null(fp, src_addr);
    fprintf(fp, ",\"src_port\":");
    json_print_string_or_null(fp, src_port);
    fprintf(fp, ",\"dst_addr\":");
    json_print_string_or_null(fp, dst_addr);
    fprintf(fp, ",\"dst_port\":");
    json_print_string_or_null(fp, dst_port);
    fprintf(fp, "}");
}

static int format_sockaddr_endpoint(const struct sockaddr* sa,
                                    socklen_t salen,
                                    char* addr,
                                    size_t addrsz,
                                    char* port,
                                    size_t portsz) {
#ifdef __linux__
    if (sa != NULL && sa->sa_family == AF_VSOCK) {
        const struct sockaddr_vm* svm;

        if (addrsz == 0 || portsz == 0)
            return -1;
        svm = (const struct sockaddr_vm*)sa;
        snprintf(addr, addrsz, "vsock:%u", svm->svm_cid);
        snprintf(port, portsz, "%u", svm->svm_port);
        return 0;
    }
#endif
    if (sa != NULL && sa->sa_family == AF_UNIX) {
        const struct sockaddr_un* sun;

        if (addrsz == 0 || portsz == 0)
            return -1;
        sun = (const struct sockaddr_un*)sa;
        if (sun->sun_path[0] == '\0')
            snprintf(addr, addrsz, "@%s", sun->sun_path + 1);
        else
            snprintf(addr, addrsz, "%s", sun->sun_path);
        snprintf(port, portsz, "0");
        return 0;
    }

    if (getnameinfo(sa, salen, addr, addrsz, port, portsz, NI_NUMERICHOST | NI_NUMERICSERV) != 0)
        return -1;

    return 0;
}

int json_socket_tuple_from_fd(int fd,
                              char* src_addr,
                              size_t src_addr_len,
                              char* src_port,
                              size_t src_port_len,
                              char* dst_addr,
                              size_t dst_addr_len,
                              char* dst_port,
                              size_t dst_port_len) {
    struct sockaddr_storage local_addr;
    struct sockaddr_storage remote_addr;
    socklen_t local_len;
    socklen_t remote_len;

    if (fd < 0 || src_addr == NULL || src_port == NULL || dst_addr == NULL || dst_port == NULL)
        return -1;

    local_len = sizeof(local_addr);
    remote_len = sizeof(remote_addr);
    if (getsockname(fd, (struct sockaddr*)&local_addr, &local_len) == -1)
        return -1;
    if (getpeername(fd, (struct sockaddr*)&remote_addr, &remote_len) == -1)
        return -1;

    if (format_sockaddr_endpoint((struct sockaddr*)&local_addr, local_len, src_addr, src_addr_len, src_port,
                                 src_port_len) == -1)
        return -1;
    if (format_sockaddr_endpoint((struct sockaddr*)&remote_addr, remote_len, dst_addr, dst_addr_len, dst_port,
                                 dst_port_len) == -1)
        return -1;

    return 0;
}

static const char* report_direction(const char* msg) {
    if (msg != NULL &&
        (strcmp(msg, "Listening") == 0 || strcmp(msg, "Bound") == 0 || strcmp(msg, "Connection received") == 0))
        return "in";
    return "out";
}

static const char* report_protocol(const struct sockaddr* sa, const char* path) {
    if (path != NULL || (sa != NULL && sa->sa_family == AF_UNIX))
        return "unix";
#ifdef __linux__
    if (sa != NULL && sa->sa_family == AF_VSOCK)
        return "vsock";
#endif
    return uflag ? "udp" : "tcp";
}

void report_sock(const char* msg, const struct sockaddr* sa, socklen_t salen, char* path) {
    char host[NI_MAXHOST], port[NI_MAXSERV];
    char cid[32], vmport[32];
    const char* src_addr = NULL;
    const char* src_port = NULL;
    const char* dst_addr = NULL;
    const char* dst_port = NULL;
    const char* direction;
    const char* protocol;
    int herr;
    int flags = NI_NUMERICSERV;
    char event_name[64];
    const char* zero_port = "0";

    canonicalize_event_name(msg, event_name, sizeof(event_name));
    direction = report_direction(msg);
    protocol = report_protocol(sa, path);

    if (path != NULL) {
        if (strcmp(msg, "Connection received") == 0) {
            src_addr = path;
            src_port = zero_port;
        }
        else {
            dst_addr = path;
            dst_port = zero_port;
        }
        if (jflag) {
            json_event_begin(stderr, "info", event_name, direction, protocol, usetls ? "enabled" : "disabled",
                             "disabled", src_addr, src_port, dst_addr, dst_port);
            fprintf(stderr, ",\"message\":\"%s\",\"path\":\"%s\"}\n", msg, path);
        }
        else {
            fprintf(stderr, "%s on %s\n", msg, path);
        }
        return;
    }

    if (nflag)
        flags |= NI_NUMERICHOST;

#ifdef __linux__
    if (sa && sa->sa_family == AF_VSOCK) {
        const struct sockaddr_vm* svm = (const struct sockaddr_vm*)sa;
        snprintf(cid, sizeof(cid), "%u", svm->svm_cid);
        snprintf(vmport, sizeof(vmport), "%u", svm->svm_port);
        if (strcmp(msg, "Connection received") == 0) {
            src_addr = cid;
            src_port = vmport;
        }
        else {
            dst_addr = cid;
            dst_port = vmport;
        }
        if (jflag) {
            json_event_begin(stderr, "info", event_name, direction, protocol, usetls ? "enabled" : "disabled",
                             "disabled", src_addr, src_port, dst_addr, dst_port);
            fprintf(stderr, ",\"message\":\"%s\",\"cid\":%u,\"port\":%u}\n", msg, svm->svm_cid, svm->svm_port);
        }
        else {
            fprintf(stderr, "%s on vsock:%u:%u\n", msg, svm->svm_cid, svm->svm_port);
        }
        return;
    }
#endif

    herr = getnameinfo(sa, salen, host, sizeof(host), port, sizeof(port), flags);
    switch (herr) {
        case 0:
            break;
        case EAI_SYSTEM:
            err(EXIT_RUNTIME, "getnameinfo");
        default:
            errx(EXIT_RUNTIME, "getnameinfo: %s", gai_strerror(herr));
    }

    if (jflag) {
        if (strcmp(msg, "Connection received") == 0) {
            src_addr = host;
            src_port = port;
        }
        else {
            dst_addr = host;
            dst_port = port;
        }
        json_event_begin(stderr, "info", event_name, direction, protocol, usetls ? "enabled" : "disabled", "disabled",
                         src_addr, src_port, dst_addr, dst_port);
        fprintf(stderr, ",\"message\":\"%s\",\"host\":\"%s\",\"port\":\"%s\"}\n", msg, host, port);
    }
    else {
        fprintf(stderr, "%s on %s %s\n", msg, host, port);
    }
}

void help(void) {
    fprintf(stderr, "Netcat 30th anniversary edition\n");
    fprintf(stderr,
            "\n"
            "        /\\_/\\\n"
            "       / 0 0 \\\n"
            "      ====v====\n"
            "       \\  W  /\n"
            "       |     |     _\n"
            "       / ___ \\    /\n"
            "      / /   \\ \\  |\n"
            "     (((-----)))-'\n"
            "      /\n"
            "     (      ___\n"
            "      \\__.=|___E\n"
            "             /\n"
            "\n");
    fprintf(stderr,
            "\n"
            "Usage: nc [options] [destination] [port]\n"
            "\n"
            "Options taking a time assume seconds.\n"
            "\n"
            "  -4                         Use IPv4 only\n"
            "  -6                         Use IPv6 only\n"
            "  -U                         Use UNIX domain socket\n"
            "      --vsock <cid:port>     Use vsock sockets only\n"
            "  -l                         Listen mode, for inbound connects\n"
            "      --keep-open            Accept multiple connections in listen mode\n"
            "  -u                         UDP mode\n"
            "  -c                         Use TLS\n"
            "  -C <certfile>              Public key file\n"
            "  -K <keyfile>               Private key file\n"
            "  -R <CAfile>                CA bundle\n"
            "  -e <name>                  Required name in peer certificate\n"
            "  -H <hash>                  Required hash of peer certificate\n"
            "  -o <file>                  OCSP stapling file\n"
            "  -Z <file>                  Save peer certificate (use \"-\" for stderr)\n"
            "  -n                         Suppress name/port resolutions\n"
            "  -v                         Verbose\n"
            "  -z                         Zero-I/O mode (scan)\n"
            "  -N                         Shutdown network socket after EOF on stdin\n"
            "  -d                         Detach from stdin\n"
            "  -F                         Pass socket fd to stdout and exit\n"
            "  -j                         JSON output\n"
            "      --log-file <file>      Append diagnostic output to a log file\n"
            "      --quiet                Suppress non-error informational output\n"
            "  -i <interval>              Delay between read/write polls\n"
            "      --jitter <seconds>     Add Gaussian-distributed random delay to -i\n"
            "  -w <timeout>               Connect timeout and final net reads\n"
            "  -W <recvlimit>             Terminate after receiving N packets\n"
            "  -p <port>                  Specify local source port\n"
            "  -s <addr>                  Source address\n"
            "  -r                         Randomize remote ports\n"
            "  -M <ttl>                   Set IP TTL / IPv6 hops\n"
            "  -m <minttl>                Set IP minimum TTL / hopcount\n"
            "  -T <keyword|value>         IP TOS/TCLASS or TLS option\n"
            "  -I <length>                TCP receive buffer size\n"
            "  -O <length>                TCP send buffer size\n"
            "  -x <addr[:port]>           Proxy address and port\n"
            "  -X <proto>                 Proxy protocol: \"5\" (SOCKS) or \"connect\"\n"
            "  -P <proxyuser>             Proxy authentication username\n"
            "      --proxy-proto          Expect PROXY protocol v2 header\n"
            "      --send-proxy           Send PROXY protocol v2 header\n"
            "      --pcap <file>          Write a PCAP capture\n"
            "      --pcap-default-path    Write PCAP to a safe auto-generated path\n"
            "      --pcap-snaplen <bytes> Limit captured bytes per packet (1-65535)\n"
            "      --pcap-filter <dir>    Capture direction: in, out, or both\n"
            "      --pcap-rotate-size <bytes>\n"
            "                             Rotate capture after this many bytes\n"
            "      --pcap-rotate-seconds <seconds>\n"
            "                             Rotate capture after this many seconds\n"
            "      --hex-dump <file>      Dump session data as hex\n"
            "      --bpf-prog <file>      Attach BPF program to socket\n"
            "      --bpf-evasion <file>   Load eBPF program to hide process artifacts\n"
            "      --xdp-stealth <iface>  Load XDP program for invisible networking\n"
            "      --mptcp                Enable Multipath TCP\n"
            "      --mptcp-netlink        Enable MPTCP PM netlink transition events\n"
            "      --tfo                  Enable TCP Fast Open\n"
            "      --mark <mark>          Set SO_MARK\n"
            "      --interface <iface>    Bind socket to device\n"
            "      --transparent          Enable IP_TRANSPARENT\n"
            "      --namespace <path>     Network namespace path\n"
            "      --splice               Use zero-copy splice loop\n"
            "      --io-uring             Require io_uring backend (fails if unavailable)\n"
            "      --fuzz-tcp             Send random TCP data\n"
            "      --fuzz-udp             Send random UDP data\n"
            "      --quic                 QUIC probe (UDP)\n"
            "      --quic-mask            Pad UDP packets to ~1350 bytes\n"
            "      --profile <type>       Malleable profile: html, css, base64-json,\n"
            "                             json-dialect, xor-mask\n"
            "      --version              Show version information\n"
            "  -h, --help                 Display this help screen\n"
            "\n");
    exit(EXIT_OK);
}

void usage(int ret) {
    fprintf(stderr,
            "usage: nc [-46cDdFhklNnruUvz] [-C certfile] [-e name] "
            "[-H hash] [-I length]\n"
            "\t  [-i interval] [--jitter s] [-K keyfile] [-M ttl] [-m minttl] [-O length]\n"
            "\t  [--log-file file] [--mptcp-netlink] [--profile type] [--quic-mask] [--quiet]\n"
            "\t  [--pcap file] [--pcap-default-path] [--pcap-snaplen bytes]\n"
            "\t  [--pcap-filter in|out|both] [--pcap-rotate-size bytes]\n"
            "\t  [--pcap-rotate-seconds seconds]\n"
            "\t  [-P proxy_username] [-p source_port]\n"
            "\t  [-R CAfile] [-s sourceaddr] [-T keyword] [-W recvlimit]\n"
            "\t  [-w timeout] [-X proxy_protocol] [-x proxy_address[:port]]\n"
            "\t  [--bpf-evasion file] [--xdp-stealth iface] [--version]\n"
            "\t  [destination] [port]\n");
    if (ret)
        exit(EXIT_USAGE);
}
