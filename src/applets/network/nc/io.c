#include "netcat.h"
#include "pcap.h"
#include "syscalls.h"
#include <fcntl.h>
#include <math.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "lib/time_parse.h"

static size_t hex_total_in, hex_total_out;
ssize_t (*nc_sendfile_fn)(int out_fd, int in_fd, off_t* offset, size_t count) = direct_sendfile;

static size_t io_buffer_capacity(struct tls* tls_ctx) {
    if (uflag && tls_ctx == NULL)
        return UDP_IO_BUFSIZE;
    return BUFSIZE;
}

static int io_socket_is_stream(int fd) {
    int type;
    socklen_t typelen = sizeof(type);

    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &typelen) == -1)
        return 0;
    return type == SOCK_STREAM;
}

static int io_sendfile_eligible(int net_fd, int stdin_fd, struct tls* tls_ctx) {
    struct stat st;

    if (stdin_fd < 0)
        return 0;
    if (tls_ctx != NULL || uflag || dflag || fuzz_tcp || fuzz_udp)
        return 0;
    if (iflag || jitter || profile || quic_mask)
        return 0;
    if (pcapfile || hex_path)
        return 0;
    if (!io_socket_is_stream(net_fd))
        return 0;
    if (fstat(stdin_fd, &st) == -1)
        return 0;
    return S_ISREG(st.st_mode);
}

static int io_sendfile_is_compat_error(int errnum) {
    return errnum == EINVAL || errnum == ENOSYS || errnum == EOPNOTSUPP || errnum == ENOTSUP || errnum == EXDEV;
}

/* Box-Muller transform to generate Gaussian random numbers */
double gaussian_random(double mean, double stddev) {
    static double V1, V2, S;
    static int phase = 0;
    double X;

    if (phase == 0) {
        do {
            double U1 = (double)nc_random() / UINT32_MAX;
            double U2 = (double)nc_random() / UINT32_MAX;
            V1 = 2 * U1 - 1;
            V2 = 2 * U2 - 1;
            S = V1 * V1 + V2 * V2;
        } while (S >= 1 || S == 0);

        X = V1 * sqrt(-2 * log(S) / S);
    }
    else {
        X = V2 * sqrt(-2 * log(S) / S);
    }

    phase = 1 - phase;
    return X * stddev + mean;
}

static void rolling_xor(const unsigned char* in,
                        size_t len,
                        unsigned char* out,
                        const unsigned char* key,
                        size_t key_len) {
    size_t i;
    for (i = 0; i < len; i++) {
        out[i] = in[i] ^ key[i % key_len];
    }
}

static int base64_encode(const unsigned char* in, size_t in_len, char* out, size_t out_len) {
    static const char set[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char* p = out;
    size_t i;
    unsigned int val = 0;
    int valb = -6;

    if (out_len < (in_len * 4 / 3) + 5)
        return -1;

    for (i = 0; i < in_len; i++) {
        val = (val << 8) + in[i];
        valb += 8;
        while (valb >= 0) {
            *p++ = set[(val >> valb) & 0x3F];
            valb -= 6;
        }
    }
    if (valb > -6)
        *p++ = set[((val << 8) >> (valb + 8)) & 0x3F];
    while ((p - out) % 4)
        *p++ = '=';
    *p = 0;
    return p - out;
}

static void splice_loop(int net_fd) {
    int p_in[2], p_out[2];
    struct pollfd pfd[4];
    int stdin_fd = STDIN_FILENO;
    int stdout_fd = STDOUT_FILENO;
    int n, num_fds;

    if (pipe(p_in) == -1 || pipe(p_out) == -1)
        err(EXIT_RUNTIME, "pipe");

    if (dflag)
        stdin_fd = -1;

    pfd[POLL_STDIN].fd = stdin_fd;
    pfd[POLL_STDIN].events = POLLIN;
    pfd[POLL_NETOUT].fd = net_fd;
    pfd[POLL_NETOUT].events = 0;
    pfd[POLL_NETIN].fd = net_fd;
    pfd[POLL_NETIN].events = POLLIN;
    pfd[POLL_STDOUT].fd = stdout_fd;
    pfd[POLL_STDOUT].events = 0;

    while (1) {
        if (pfd[POLL_STDIN].fd == -1 && pfd[POLL_NETIN].fd == -1)
            return;
        if (pfd[POLL_NETOUT].fd == -1 && pfd[POLL_STDOUT].fd == -1)
            return;

        num_fds = poll(pfd, 4, timeout);
        if (num_fds == -1)
            err(EXIT_RUNTIME, "poll");
        if (num_fds == 0)
            return;

        for (n = 0; n < 4; n++) {
            if (pfd[n].revents & (POLLERR | POLLNVAL))
                pfd[n].fd = -1;
        }

        if (pfd[POLL_STDIN].revents & POLLIN) {
            ssize_t s = splice(pfd[POLL_STDIN].fd, NULL, p_in[1], NULL, BUFSIZE, SPLICE_F_MOVE | SPLICE_F_NONBLOCK);
            if (s > 0) {
                if (splice(p_in[0], NULL, pfd[POLL_NETOUT].fd, NULL, s, SPLICE_F_MOVE) == -1)
                    pfd[POLL_NETOUT].fd = -1;
            }
            else if (s == 0) {
                pfd[POLL_STDIN].fd = -1;
                if (Nflag)
                    shutdown(pfd[POLL_NETOUT].fd, SHUT_WR);
                pfd[POLL_NETOUT].fd = -1;
            }
        }

        if (pfd[POLL_NETIN].revents & POLLIN) {
            ssize_t s = splice(pfd[POLL_NETIN].fd, NULL, p_out[1], NULL, BUFSIZE, SPLICE_F_MOVE | SPLICE_F_NONBLOCK);
            if (s > 0) {
                if (splice(p_out[0], NULL, pfd[POLL_STDOUT].fd, NULL, s, SPLICE_F_MOVE) == -1)
                    pfd[POLL_STDOUT].fd = -1;
            }
            else if (s == 0) {
                pfd[POLL_NETIN].fd = -1;
                pfd[POLL_STDOUT].fd = -1;
            }
        }
    }
}

/*
 * readwrite()
 * Loop that polls on the network file descriptor and stdin.
 */
void readwrite(int net_fd, struct tls* tls_ctx) {
    struct pollfd pfd[4];
    int stdin_fd = STDIN_FILENO;
    int stdout_fd = STDOUT_FILENO;
    int sendfile_stdin_fd = -1;
    int sendfile_enabled = 0;
    size_t iobufsize = io_buffer_capacity(tls_ctx);
    unsigned char* netinbuf = NULL;
    unsigned char* stdinbuf = NULL;
    size_t netinbufpos = 0;
    size_t stdinbufpos = 0;
    int n, num_fds;
    ssize_t ret;
    int mptcp_diag_enabled = (mptcpflag && vflag >= 2 && !uflag);
    struct timespec mptcp_diag_last;

    /* Use aligned heap memory for mprotect compatibility (Foliage Sleep) */
    if (posix_memalign((void**)&netinbuf, 4096, iobufsize))
        err(EXIT_RUNTIME, "memalign");
    if (posix_memalign((void**)&stdinbuf, 4096, iobufsize))
        err(EXIT_RUNTIME, "memalign");

    if (spliceflag && !tls_ctx) {
        free(netinbuf);
        free(stdinbuf);
        splice_loop(net_fd);
        return;
    }

    if (pcapfile)
        pcap_open(net_fd, pcapfile);

    if (hex_path) {
        if (strcmp(hex_path, "-") == 0)
            hex_fp = stderr;
        else if ((hex_fp = fopen(hex_path, "w")) == NULL)
            err(EXIT_RUNTIME, "hex-dump");
        hex_total_in = hex_total_out = 0;
    }

    /* don't read from stdin if requested or fuzzing */
    if (dflag || fuzz_tcp || fuzz_udp)
        stdin_fd = -1;
    sendfile_stdin_fd = stdin_fd;
    sendfile_enabled = io_sendfile_eligible(net_fd, sendfile_stdin_fd, tls_ctx);
    if (sendfile_enabled)
        stdin_fd = -1;

    /* stdin */
    pfd[POLL_STDIN].fd = stdin_fd;
    pfd[POLL_STDIN].events = (stdin_fd == -1) ? 0 : POLLIN;

    /* network out */
    pfd[POLL_NETOUT].fd = net_fd;
    pfd[POLL_NETOUT].events = sendfile_enabled ? POLLOUT : 0;

    /* network in */
    pfd[POLL_NETIN].fd = net_fd;
    pfd[POLL_NETIN].events = POLLIN;

    /* stdout */
    pfd[POLL_STDOUT].fd = stdout_fd;
    pfd[POLL_STDOUT].events = 0;

    if (mptcp_diag_enabled) {
        clock_gettime(CLOCK_MONOTONIC, &mptcp_diag_last);
        report_mptcp_info(net_fd);
    }

    while (1) {
        int sendfile_pending = sendfile_enabled && pfd[POLL_NETOUT].fd != -1;

        /* both inputs are gone, buffers are empty, we are done */
        if (!sendfile_pending && pfd[POLL_STDIN].fd == -1 && pfd[POLL_NETIN].fd == -1 && stdinbufpos == 0 &&
            netinbufpos == 0)
            goto cleanup;
        /* both outputs are gone, we can't continue */
        if (pfd[POLL_NETOUT].fd == -1 && pfd[POLL_STDOUT].fd == -1)
            goto cleanup;
        /* listen and net in gone, queues empty, done */
        if (lflag && pfd[POLL_NETIN].fd == -1 && stdinbufpos == 0 && netinbufpos == 0)
            goto cleanup;

        /* help says -i is for "wait between lines sent". We read and
         * write arbitrary amounts of data, and we don't want to start
         * scanning for newlines, so this is as good as it gets */
        if (iflag) {
            double s = (double)iflag;
            if (jitter) {
                /* Use Gaussian distribution for human-like burstiness */
                s = gaussian_random((double)iflag, (double)jitter / 4.0);
                if (s < 0)
                    s = 0;
            }

            /* Foliage / Timer-Queue Sleep Logic */
            /* 1. Obfuscate buffers (XOR) */
            unsigned char key = 0x55;
            for (size_t k = 0; k < iobufsize; k++) {
                stdinbuf[k] ^= key;
                netinbuf[k] ^= key;
            }
            /* 2. Mark memory inaccessible (PAGE_NOACCESS equivalent) */
            mprotect(stdinbuf, iobufsize, PROT_NONE);
            mprotect(netinbuf, iobufsize, PROT_NONE);

            /* 3. Sleep on monotonic clock for sub-second jitter precision. */
            if (nc_sleep_monotonic(s) == -1)
                err(EXIT_RUNTIME, "jitter sleep");

            /* 4. Wake up & Restore */
            mprotect(stdinbuf, iobufsize, PROT_READ | PROT_WRITE);
            mprotect(netinbuf, iobufsize, PROT_READ | PROT_WRITE);
            for (size_t k = 0; k < iobufsize; k++) {
                stdinbuf[k] ^= key;
                netinbuf[k] ^= key;
            }
        }

        /* try to fill buffer for fuzzing */
        if (((fuzz_tcp && !uflag) || (fuzz_udp && uflag)) && stdinbufpos < iobufsize) {
            nc_random_buf(stdinbuf + stdinbufpos, iobufsize - stdinbufpos);
            stdinbufpos = iobufsize;
            pfd[POLL_NETOUT].events = POLLOUT;
        }

        if (mptcp_diag_enabled) {
            struct timespec now;
            int64_t elapsed_ms;

            if (clock_gettime(CLOCK_MONOTONIC, &now) == 0 &&
                bx_time_timespec_elapsed_milliseconds_int64(&mptcp_diag_last, &now, &elapsed_ms) &&
                elapsed_ms >= 1000) {
                report_mptcp_info(net_fd);
                mptcp_diag_last = now;
            }
        }

        /* poll */
        num_fds = poll(pfd, 4, timeout);

        /* treat poll errors */
        if (num_fds == -1)
            err(EXIT_RUNTIME, "polling error");

        /* timeout happened */
        if (num_fds == 0)
            goto cleanup;

        /* treat socket error conditions */
        for (n = 0; n < 4; n++) {
            if (pfd[n].revents & (POLLERR | POLLNVAL)) {
                pfd[n].fd = -1;
            }
        }
        /* reading is possible after HUP */
        if (pfd[POLL_STDIN].events & POLLIN && pfd[POLL_STDIN].revents & POLLHUP && !(pfd[POLL_STDIN].revents & POLLIN))
            pfd[POLL_STDIN].fd = -1;

        if (pfd[POLL_NETIN].events & POLLIN && pfd[POLL_NETIN].revents & POLLHUP && !(pfd[POLL_NETIN].revents & POLLIN))
            pfd[POLL_NETIN].fd = -1;

        if (pfd[POLL_NETOUT].revents & POLLHUP) {
            if (pfd[POLL_NETOUT].fd != -1 && Nflag)
                shutdown(pfd[POLL_NETOUT].fd, SHUT_WR);
            pfd[POLL_NETOUT].fd = -1;
        }
        /* if HUP, stop watching stdout */
        if (pfd[POLL_STDOUT].revents & POLLHUP)
            pfd[POLL_STDOUT].fd = -1;
        /* if no net out, stop watching stdin */
        if (pfd[POLL_NETOUT].fd == -1) {
            pfd[POLL_STDIN].fd = -1;
            sendfile_enabled = 0;
        }
        /* if no stdout, stop watching net in */
        if (pfd[POLL_STDOUT].fd == -1) {
            if (pfd[POLL_NETIN].fd != -1)
                shutdown(pfd[POLL_NETIN].fd, SHUT_RD);
            pfd[POLL_NETIN].fd = -1;
        }

        /* try to read from stdin */
        if (pfd[POLL_STDIN].revents & POLLIN && stdinbufpos < iobufsize) {
            ret = fillbuf(pfd[POLL_STDIN].fd, stdinbuf, &stdinbufpos, iobufsize, NULL, net_fd);
            if (ret == TLS_WANT_POLLIN)
                pfd[POLL_STDIN].events = POLLIN;
            else if (ret == TLS_WANT_POLLOUT)
                pfd[POLL_STDIN].events = POLLOUT;
            else if (ret == 0 || ret == -1)
                pfd[POLL_STDIN].fd = -1;
            /* read something - poll net out */
            if (stdinbufpos > 0)
                pfd[POLL_NETOUT].events = POLLOUT;
            /* filled buffer - remove self from polling */
            if (stdinbufpos == iobufsize)
                pfd[POLL_STDIN].events = 0;
        }
        if (sendfile_enabled && (pfd[POLL_NETOUT].revents & POLLOUT)) {
            ssize_t sf_ret = nc_sendfile_fn(pfd[POLL_NETOUT].fd, sendfile_stdin_fd, NULL, BUFSIZE);

            if (sf_ret > 0) {
                pfd[POLL_NETOUT].events = POLLOUT;
            }
            else if (sf_ret == 0) {
                sendfile_enabled = 0;
                if (pfd[POLL_NETOUT].fd != -1 && Nflag)
                    shutdown(pfd[POLL_NETOUT].fd, SHUT_WR);
                pfd[POLL_NETOUT].fd = -1;
                pfd[POLL_NETOUT].events = 0;
            }
            else if (errno == EAGAIN || errno == EINTR) {
                pfd[POLL_NETOUT].events = POLLOUT;
            }
            else if (io_sendfile_is_compat_error(errno)) {
                sendfile_enabled = 0;
                pfd[POLL_STDIN].fd = sendfile_stdin_fd;
                pfd[POLL_STDIN].events = POLLIN;
                pfd[POLL_NETOUT].events = 0;
            }
            else {
                sendfile_enabled = 0;
                pfd[POLL_NETOUT].fd = -1;
                pfd[POLL_NETOUT].events = 0;
            }
        }
        /* try to write to network */
        if (!sendfile_enabled && (pfd[POLL_NETOUT].revents & POLLOUT) && stdinbufpos > 0) {
            ret = drainbuf(pfd[POLL_NETOUT].fd, stdinbuf, &stdinbufpos, iobufsize, tls_ctx, net_fd);
            if (ret == TLS_WANT_POLLIN)
                pfd[POLL_NETOUT].events = POLLIN;
            else if (ret == TLS_WANT_POLLOUT)
                pfd[POLL_NETOUT].events = POLLOUT;
            else if (ret == -1)
                pfd[POLL_NETOUT].fd = -1;
            /* buffer empty - remove self from polling */
            if (stdinbufpos == 0)
                pfd[POLL_NETOUT].events = 0;
            /* buffer no longer full - poll stdin again */
            if (stdinbufpos < iobufsize)
                pfd[POLL_STDIN].events = POLLIN;
        }
        /* try to read from network */
        if (pfd[POLL_NETIN].revents & POLLIN && netinbufpos < iobufsize) {
            ret = fillbuf(pfd[POLL_NETIN].fd, netinbuf, &netinbufpos, iobufsize, tls_ctx, net_fd);
            if (ret == TLS_WANT_POLLIN)
                pfd[POLL_NETIN].events = POLLIN;
            else if (ret == TLS_WANT_POLLOUT)
                pfd[POLL_NETIN].events = POLLOUT;
            else if (ret == -1)
                pfd[POLL_NETIN].fd = -1;
            /* eof on net in - remove from pfd */
            if (ret == 0) {
                shutdown(pfd[POLL_NETIN].fd, SHUT_RD);
                pfd[POLL_NETIN].fd = -1;
            }
            if (recvlimit > 0 && ++recvcount >= recvlimit) {
                if (pfd[POLL_NETIN].fd != -1)
                    shutdown(pfd[POLL_NETIN].fd, SHUT_RD);
                pfd[POLL_NETIN].fd = -1;
                pfd[POLL_STDIN].fd = -1;
            }
            /* read something - poll stdout */
            if (netinbufpos > 0)
                pfd[POLL_STDOUT].events = POLLOUT;
            /* filled buffer - remove self from polling */
            if (netinbufpos == iobufsize)
                pfd[POLL_NETIN].events = 0;
        }
        /* try to write to stdout */
        if (pfd[POLL_STDOUT].revents & POLLOUT && netinbufpos > 0) {
            ret = drainbuf(pfd[POLL_STDOUT].fd, netinbuf, &netinbufpos, iobufsize, NULL, net_fd);
            if (ret == TLS_WANT_POLLIN)
                pfd[POLL_STDOUT].events = POLLIN;
            else if (ret == TLS_WANT_POLLOUT)
                pfd[POLL_STDOUT].events = POLLOUT;
            else if (ret == -1)
                pfd[POLL_STDOUT].fd = -1;
            /* buffer empty - remove self from polling */
            if (netinbufpos == 0)
                pfd[POLL_STDOUT].events = 0;
            /* buffer no longer full - poll net in again */
            if (netinbufpos < iobufsize)
                pfd[POLL_NETIN].events = POLLIN;
        }

        /* stdin gone and queue empty? */
        if (!sendfile_enabled && pfd[POLL_STDIN].fd == -1 && stdinbufpos == 0) {
            if (pfd[POLL_NETOUT].fd != -1 && Nflag)
                shutdown(pfd[POLL_NETOUT].fd, SHUT_WR);
            pfd[POLL_NETOUT].fd = -1;
        }
        /* net in gone and queue empty? */
        if (pfd[POLL_NETIN].fd == -1 && netinbufpos == 0) {
            pfd[POLL_STDOUT].fd = -1;
        }
    }
cleanup:
    if (pcapfile)
        pcap_close();
    free(netinbuf);
    free(stdinbuf);
}

ssize_t drainbuf(int fd, unsigned char* buf, size_t* bufpos, size_t buflen, struct tls* tls, int net_fd) {
    ssize_t n;
    ssize_t adjust;
    size_t write_len;

    if (fd == -1)
        return -1;
    if (buflen == 0 || *bufpos > buflen) {
        errno = EINVAL;
        return -1;
    }

    /* Apply Traffic Masking/Shaping when writing to network */
    if (fd == net_fd && (profile || quic_mask)) {
        unsigned char temp_buf[BUFSIZE * 2];
        unsigned char* write_buf = temp_buf;
        size_t original_len = *bufpos;
        write_len = 0;

        /* Preserve UDP datagram semantics by splitting oversized writes. */
        if (uflag && !tls && original_len > UDP_MAX_WRITE_PAYLOAD)
            original_len = UDP_MAX_WRITE_PAYLOAD;
        if (original_len > BUFSIZE)
            original_len = BUFSIZE;

        /* Malleable Profile */
        if (profile) {
            if (strcmp(profile, "html") == 0) {
                snprintf((char*)temp_buf, sizeof(temp_buf), "<!-- %.*s -->", (int)original_len, buf);
                write_len = strlen((char*)temp_buf);
            }
            else if (strcmp(profile, "css") == 0) {
                snprintf((char*)temp_buf, sizeof(temp_buf), "/* %.*s */", (int)original_len, buf);
                write_len = strlen((char*)temp_buf);
            }
            else if (strcmp(profile, "base64-json") == 0) {
                char b64[BUFSIZE * 2];
                if (base64_encode(buf, original_len, b64, sizeof(b64)) != -1) {
                    int prefix_len = snprintf((char*)temp_buf,
                                              sizeof(temp_buf),
                                              "{\"status\": \"success\", \"session_id\": \"89234\", \"debug_trace\": \"");
                    if (prefix_len < 0 || (size_t)prefix_len >= sizeof(temp_buf)) {
                        memcpy(temp_buf, buf, original_len);
                        write_len = original_len;
                    }
                    else {
                        size_t used = (size_t)prefix_len;
                        size_t avail = sizeof(temp_buf) - used;
                        size_t payload_len = strlen(b64);

                        if (avail <= 3)
                            payload_len = 0;
                        else if (payload_len > avail - 3)
                            payload_len = avail - 3;

                        memcpy(temp_buf + used, b64, payload_len);
                        used += payload_len;
                        temp_buf[used++] = '"';
                        temp_buf[used++] = '}';
                        temp_buf[used] = '\0';
                        write_len = used;
                    }
                }
                else {
                    memcpy(temp_buf, buf, original_len);
                    write_len = original_len;
                }
            }
            else if (strcmp(profile, "json-dialect") == 0) {
                char b64[BUFSIZE * 2];
                if (base64_encode(buf, original_len, b64, sizeof(b64)) != -1) {
                    /* Dialect: Looks like a telemetry report */
                    const char* statuses[] = {"active", "idle", "processing", "maintenance"};
                    const char* regions[] = {"us-east-1", "eu-west-1", "ap-southeast-2", "sa-east-1"};
                    int prefix_len = snprintf((char*)temp_buf,
                                              sizeof(temp_buf),
                                              "{\"metadata\":{\"id\":%u,\"region\":\"%s\",\"status\":\"%s\"},\"payload\":\"",
                                              nc_random_uniform(10000),
                                              regions[nc_random_uniform(4)],
                                              statuses[nc_random_uniform(4)]);
                    if (prefix_len < 0 || (size_t)prefix_len >= sizeof(temp_buf)) {
                        memcpy(temp_buf, buf, original_len);
                        write_len = original_len;
                    }
                    else {
                        size_t used = (size_t)prefix_len;
                        size_t avail = sizeof(temp_buf) - used;
                        size_t payload_len = strlen(b64);

                        if (avail <= 3)
                            payload_len = 0;
                        else if (payload_len > avail - 3)
                            payload_len = avail - 3;

                        memcpy(temp_buf + used, b64, payload_len);
                        used += payload_len;
                        temp_buf[used++] = '"';
                        temp_buf[used++] = '}';
                        temp_buf[used] = '\0';
                        write_len = used;
                    }
                }
                else {
                    memcpy(temp_buf, buf, original_len);
                    write_len = original_len;
                }
            }
            else if (strcmp(profile, "xor-mask") == 0) {
                /* 4-byte rolling key: DE AD BE EF */
                unsigned char key[] = {0xDE, 0xAD, 0xBE, 0xEF};
                rolling_xor(buf, original_len, temp_buf, key, sizeof(key));
                write_len = original_len;
                write_buf = temp_buf;
            }
            else {
                /* Unknown profile, just copy */
                memcpy(temp_buf, buf, original_len);
                write_len = original_len;
            }
        }
        else {
            memcpy(temp_buf, buf, original_len);
            write_len = original_len;
        }

        /* QUIC Masking (Padding) */
        if (quic_mask && write_len < 1350 && sizeof(temp_buf) > 1350) {
            memset(write_buf + write_len, 'X', 1350 - write_len);
            write_len = 1350;
        }

        /* Blocking write loop to ensure masked packet is sent intact */
        size_t total_written = 0;
        while (total_written < write_len) {
            ssize_t res;
            if (tls) {
                res = tls_write(tls, write_buf + total_written, write_len - total_written);
                if (res == -1)
                    errx(EXIT_RUNTIME, "tls write failed (%s)", tls_error(tls));
            }
            else {
                res = direct_write(fd, write_buf + total_written, write_len - total_written);
            }

            if (res == -1) {
                if (errno == EAGAIN || errno == EINTR) {
                    if (nc_sleep_milliseconds(1) == -1)
                        return -1;
                    continue;
                }
                return -1;
            }
            if (res == TLS_WANT_POLLIN || res == TLS_WANT_POLLOUT) {
                if (nc_sleep_milliseconds(1) == -1)
                    return -1;
                continue;
            }
            total_written += res;
        }

        /* We pretend we wrote 'original_len' of the input buffer */
        n = original_len;

        if (pcapfile)
            pcap_log(fd, write_buf, write_len, 1);

        if (hex_fp) {
            hexdump(hex_fp, ">", write_buf, write_len, hex_total_out);
            hex_total_out += write_len;
        }
    }
    else {
        write_len = *bufpos;
        if (uflag && fd == net_fd && !tls && write_len > UDP_MAX_WRITE_PAYLOAD)
            write_len = UDP_MAX_WRITE_PAYLOAD;

        if (tls) {
            n = tls_write(tls, buf, write_len);
            if (n == -1)
                errx(EXIT_RUNTIME, "tls write failed (%s)", tls_error(tls));
        }
        else {
            n = direct_write(fd, buf, write_len);
            /* don't treat EAGAIN, EINTR as error */
            if (n == -1 && (errno == EAGAIN || errno == EINTR))
                n = TLS_WANT_POLLOUT;
        }
        if (n <= 0)
            return n;

        if (pcapfile)
            pcap_log(fd, buf, n, 1);

        if (hex_fp && fd == net_fd) {
            hexdump(hex_fp, ">", buf, n, hex_total_out);
            hex_total_out += n;
        }
    }

    /* adjust buffer */
    adjust = *bufpos - n;
    if (adjust > 0)
        memmove(buf, buf + n, adjust);
    *bufpos -= n;
    return n;
}

ssize_t fillbuf(int fd, unsigned char* buf, size_t* bufpos, size_t buflen, struct tls* tls, int net_fd) {
    size_t num;
    ssize_t n;

    if (fd == -1)
        return -1;
    if (buflen == 0 || *bufpos > buflen) {
        errno = EINVAL;
        return -1;
    }
    num = buflen - *bufpos;

    if (tls) {
        n = tls_read(tls, buf + *bufpos, num);
        if (n == -1)
            errx(EXIT_RUNTIME, "tls read failed (%s)", tls_error(tls));
    }
    else {
        n = direct_read(fd, buf + *bufpos, num);
        /* don't treat EAGAIN, EINTR as error */
        if (n == -1 && (errno == EAGAIN || errno == EINTR))
            n = TLS_WANT_POLLIN;
    }
    if (n <= 0)
        return n;

    if (pcapfile)
        pcap_log(fd, buf + *bufpos, n, 0);

    if (hex_fp && fd == net_fd) {
        hexdump(hex_fp, "<", buf + *bufpos, n, hex_total_in);
        hex_total_in += n;
    }

    *bufpos += n;
    return n;
}

/*
 * fdpass()
 * Pass the connected file descriptor to stdout and exit.
 */
void fdpass(int nfd) {
    struct msghdr mh;
    union {
        struct cmsghdr hdr;
        char buf[CMSG_SPACE(sizeof(int))];
    } cmsgbuf;
    struct cmsghdr* cmsg;
    struct iovec iov;
    char c = '\0';
    ssize_t r;
    struct pollfd pfd;

    /* Avoid obvious stupidity */
    if (isatty(STDOUT_FILENO))
        errx(EXIT_RUNTIME, "Cannot pass file descriptor to tty");

    memset(&mh, 0, sizeof(mh));
    memset(&cmsgbuf, 0, sizeof(cmsgbuf));
    memset(&iov, 0, sizeof(iov));

    mh.msg_control = &cmsgbuf.buf;
    mh.msg_controllen = sizeof(cmsgbuf.buf);
    cmsg = CMSG_FIRSTHDR(&mh);
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    *(int*)CMSG_DATA(cmsg) = nfd;

    iov.iov_base = &c;
    iov.iov_len = 1;
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;

    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = STDOUT_FILENO;
    pfd.events = POLLOUT;
    for (;;) {
        r = sendmsg(STDOUT_FILENO, &mh, 0);
        if (r == -1) {
            if (errno == EAGAIN || errno == EINTR) {
                if (poll(&pfd, 1, -1) == -1)
                    err(EXIT_RUNTIME, "poll");
                continue;
            }
            err(EXIT_RUNTIME, "sendmsg");
        }
        else if (r != 1)
            errx(EXIT_RUNTIME, "sendmsg: unexpected return value %zd", r);
        else
            break;
    }
    exit(EXIT_OK);
}
