/*
 *  knock.c
 *
 *  Copyright (c) 2004-2012 by Judd Vinet <jvinet@zeroflux.org>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <netdb.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "dispatch/applets.h"
#include "lib/size_parse.h"

struct knock_port {
    char service[6];
    bool udp;
};

static void usage(FILE *out) {
    fputs("Usage: knock [options] host port[:tcp|udp] ...\n"
          "  -u, --udp          use UDP for all ports\n"
          "  -d, --delay MS     delay between knocks\n"
          "  -4, --ipv4        use IPv4\n"
          "  -6, --ipv6        use IPv6\n"
          "  -v, --verbose     report each knock\n"
          "  -h, --help        show help\n"
          "  -V, --version     show version\n", out);
}

int bx_knock_main(int argc, char **argv) {
    static const struct option options[] = {
        {"udp", no_argument, NULL, 'u'}, {"delay", required_argument, NULL, 'd'},
        {"ipv4", no_argument, NULL, '4'}, {"ipv6", no_argument, NULL, '6'},
        {"verbose", no_argument, NULL, 'v'}, {"help", no_argument, NULL, 'h'},
        {"version", no_argument, NULL, 'V'}, {NULL, 0, NULL, 0}
    };
    bool udp = false, verbose = false;
    int family = AF_UNSPEC, opt;
    uintmax_t delay = 0;
    while ((opt = getopt_long(argc, argv, "ud:46vhV", options, NULL)) != -1) {
        switch (opt) {
        case 'u': udp = true; break;
        case 'v': verbose = true; break;
        case '4': family = AF_INET; break;
        case '6': family = AF_INET6; break;
        case 'd':
            if (!bx_size_parse_uint(optarg, &delay) || delay > INT_MAX) {
                fputs("knock: invalid delay\n", stderr);
                return 1;
            }
            break;
        case 'h': usage(stdout); return 0;
        case 'V': printf("knock (bx) %s\n", BX_VERSION); return 0;
        default: usage(stderr); return 1;
        }
    }
    if (argc - optind < 2) { usage(stderr); return 1; }
    const char *host = argv[optind++];
    size_t count = (size_t)(argc - optind);
    struct knock_port *ports = calloc(count, sizeof(*ports));
    if (!ports) { perror("knock"); return 1; }
    int status = 1;
    /* Validate the complete sequence before sending its first packet. */
    for (size_t i = 0; i < count; ++i) {
        const char *arg = argv[optind + (int)i], *colon = strchr(arg, ':');
        size_t len = colon ? (size_t)(colon - arg) : strlen(arg);
        if (!len || len >= sizeof(ports[i].service) ||
            (colon && strcmp(colon + 1, "tcp") && strcmp(colon + 1, "udp"))) {
            fprintf(stderr, "knock: invalid port: %s\n", arg);
            goto done;
        }
        memcpy(ports[i].service, arg, len);
        uintmax_t port;
        if (!bx_size_parse_uint(ports[i].service, &port) || !port || port > UINT16_MAX) {
            fprintf(stderr, "knock: invalid port: %s\n", arg);
            goto done;
        }
        ports[i].udp = udp || (colon && !strcmp(colon + 1, "udp"));
    }
    for (size_t i = 0; i < count; ++i) {
        struct addrinfo hints = {
            .ai_family = family,
            .ai_socktype = ports[i].udp ? SOCK_DGRAM : SOCK_STREAM,
            .ai_flags = AI_NUMERICSERV
        }, *address = NULL;
        int error = getaddrinfo(host, ports[i].service, &hints, &address);
        if (error) {
            fprintf(stderr, "knock: %s: %s\n", host, gai_strerror(error));
            goto done;
        }
        int fd = socket(address->ai_family,
                        hints.ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK,
                        address->ai_protocol);
        if (fd < 0) { perror("knock: socket"); freeaddrinfo(address); goto done; }
        if (verbose) {
            char ip[NI_MAXHOST];
            error = getnameinfo(address->ai_addr, address->ai_addrlen, ip, sizeof(ip),
                                NULL, 0, NI_NUMERICHOST);
            if (error) strcpy(ip, host[0] ? "?" : "");
            printf("hitting %s %s:%s\n", ports[i].udp ? "udp" : "tcp",
                   ip, ports[i].service);
        }
        int result;
        if (ports[i].udp)
            result = sendto(fd, "", 1, 0, address->ai_addr, address->ai_addrlen) == 1 ? 0 : -1;
        else
            result = connect(fd, address->ai_addr, address->ai_addrlen);
        int saved = errno;
        close(fd);
        freeaddrinfo(address);
        /* A closed TCP port is a valid knock, not a failed connection test. */
        if (result < 0 && (ports[i].udp ||
            (saved != EINPROGRESS && saved != ECONNREFUSED))) {
            errno = saved;
            perror("knock: send");
            goto done;
        }
        if (delay && i + 1 < count) {
            struct timespec wait = {(time_t)(delay / 1000), (long)(delay % 1000) * 1000000};
            while (nanosleep(&wait, &wait) < 0) {
                if (errno != EINTR) { perror("knock: delay"); goto done; }
            }
        }
    }
    status = fflush(stdout) == 0 && !ferror(stdout) ? 0 : 1;
done:
    free(ports);
    return status;
}
