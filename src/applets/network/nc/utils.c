#include "netcat.h"
#include <errno.h>
#include <arpa/inet.h>
#ifdef __linux__
#include <sys/timerfd.h>
#endif

#include "lib/fd_ops.h"
#include "lib/poll_deadline.h"
#include "lib/random_bytes.h"
#include "lib/time_parse.h"

uint32_t (*nc_random)(void) = nc_random_u32;

static int nc_msec_to_timespec(int timeout_ms, struct timespec* ts) {
    if (ts == NULL)
        return 0;

    if (timeout_ms <= 0) {
        ts->tv_sec = 0;
        ts->tv_nsec = 0;
        return 1;
    }

    return bx_time_milliseconds_to_timespec(timeout_ms, ts);
}

static int nc_seconds_to_timespec(double seconds, struct timespec* ts) {
    if (ts == NULL)
        return 0;

    if (seconds <= 0) {
        ts->tv_sec = 0;
        ts->tv_nsec = 0;
        return 1;
    }

    return bx_time_seconds_to_timespec(seconds, ts);
}

int nc_sleep_monotonic(double seconds) {
    struct timespec req;

    if (seconds <= 0)
        return 0;

#ifdef __linux__
    {
        int tfd;
        struct itimerspec its;
        uint64_t expirations;

        tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
        if (tfd != -1) {
            memset(&its, 0, sizeof(its));
            if (nc_seconds_to_timespec(seconds, &its.it_value) &&
                timerfd_settime(tfd, 0, &its, NULL) == 0) {
                for (;;) {
                    ssize_t n = read(tfd, &expirations, sizeof(expirations));
                    if (n == (ssize_t)sizeof(expirations)) {
                        close(tfd);
                        return 0;
                    }
                    if (n == -1 && errno == EINTR)
                        continue;
                    break;
                }
            }
            close(tfd);
        }
    }
#endif

    if (!nc_seconds_to_timespec(seconds, &req)) {
        errno = EINVAL;
        return -1;
    }
    while (nanosleep(&req, &req) == -1) {
        if (errno != EINTR)
            return -1;
    }
    return 0;
}

int nc_sleep_milliseconds(int milliseconds) {
    struct timespec req;

    if (milliseconds <= 0)
        return 0;
    if (!nc_msec_to_timespec(milliseconds, &req)) {
        errno = EINVAL;
        return -1;
    }
    while (nanosleep(&req, &req) == -1) {
        if (errno != EINTR)
            return -1;
    }
    return 0;
}

int nc_wait_fd_events_monotonic(int fd, short events, int timeout_ms) {
    struct pollfd pfd[2];
    int nfds = 1;
    int timerfd = -1;

    pfd[0].fd = fd;
    pfd[0].events = events;
    pfd[0].revents = 0;

    if (timeout_ms == 0) {
        int ret = bx_poll_retry_eintr(pfd, 1, 0);
        if (ret <= 0)
            return ret;
        return 1;
    }

#ifdef __linux__
    if (timeout_ms >= 0) {
        struct itimerspec its;

        timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
        if (timerfd != -1) {
            memset(&its, 0, sizeof(its));
            if (nc_msec_to_timespec(timeout_ms, &its.it_value) &&
                timerfd_settime(timerfd, 0, &its, NULL) == 0) {
                pfd[1].fd = timerfd;
                pfd[1].events = POLLIN;
                pfd[1].revents = 0;
                nfds = 2;
            }
            else {
                close(timerfd);
                timerfd = -1;
            }
        }
    }
#endif

    for (;;) {
        int ret;

        pfd[0].revents = 0;
        if (nfds == 2)
            pfd[1].revents = 0;

        if (nfds == 2)
            ret = bx_poll_retry_eintr(pfd, nfds, -1);
        else
            ret = bx_poll_for_milliseconds(pfd, 1, timeout_ms);

        if (ret <= 0) {
            if (timerfd != -1)
                close(timerfd);
            return ret;
        }

        if (pfd[0].revents != 0) {
            if (timerfd != -1)
                close(timerfd);
            return 1;
        }

        if (nfds == 2 && (pfd[1].revents & POLLIN)) {
            uint64_t expirations;

            while (read(timerfd, &expirations, sizeof(expirations)) == -1) {
                if (errno == EINTR)
                    continue;
                break;
            }
            close(timerfd);
            return 0;
        }
    }
}

int is_address(const char* s) {
    struct in_addr in4;
    struct in6_addr in6;

    if (inet_pton(AF_INET, s, &in4) == 1)
        return 1;
    if (inet_pton(AF_INET6, s, &in6) == 1)
        return 1;
    return 0;
}

long long nc_strtonum(const char* numstr, long long minval, long long maxval, const char** errstrp) {
    long long ll = 0;
    char* ep;
    int error = 0;
    static const char* const errors[] = {NULL, "invalid", "too small", "too large"};

    if (minval > maxval)
        error = 1;
    else {
        errno = 0;
        ll = strtoll(numstr, &ep, 10);
        if (numstr == ep || *ep != '\0')
            error = 1;
        else if ((ll == LLONG_MIN && errno == ERANGE) || ll < minval)
            error = 2;
        else if ((ll == LLONG_MAX && errno == ERANGE) || ll > maxval)
            error = 3;
    }

    if (errstrp != NULL)
        *errstrp = errors[error];
    if (error)
        return 0;
    return ll;
}

size_t nc_strlcpy(char* dst, const char* src, size_t dsize) {
    size_t srclen = strlen(src);
    if (dsize != 0) {
        size_t copy = srclen >= dsize ? dsize - 1 : srclen;
        memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return srclen;
}

void nc_random_buf(void* buf, size_t len) {
    if (bx_random_bytes(buf, len))
        return;
    static int seeded = 0;
    if (!seeded) {
        seeded = 1;
        srand((unsigned int)time(NULL) ^ (unsigned int)getpid());
    }
    unsigned char* p = buf;
    for (size_t i = 0; i < len; i++)
        p[i] = (unsigned char)(rand() & 0xff);
}

uint32_t nc_random_u32(void) {
    uint32_t v = 0;
    nc_random_buf(&v, sizeof(v));
    return v;
}

uint32_t nc_random_uniform(uint32_t upper_bound) {
    if (upper_bound == 0)
        return 0;
    uint32_t threshold = (uint32_t)(-upper_bound) % upper_bound;
    for (;;) {
        uint32_t r = nc_random_u32();
        if (r >= threshold)
            return r % upper_bound;
    }
}

int strtoport(char* portstr, int udp) {
    struct servent* entry;
    const char* errstr;
    char* proto;
    int port = -1;

    proto = udp ? "udp" : "tcp";

    port = nc_strtonum(portstr, 1, PORT_MAX, &errstr);
    if (errstr == NULL)
        return port;
    if (strcmp(errstr, "invalid") != 0)
        errx(EXIT_USAGE, "port number %s: %s", errstr, portstr);
    if ((entry = getservbyname(portstr, proto)) == NULL)
        errx(EXIT_USAGE, "service \"%s\" unknown", portstr);
    return ntohs(entry->s_port);
}

/*
 * build_ports()
 * Build an array of ports in portlist[], listing each port
 * that we should try to connect to.
 */
void build_ports(char* p) {
    char* n;
    int hi, lo, cp;
    int x = 0;

    if (isdigit((unsigned char)*p) && (n = strchr(p, '-')) != NULL) {
        *n = '\0';
        n++;

        /* Make sure the ports are in order: lowest->highest. */
        hi = strtoport(n, uflag);
        lo = strtoport(p, uflag);
        if (lo > hi) {
            cp = hi;
            hi = lo;
            lo = cp;
        }

        /*
         * Initialize portlist with a random permutation.  Based on
         * Knuth, as in ip_randomid() in sys/netinet/ip_id.c.
         */
        if (rflag) {
            for (x = 0; x <= hi - lo; x++) {
                cp = (int)nc_random_uniform((uint32_t)(x + 1));
                portlist[x] = portlist[cp];
                if (asprintf(&portlist[cp], "%d", x + lo) == -1)
                    err(EXIT_RUNTIME, "asprintf");
            }
        }
        else { /* Load ports sequentially. */
            for (cp = lo; cp <= hi; cp++) {
                if (asprintf(&portlist[x], "%d", cp) == -1)
                    err(EXIT_RUNTIME, "asprintf");
                x++;
            }
        }
    }
    else {
        char* tmp;

        hi = strtoport(p, uflag);
        if (asprintf(&tmp, "%d", hi) != -1)
            portlist[0] = tmp;
        else {
            err(EXIT_RUNTIME, NULL);
        }
    }
}

#ifdef GAPING_SECURITY_HOLE
void spawn_exec(int net_fd) {
    int pin[2], pout[2];

    if (bx_fd_pipe_cloexec(pin) == -1)
        err(EXIT_RUNTIME, "pipe");
    if (bx_fd_pipe_cloexec(pout) == -1) {
        int saved_errno = errno;
        close(pin[0]);
        close(pin[1]);
        errno = saved_errno;
        err(EXIT_RUNTIME, "pipe");
    }

    switch (child_pid = fork()) {
        case -1:
            err(EXIT_RUNTIME, "fork");
        case 0: /* Child */
            close(net_fd);
            if (bx_fd_dup2_exact(pin[0], STDIN_FILENO) == -1)
                err(EXIT_RUNTIME, "dup2 child stdin");
            close(pin[0]);
            close(pin[1]);

            if (bx_fd_dup2_exact(pout[1], STDOUT_FILENO) == -1)
                err(EXIT_RUNTIME, "dup2 child stdout");
            if (bx_fd_dup2_exact(pout[1], STDERR_FILENO) == -1)
                err(EXIT_RUNTIME, "dup2 child stderr");
            close(pout[0]);
            close(pout[1]);

            execl("/bin/sh", "sh", "-c", exec_path, (char*)NULL);
            if (errno == ENOENT)
                err(EXIT_COMMAND_NOT_FOUND, "execl");
            else
                err(EXIT_EXEC_FAILED, "execl");
        default: /* Parent */
            if (bx_fd_dup2_exact(pin[1], STDOUT_FILENO) == -1)
                err(EXIT_RUNTIME, "dup2 parent stdout");
            close(pin[0]);
            close(pin[1]);

            if (bx_fd_dup2_exact(pout[0], STDIN_FILENO) == -1)
                err(EXIT_RUNTIME, "dup2 parent stdin");
            close(pout[0]);
            close(pout[1]);
    }
}
#endif
