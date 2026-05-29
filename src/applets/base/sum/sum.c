#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdint.h>
#include "applets.h"
#include "bx/diag.h"
#include "lib/cli_common.h"
#include "lib/args_common.h"

static void sum_bsd(FILE* f, const char* name, bool print_name) {
    uint16_t checksum = 0;
    long long total_bytes = 0;
    int c;
    while ((c = getc(f)) != EOF) {
        checksum = (checksum >> 1) + ((checksum & 1) << 15);
        checksum += (uint8_t)c;
        total_bytes++;
    }
    printf("%05u %5lld%s%s\n", checksum, (total_bytes + 1023) / 1024, print_name ? " " : "", name ? name : "");
}

static void sum_sysv(FILE* f, const char* name, bool print_name) {
    uint32_t s = 0;
    long long total_bytes = 0;
    int c;
    while ((c = getc(f)) != EOF) {
        s += (uint8_t)c;
        total_bytes++;
    }
    uint32_t r = (s & 0xffff) + (s >> 16);
    uint16_t checksum = (r & 0xffff) + (r >> 16);
    printf("%u %lld%s%s\n", checksum, (total_bytes + 511) / 512, print_name ? " " : "", name ? name : "");
}

int bx_sum_main(int argc, char** argv) {
    static const struct option long_options[] = {{"sysv", no_argument, NULL, 's'}, {"help", no_argument, NULL, 'h'}, {"version", no_argument, NULL, 'v'}, {NULL, 0, NULL, 0}};
    struct bx_diag_ctx diag = {
        .progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "sum"),
        .exit_status = 1,
    };

    bool sysv = false;
    int c;
    bx_args_getopt_reset();
    while ((c = bx_args_getopt_long(argc, argv, ":rshv", long_options, NULL)) != -1) {
        switch (c) {
            case 'r':
                sysv = false;
                break;
            case 's':
                sysv = true;
                break;
            case 'h':
                printf("Usage: %s [OPTION]... [FILE]...\n", argv[0]);
                printf("Print checksum and block counts for each FILE.\n");
                printf("\n");
                printf("  -r              use BSD sum algorithm, 1K blocks (default)\n");
                printf("  -s, --sysv      use System V sum algorithm, 512B blocks\n");
                printf("      --help          display this help and exit\n");
                printf("      --version       output version information and exit\n");
                return 0;
            case 'v':
                printf("sum (bx) %s\n", BX_VERSION);
                return 0;
            default:
                bx_cli_diag_unrecognized_option(&diag, optopt, optind, argc, argv);
                return 1;
        }
    }

    if (optind == argc) {
        if (sysv)
            sum_sysv(stdin, NULL, false);
        else
            sum_bsd(stdin, NULL, false);
        return 0;
    }

    for (int i = optind; i < argc; i++) {
        FILE* f = fopen(argv[i], "r");
        if (!f) {
            bx_perror_path(&diag, argv[i]);
            continue;
        }
        if (sysv)
            sum_sysv(f, argv[i], true);
        else
            sum_bsd(f, argv[i], true);
        fclose(f);
    }

    return 0;
}
