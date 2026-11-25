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
#include "lib/option_scan.h"
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
    static const struct bx_option_spec specs[] = {
        {'u', 'u', "udp", BX_OPTION_NO_VALUE},
        {'d', 'd', "delay", BX_OPTION_REQUIRED_VALUE},
        {'4', '4', "ipv4", BX_OPTION_NO_VALUE},
        {'6', '6', "ipv6", BX_OPTION_NO_VALUE},
        {'v', 'v', "verbose", BX_OPTION_NO_VALUE},
        {'h', 'h', "help", BX_OPTION_NO_VALUE},
        {'V', 'V', "version", BX_OPTION_NO_VALUE},
    };
    bool udp = false, verbose = false;
    int family = AF_UNSPEC, status = 1;
    uintmax_t delay = 0;
    const char **operands = calloc((size_t)argc, sizeof(*operands));
    if (!operands) { perror("knock"); return 1; }
    size_t num_operands = 0;
    struct knock_port *ports = NULL;
    struct bx_option_cursor cursor;
    bx_option_cursor_init(&cursor, argv, 1, (size_t)argc, BX_OPTION_ORDERED | BX_OPTION_ABBREVIATE,
                          specs, sizeof(specs) / sizeof(specs[0]));
    struct bx_option_result option;
    while ((option = bx_option_next(&cursor)).event != BX_OPTION_END) {
        if (option.event == BX_OPTION_OPERAND) {
            operands[num_operands++] = option.token;
            continue;
        }
        if (option.event != BX_OPTION_MATCH) {
            fprintf(stderr, "knock: %s: %s\n", option.token,
                    option.event == BX_OPTION_MISSING_VALUE ? "option requires an argument" :
                    option.event == BX_OPTION_UNEXPECTED_VALUE ? "option does not take an argument" :
                    "unrecognized option");
            usage(stderr);
            goto done;
        }
        switch (option.spec->id) {
        case 'u': udp = true; break;
        case 'v': verbose = true; break;
        case '4': family = AF_INET; break;
        case '6': family = AF_INET6; break;
        case 'd':
            if (!bx_size_parse_uint(option.value, &delay) || delay > INT_MAX) {
                fputs("knock: invalid delay\n", stderr);
                goto done;
            }
            break;
        case 'h': usage(stdout); status = 0; goto done;
        case 'V': printf("knock (bx) %s\n", BX_VERSION); status = 0; goto done;
        default: usage(stderr); goto done;
        }
    }
    if (num_operands < 2) { usage(stderr); goto done; }
    const char *host = operands[0];
    if (host[0] == '\0') { fputs("knock: empty host\n", stderr); goto done; }
    size_t count = num_operands - 1;
    ports = calloc(count, sizeof(*ports));
    if (!ports) { perror("knock"); goto done; }
    /* Validate the complete sequence before sending its first packet. */
    for (size_t i = 0; i < count; ++i) {
        const char *arg = operands[i + 1], *colon = strchr(arg, ':');
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
    free(operands);
    return status;
}
