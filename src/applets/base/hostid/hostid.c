#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <getopt.h>
#include <stdbool.h>
#include <string.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>

#include "applets.h"
#include "bx/diag.h"
#include "bx/libbx.h"
#include "lib/cli_common.h"
#include "lib/args_common.h"
#include "lib/fd_ops.h"

static bool bx_hostid_try_file(uint32_t* id_out) {
    int fd = bx_fd_open_cloexec("/etc/hostid", O_RDONLY, 0);
    if (fd < 0) {
        return false;
    }

    int32_t id = 0;
    ssize_t nread = read(fd, &id, sizeof(id));
    close(fd);

    if (nread != (ssize_t)sizeof(id)) {
        return false;
    }

    *id_out = (uint32_t)id;
    return true;
}

static bool bx_hostid_try_hostname(uint32_t* id_out) {
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname) - 1) != 0) {
        return false;
    }
    hostname[sizeof(hostname) - 1] = '\0';

    if (hostname[0] == '\0') {
        return false;
    }

    struct hostent* host = gethostbyname(hostname);
    if (host == NULL || host->h_addrtype != AF_INET || host->h_addr_list == NULL ||
        host->h_addr_list[0] == NULL || host->h_length <= 0) {
        return false;
    }

    struct in_addr addr = {0};
    size_t copy_len = sizeof(addr);
    if ((size_t)host->h_length < copy_len) {
        copy_len = (size_t)host->h_length;
    }
    memcpy(&addr, host->h_addr_list[0], copy_len);

    uint32_t value = addr.s_addr;
    *id_out = (value << 16) | (value >> 16);
    return true;
}

static uint32_t bx_hostid_value(void) {
    uint32_t id = 0;
    if (bx_hostid_try_file(&id)) {
        return id;
    }

    if (bx_hostid_try_hostname(&id)) {
        return id;
    }

    return 0;
}

static void bx_hostid_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]\n", progname);
    fprintf(stream, "Print the numeric identifier (in hexadecimal) for the current host.\n");
    fprintf(stream, "\n");
    fprintf(stream, "On Linux, bx uses /etc/hostid when present; otherwise it derives the\n");
    fprintf(stream, "value from the first IPv4 address for the current hostname.\n");
    fprintf(stream, "\n");
    fprintf(stream, "      --help     display this help and exit\n");
    fprintf(stream, "      --version  output version information and exit\n");
}

int bx_hostid_main(int argc, char** argv) {
    static const struct option long_options[] = {
        {"help", no_argument, NULL, 1},
        {"version", no_argument, NULL, 2},
        {NULL, 0, NULL, 0},
    };

    int c;
    while ((c = bx_args_getopt_long(argc, argv, "", long_options, NULL)) != -1) {
        switch (c) {
            case 1:
                bx_hostid_print_help(stdout, "hostid");
                return 0;
            case 2:
                bx_cli_print_version("hostid");
                return 0;
            default:
                return 1;
        }
    }

    if (optind < argc) {
        fprintf(stderr, "hostid: extra operand '%s'\n", argv[optind]);
        return 1;
    }

    printf("%08lx\n", (unsigned long)bx_hostid_value());

    return 0;
}
