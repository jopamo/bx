#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"
#include "lib/args_common.h"
#include "lib/cli_common.h"
#include "lib/daemon_ops.h"
#include "lib/fd_ops.h"
#include "lib/pidfile_ops.h"
#include "lib/size_parse.h"
#include "lib/system/syslog_config.h"
#include "lib/system/syslog_core.h"
#include "lib/xreadwrite.h"

enum {
    RECV_CAP = 16384,
    PARSE_CAP = RECV_CAP * 2 + 1,
    OUTPUT_CAP = PARSE_CAP + 256,
    DNS_RETRY_SEC = 120,
};

struct options {
    bool foreground, compact, strip_time, dedup, local, kmsg, explicit_config;
    const char *output, *config, *socket, *pidfile;
    unsigned level, backups;
    uint64_t rotate_size;
    struct bx_syslog_remote *remotes;
    size_t remote_count, remote_capacity;
};

struct state {
    struct options opt;
    struct bx_syslog_config config;
    struct bx_syslog_sink default_sink;
    struct bx_pidfile pidfile;
    struct bx_syslog_counters counters;
    int socket_fd, signal_fd, kmsg_fd;
    char *bound_path;
    dev_t bound_dev;
    ino_t bound_ino;
    char hostname[65];
    unsigned char recvbuf[RECV_CAP + 1], lastbuf[RECV_CAP], parsebuf[PARSE_CAP];
    char outbuf[OUTPUT_CAP];
    size_t lastlen;
    bool have_last;
};

static void usage(FILE *f, const char *p) {
    fprintf(f,
        "Usage: %s [OPTIONS]\n"
        "System logging utility\n\n"
        " -n             Run in foreground\n"
        " -R HOST[:PORT] Log to HOST:PORT (default PORT:514)\n"
        " -L             Log locally and via network\n"
        " -K             Log to /dev/kmsg\n"
        " -O FILE        Log to FILE (default /var/log/messages, stdout if -)\n"
        " -s SIZE        Max size (KB) before rotation (default 200KB, 0=off)\n"
        " -b N           Rotated logs to keep (default 1, max 99, 0=purge)\n"
        " -l N           Log priorities below N (1-8)\n"
        " -S             Smaller output\n"
        " -t             Strip client-generated timestamps\n"
        " -D             Drop duplicates\n"
        " -f FILE        Use FILE as config (default /etc/syslog.conf)\n"
        " -m MIN         Accept legacy MARK interval without emitting MARK\n"
        " --socket PATH  Local socket (default /dev/log)\n",
        p);
}

static uint64_t monotonic_sec(void) {
    struct timespec ts;
    return clock_gettime(CLOCK_MONOTONIC, &ts) == 0 ? (uint64_t)ts.tv_sec : 0;
}

static bool add_remote(struct options *o, const char *spec) {
    if (o->remote_count == o->remote_capacity) {
        size_t n = o->remote_capacity ? o->remote_capacity * 2 : 4;
        void *p = realloc(o->remotes, n * sizeof(*o->remotes));
        if (!p) return false;
        o->remotes = p; o->remote_capacity = n;
    }
    struct bx_syslog_remote *r = &o->remotes[o->remote_count];
    memset(r, 0, sizeof(*r)); r->fd = -1;
    const char *begin = spec, *end = NULL, *port = "514";
    if (*spec == '[') {
        begin++; end = strchr(begin, ']');
        if (!end || (end[1] && end[1] != ':')) { errno = EINVAL; return false; }
        if (end[1] == ':') port = end + 2;
    } else {
        const char *a = strchr(spec, ':'), *b = strrchr(spec, ':');
        if (a && a == b) { end = a; port = a + 1; }
    }
    if (!end) end = spec + strlen(spec);
    if (end == begin || !*port) { errno = EINVAL; return false; }
    r->host = strndup(begin, (size_t)(end - begin));
    r->port = strdup(port);
    if (!r->host || !r->port) { free(r->host); free(r->port); return false; }
    o->remote_count++;
    return true;
}

static void options_destroy(struct options *o) {
    for (size_t i = 0; i < o->remote_count; i++) {
        struct bx_syslog_remote *r = &o->remotes[i];
        if (r->fd >= 0) close(r->fd);
        if (r->addresses) freeaddrinfo(r->addresses);
        free(r->host); free(r->port);
    }
    free(o->remotes);
}

static bool parse_options(int argc, char **argv, struct options *o,
                          struct bx_diag_ctx *d) {
    static const struct option longs[] = {
        {"socket", required_argument, NULL, 256},
        {"pidfile", required_argument, NULL, 257},
        {"help", no_argument, NULL, 'h'}, {NULL, 0, NULL, 0}
    };
    memset(o, 0, sizeof(*o));
    o->output = "/var/log/messages"; o->config = "/etc/syslog.conf";
    o->socket = "/dev/log"; o->pidfile = "/run/syslogd.pid";
    o->level = 8; o->rotate_size = 200 * 1024;
    o->backups = 1;
    bx_args_getopt_reset();
    int c;
    while ((c = bx_args_getopt_long(argc, argv,
             ":nO:l:StDs:b:R:Lf:Km:C::h", longs, NULL)) != -1) {
        uintmax_t n;
        switch (c) {
        case 'n': o->foreground = true; break;
        case 'O': o->output = optarg; break;
        case 'l':
            if (!bx_size_parse_uint(optarg, &n) || n < 1 || n > 8)
                goto badnum;
            o->level = (unsigned)n; break;
        case 'S': o->compact = true; break;
        case 't': o->strip_time = true; break;
        case 'D': o->dedup = true; break;
        case 's':
            if (!bx_size_parse_uint(optarg, &n) || n > UINT64_MAX / 1024)
                goto badnum;
            o->rotate_size = (uint64_t)n * 1024; break;
        case 'b':
            if (!bx_size_parse_uint(optarg, &n) || n > 99) goto badnum;
            o->backups = (unsigned)n; break;
        case 'R':
            if (!add_remote(o, optarg)) {
                bx_diag(d, "invalid remote destination '%s'", optarg);
                return false;
            }
            break;
        case 'L': o->local = true; break;
        case 'f': o->config = optarg; o->explicit_config = true; break;
        case 'K': o->kmsg = true; break;
        case 'm':
            if (!bx_size_parse_uint(optarg, &n) || n > INT_MAX / 60)
                goto badnum;
            break;
        case 'C':
            bx_diag(d, "-C is unsupported: BusyBox logread IPC is not implemented");
            return false;
        case 256: o->socket = optarg; break;
        case 257: o->pidfile = optarg; break;
        case 'h': usage(stdout, d->progname); return false;
        case ':': bx_cli_diag_option_requires_arg(d, optopt, optind, argc, argv);
                  return false;
        default: bx_cli_diag_unrecognized_option(d, optopt, optind, argc, argv);
                 return false;
        }
        continue;
badnum:
        bx_diag(d, "invalid numeric argument '%s'", optarg);
        return false;
    }
    if (optind != argc) { bx_cli_diag_extra_operand(d, argv[optind]); return false; }
    if (!o->remote_count) o->local = true;
    return true;
}

static char *resolve_socket(const char *input) {
    char *cur = strdup(input);
    for (unsigned depth = 0; cur && depth < 16; depth++) {
        struct stat st;
        if (lstat(cur, &st) != 0) return errno == ENOENT ? cur : (free(cur), NULL);
        if (!S_ISLNK(st.st_mode)) return cur;
        char target[PATH_MAX + 1];
        ssize_t n = readlink(cur, target, PATH_MAX);
        if (n < 0 || n == PATH_MAX) { free(cur); errno = ENAMETOOLONG; return NULL; }
        target[n] = '\0';
        char *next = NULL;
        if (*target == '/') next = strdup(target);
        else {
            char *slash = strrchr(cur, '/');
            size_t dl = slash ? (size_t)(slash - cur + 1) : 0;
            next = malloc(dl + (size_t)n + 1);
            if (next) { memcpy(next, cur, dl); memcpy(next + dl, target, (size_t)n + 1); }
        }
        free(cur); cur = next;
    }
    free(cur); errno = ELOOP; return NULL;
}

static bool bind_local(struct state *s, struct bx_diag_ctx *d) {
    s->bound_path = resolve_socket(s->opt.socket);
    if (!s->bound_path) { bx_perror_path(d, s->opt.socket); return false; }
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    if (strlen(s->bound_path) >= sizeof(a.sun_path)) {
        errno = ENAMETOOLONG; bx_perror_path(d, s->bound_path); return false;
    }
    strcpy(a.sun_path, s->bound_path);
    struct stat st;
    if (lstat(s->bound_path, &st) == 0) {
        if (!S_ISSOCK(st.st_mode)) {
            bx_diag(d, "%s: refusing to unlink non-socket", s->bound_path);
            return false;
        }
        if (unlink(s->bound_path) != 0) { bx_perror_path(d, s->bound_path); return false; }
    } else if (errno != ENOENT) { bx_perror_path(d, s->bound_path); return false; }
    s->socket_fd = bx_fd_socket_cloexec(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (s->socket_fd < 0 ||
        bind(s->socket_fd, (struct sockaddr *)&a, sizeof(a)) != 0 ||
        chmod(s->bound_path, 0666) != 0 ||
        lstat(s->bound_path, &st) != 0) {
        bx_perror_path(d, s->bound_path); return false;
    }
    s->bound_dev = st.st_dev; s->bound_ino = st.st_ino;
    return true;
}

static bool setup_signals(struct state *s, struct bx_diag_ctx *d) {
    sigset_t mask; sigemptyset(&mask);
    sigaddset(&mask, SIGTERM); sigaddset(&mask, SIGINT); sigaddset(&mask, SIGHUP);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) != 0) {
        bx_diag(d, "cannot block signals: %s", bx_strerror(errno)); return false;
    }
    s->signal_fd = bx_fd_signalfd_cloexec(-1, &mask, SFD_NONBLOCK);
    if (s->signal_fd < 0) {
        bx_diag(d, "cannot create signal fd: %s", bx_strerror(errno)); return false;
    }
    return true;
}

static void timestamp(char out[16]) {
    time_t now = time(NULL); struct tm tm;
    if (!localtime_r(&now, &tm) || strftime(out, 16, "%b %e %H:%M:%S", &tm) != 15)
        memcpy(out, "Jan  1 00:00:00", 16);
}

static bool sink_open(struct bx_syslog_sink *sink) {
    if (sink->kind == BX_SYSLOG_SINK_STDOUT) { sink->fd = 1; return true; }
    int fd = bx_fd_open_cloexec(sink->path,
        O_WRONLY | O_CREAT | O_APPEND | O_NOCTTY | O_NONBLOCK, 0666);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0) { if (fd >= 0) close(fd); return false; }
    sink->fd = fd; sink->device = st.st_dev; sink->inode = st.st_ino;
    sink->regular = S_ISREG(st.st_mode);
    sink->size = st.st_size > 0 ? (uint64_t)st.st_size : 0;
    return true;
}

static void sink_recheck(struct bx_syslog_sink *sink) {
    if (sink->fd < 0 || sink->fd == 1 || !sink->regular) return;
    struct stat st;
    if (stat(sink->path, &st) != 0 || st.st_dev != sink->device ||
        st.st_ino != sink->inode) {
        close(sink->fd); sink->fd = -1; sink->regular = false; sink->size = 0;
    }
}

static bool rotate_sink(struct state *s, struct bx_syslog_sink *sink) {
    if (s->opt.backups) {
        for (unsigned i = s->opt.backups; i > 1; i--) {
            char *old = NULL, *new = NULL;
            if (asprintf(&old, "%s.%u", sink->path, i - 2) < 0 ||
                asprintf(&new, "%s.%u", sink->path, i - 1) < 0) {
                free(old); free(new); return false;
            }
            (void)rename(old, new); free(old); free(new);
        }
        char *zero = NULL;
        if (asprintf(&zero, "%s.0", sink->path) < 0) return false;
        (void)rename(sink->path, zero); free(zero);
    }
    (void)unlink(sink->path);
    if (sink->fd >= 0) close(sink->fd);
    sink->fd = -1; sink->size = 0; sink->regular = false;
    return true;
}

static bool fallback_write(const char *p, size_t n) {
    int fd = bx_fd_open_cloexec("/dev/console",
        O_WRONLY | O_NOCTTY | O_NONBLOCK, 0);
    if (fd < 0) fd = 2;
    bool ok = bx_xwrite_all(fd, p, n);
    if (fd != 2) close(fd);
    return ok;
}

static bool sink_write(struct state *s, struct bx_syslog_sink *sink,
                       const char *p, size_t n) {
    sink_recheck(sink);
    if (sink->fd < 0 && !sink_open(sink)) {
        s->counters.output_failures++; return fallback_write(p, n);
    }
    if (s->opt.rotate_size && sink->regular && sink->size > s->opt.rotate_size &&
        (!rotate_sink(s, sink) || !sink_open(sink))) {
        s->counters.output_failures++; return fallback_write(p, n);
    }
    if (!bx_xwrite_all(sink->fd, p, n)) {
        s->counters.output_failures++;
        if (sink->fd != 1) { close(sink->fd); sink->fd = -1; }
        return false;
    }
    if (sink->regular) sink->size += n;
    return true;
}

static bool resolve_remote(struct bx_syslog_remote *r) {
    struct addrinfo hints = {0}, *a = NULL;
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_DGRAM;
    int rc = getaddrinfo(r->host, r->port, &hints, &a);
    if (rc) {
        r->unresolved++; r->retry_after_ms = (monotonic_sec() + DNS_RETRY_SEC) * 1000;
        return false;
    }
    r->addresses = a; return true;
}

static bool send_remote(struct bx_syslog_remote *r, const void *p, size_t n) {
    if (!r->addresses && monotonic_sec() * 1000 < r->retry_after_ms) return false;
    if (!r->addresses && !resolve_remote(r)) return false;
    struct addrinfo *a = r->addresses;
    if (r->fd < 0)
        r->fd = bx_fd_socket_cloexec(a->ai_family, SOCK_DGRAM | SOCK_NONBLOCK,
                                     a->ai_protocol);
    if (r->fd < 0 || sendto(r->fd, p, n, MSG_DONTWAIT | MSG_NOSIGNAL,
                            a->ai_addr, a->ai_addrlen) != (ssize_t)n) {
        r->failed++;
        if (errno == ECONNRESET || errno == ENOTCONN || errno == EPIPE ||
            errno == EAFNOSUPPORT) {
            if (r->fd >= 0) close(r->fd);
            r->fd = -1; freeaddrinfo(r->addresses); r->addresses = NULL;
            r->retry_after_ms = (monotonic_sec() + DNS_RETRY_SEC) * 1000;
        }
        return false;
    }
    r->sent++; return true;
}

static bool emit_local(struct state *s, struct bx_syslog_record *r) {
    if (s->opt.kmsg) {
        const unsigned char *message = r->message;
        size_t message_len = r->message_len;
        if (r->has_client_timestamp && message_len >= 16u) {
            message += 16u;
            message_len -= 16u;
        }
        int k = snprintf(s->outbuf, sizeof(s->outbuf), "<%d>", r->pri);
        if (k < 0 || (size_t)k + message_len + 1 > sizeof(s->outbuf))
            return false;
        memcpy(s->outbuf + k, message, message_len);
        s->outbuf[(size_t)k + message_len] = '\n';
        if (!bx_xwrite_all(s->kmsg_fd, s->outbuf,
                           (size_t)k + message_len + 1))
            return false;
        s->counters.locally_emitted_records++; return true;
    }
    char ts[16]; size_t len; timestamp(ts);
    if (!bx_syslog_format_record(r, s->hostname, ts, s->opt.compact,
            s->opt.strip_time, s->outbuf, sizeof(s->outbuf), &len))
        return false;
    bool matched = false;
    for (size_t i = 0; i < s->config.rule_count; i++) {
        struct bx_syslog_rule *rule = &s->config.rules[i];
        if (bx_syslog_rule_matches(rule, r->pri)) {
            matched = true;
            (void)sink_write(s, &s->config.sinks[rule->sink_index], s->outbuf, len);
        }
    }
    if (!matched) {
        if ((unsigned)LOG_PRI(r->pri) >= s->opt.level) {
            s->counters.filtered_records++; return true;
        }
        (void)sink_write(s, &s->default_sink, s->outbuf, len);
    }
    s->counters.locally_emitted_records++; return true;
}

static void internal_log(struct state *s, const char *text) {
    struct bx_syslog_record r;
    if (bx_syslog_record_parse((const unsigned char *)text, strlen(text),
            s->parsebuf, sizeof(s->parsebuf), &r)) {
        r.pri = LOG_SYSLOG | LOG_INFO;
        if (s->opt.local) (void)emit_local(s, &r);
    }
}

static void process_datagram(struct state *s, size_t len, bool truncated) {
    s->counters.received_datagrams++;
    len = bx_syslog_trim_datagram(s->recvbuf, len);
    if (!len) return;
    if (truncated) s->counters.truncated_datagrams++;
    if (s->opt.dedup && s->have_last && len == s->lastlen &&
        !memcmp(s->recvbuf, s->lastbuf, len)) {
        s->counters.duplicate_drops++; return;
    }
    if (s->opt.dedup) {
        memcpy(s->lastbuf, s->recvbuf, len); s->lastlen = len; s->have_last = true;
    }
    s->recvbuf[len] = '\n';
    for (size_t i = 0; i < s->opt.remote_count; i++)
        if (!send_remote(&s->opt.remotes[i], s->recvbuf, len + 1))
            s->counters.remote_send_failures++;
    if (s->opt.local) {
        size_t off = 0;
        while (off < len) {
            size_t n = 0;
            while (off + n < len && s->recvbuf[off + n]) n++;
            struct bx_syslog_record r;
            if (bx_syslog_record_parse(s->recvbuf + off, n, s->parsebuf,
                    sizeof(s->parsebuf), &r)) (void)emit_local(s, &r);
            off += n + 1;
        }
    }
    if (truncated && (s->counters.truncated_datagrams == 1 ||
        s->counters.truncated_datagrams % 1024 == 0))
        internal_log(s, "input datagram truncated at 16384 bytes");
}

static void reload_config(struct state *s) {
    struct bx_syslog_config candidate; char error[512];
    if (!bx_syslog_config_load(&candidate, s->opt.config,
            !s->opt.explicit_config, error, sizeof(error))) {
        char msg[600]; snprintf(msg, sizeof(msg), "configuration reload failed: %s", error);
        internal_log(s, msg); return;
    }
    for (size_t i = 0; i < s->config.sink_count; i++) {
        if (s->config.sinks[i].fd >= 0) close(s->config.sinks[i].fd);
        s->config.sinks[i].fd = -1;
    }
    struct bx_syslog_config old = s->config;
    s->config = candidate;
    bx_syslog_config_destroy(&old);
    internal_log(s, "configuration reloaded");
}

static int event_loop(struct state *s) {
    struct pollfd p[2] = {{s->socket_fd, POLLIN, 0}, {s->signal_fd, POLLIN, 0}};
    int term = 0;
    while (!term) {
        if (poll(p, 2, -1) < 0) { if (errno == EINTR) continue; break; }
        if (p[1].revents & POLLIN) {
            struct signalfd_siginfo si;
            while (read(s->signal_fd, &si, sizeof(si)) == sizeof(si)) {
                if (si.ssi_signo == SIGHUP) reload_config(s);
                else if (si.ssi_signo == SIGTERM || si.ssi_signo == SIGINT)
                    term = (int)si.ssi_signo;
            }
        }
        if (!term && (p[0].revents & POLLIN)) {
            struct iovec iov = {s->recvbuf, RECV_CAP};
            struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1};
            ssize_t n = recvmsg(s->socket_fd, &msg, MSG_DONTWAIT);
            if (n >= 0) process_datagram(s, (size_t)n, msg.msg_flags & MSG_TRUNC);
        }
    }
    return term;
}

static void cleanup(struct state *s) {
    bx_pidfile_release(&s->pidfile);
    if (s->bound_path) {
        struct stat st;
        if (!lstat(s->bound_path, &st) && st.st_dev == s->bound_dev &&
            st.st_ino == s->bound_ino) (void)unlink(s->bound_path);
    }
    if (s->socket_fd >= 0) close(s->socket_fd);
    if (s->signal_fd >= 0) close(s->signal_fd);
    if (s->kmsg_fd >= 0) close(s->kmsg_fd);
    if (s->default_sink.fd >= 0 && s->default_sink.fd != 1)
        close(s->default_sink.fd);
    free(s->default_sink.path); free(s->bound_path);
    bx_syslog_config_destroy(&s->config); options_destroy(&s->opt);
}

int bx_syslogd_main(int argc, char **argv) {
    struct bx_diag_ctx d = {.progname = bx_cli_progname(
        argc ? argv[0] : NULL, "syslogd")};
    struct state s; memset(&s, 0, sizeof(s));
    s.socket_fd = s.signal_fd = s.kmsg_fd = s.default_sink.fd = -1;
    bx_pidfile_init(&s.pidfile); bx_syslog_config_init(&s.config);
    if (!parse_options(argc, argv, &s.opt, &d)) { cleanup(&s); return d.exit_status; }
    char error[512];
    if (!bx_syslog_config_load(&s.config, s.opt.config, !s.opt.explicit_config,
            error, sizeof(error))) {
        bx_diag(&d, "%s", error); cleanup(&s); return 1;
    }
    s.default_sink.path = strdup(s.opt.output);
    s.default_sink.kind = !strcmp(s.opt.output, "-")
        ? BX_SYSLOG_SINK_STDOUT : BX_SYSLOG_SINK_FILE;
    if (!s.default_sink.path) { bx_diag(&d, "out of memory"); cleanup(&s); return 1; }
    if (gethostname(s.hostname, sizeof(s.hostname))) strcpy(s.hostname, "localhost");
    s.hostname[64] = '\0'; char *dot = strchr(s.hostname, '.'); if (dot) *dot = '\0';
    if (s.opt.kmsg && (s.kmsg_fd = bx_fd_open_cloexec(
            "/dev/kmsg", O_WRONLY | O_NONBLOCK, 0)) < 0) {
        bx_perror_path(&d, "/dev/kmsg"); cleanup(&s); return 1;
    }
    if (!bind_local(&s, &d) || !setup_signals(&s, &d)) { cleanup(&s); return 1; }
    if (!s.opt.foreground) {
        int rc = bx_daemonize(true, s.default_sink.kind == BX_SYSLOG_SINK_STDOUT);
        if (rc < 0) { bx_diag(&d, "cannot daemonize: %s", bx_strerror(errno));
                      cleanup(&s); return 1; }
        if (rc > 0) return 0;
    }
    if (!bx_pidfile_acquire(&s.pidfile, s.opt.pidfile)) {
        bx_perror_path(&d, s.opt.pidfile); cleanup(&s); return 1;
    }
    internal_log(&s, "syslogd started: bx");
    int sig = event_loop(&s);
    internal_log(&s, "syslogd exiting: bx");
    cleanup(&s);
    if (sig) {
        signal(sig, SIG_DFL); sigset_t mask; sigemptyset(&mask); sigaddset(&mask, sig);
        (void)sigprocmask(SIG_UNBLOCK, &mask, NULL); raise(sig);
        return 128 + sig;
    }
    return 1;
}
