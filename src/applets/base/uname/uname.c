#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <getopt.h>
#include "applets.h"
#include "bx/diag.h"
#include "lib/cli_common.h"

static void bx_uname_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]...\n", progname);
    fprintf(stream, "Print certain system information.  With no OPTION, same as -s.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -a, --all                print all information, in the following order,\n");
    fprintf(stream, "                             except omit -p and -i if unknown:\n");
    fprintf(stream, "  -s, --kernel-name        print the kernel name\n");
    fprintf(stream, "  -n, --nodename           print the network node hostname\n");
    fprintf(stream, "  -r, --kernel-release     print the kernel release\n");
    fprintf(stream, "  -v, --kernel-version     print the kernel version\n");
    fprintf(stream, "  -m, --machine            print the machine hardware name\n");
    fprintf(stream, "  -p, --processor          print the processor type (non-portable)\n");
    fprintf(stream, "  -i, --hardware-platform  print the hardware platform (non-portable)\n");
    fprintf(stream, "  -o, --operating-system   print the operating system\n");
    fprintf(stream, "      --help          display this help and exit\n");
    fprintf(stream, "      --version       output version information and exit\n");
}

int bx_uname_main(int argc, char** argv) {
    static const struct option long_options[] = {{"all", no_argument, NULL, 'a'},
                                                 {"kernel-name", no_argument, NULL, 's'},
                                                 {"nodename", no_argument, NULL, 'n'},
                                                 {"kernel-release", no_argument, NULL, 'r'},
                                                 {"kernel-version", no_argument, NULL, 'v'},
                                                 {"machine", no_argument, NULL, 'm'},
                                                 {"processor", no_argument, NULL, 'p'},
                                                 {"hardware-platform", no_argument, NULL, 'i'},
                                                 {"operating-system", no_argument, NULL, 'o'},
                                                 {"help", no_argument, NULL, 'h'},
                                                 {"version", no_argument, NULL, 'V'},
                                                 {NULL, 0, NULL, 0}};

    const char* progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "uname");
    struct bx_diag_ctx diag = {
        .progname = progname,
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };
    int opt_a = 0, opt_s = 0, opt_n = 0, opt_r = 0, opt_v = 0, opt_m = 0, opt_p = 0, opt_i = 0, opt_o = 0;
    int c;

    opterr = 0;
    optind = 1;
    while ((c = getopt_long(argc, argv, "asnrvmpio", long_options, NULL)) != -1) {
        switch (c) {
            case 'a':
                opt_a = 1;
                break;
            case 's':
                opt_s = 1;
                break;
            case 'n':
                opt_n = 1;
                break;
            case 'r':
                opt_r = 1;
                break;
            case 'v':
                opt_v = 1;
                break;
            case 'm':
                opt_m = 1;
                break;
            case 'p':
                opt_p = 1;
                break;
            case 'i':
                opt_i = 1;
                break;
            case 'o':
                opt_o = 1;
                break;
            case 'h':
                bx_uname_print_help(stdout, progname);
                return 0;
            case 'V':
                bx_cli_print_version(progname);
                return 0;
            case '?':
                bx_cli_diag_unrecognized_option(&diag, optopt, optind, argc, argv);
                bx_cli_print_try_help(progname);
                return 1;
            default:
                return 1;
        }
    }

    if (optind < argc) {
        bx_cli_diag_extra_operand(&diag, argv[optind]);
        bx_cli_print_try_help(progname);
        return 1;
    }

    if (opt_a) {
        opt_s = opt_n = opt_r = opt_v = opt_m = opt_p = opt_i = opt_o = 1;
    }

    if (!(opt_s || opt_n || opt_r || opt_v || opt_m || opt_p || opt_i || opt_o)) {
        opt_s = 1;
    }

    struct utsname u;
    if (uname(&u) == -1) {
        perror(progname);
        return 1;
    }

    int first = 1;
    if (opt_s) {
        printf("%s%s", first ? "" : " ", u.sysname);
        first = 0;
    }
    if (opt_n) {
        printf("%s%s", first ? "" : " ", u.nodename);
        first = 0;
    }
    if (opt_r) {
        printf("%s%s", first ? "" : " ", u.release);
        first = 0;
    }
    if (opt_v) {
        printf("%s%s", first ? "" : " ", u.version);
        first = 0;
    }
    if (opt_m) {
        printf("%s%s", first ? "" : " ", u.machine);
        first = 0;
    }

    // Processor and Hardware platform are often "unknown"
    const char* p = "unknown";
    const char* i_val = "unknown";

    // On Linux, processor and hardware platform are often not provided by uname()
    // but some implementations use the machine type if they can't find better.
    // GNU uname -p and -i often return the same as -m or "unknown".

    if (opt_p) {
        if (!opt_a || strcmp(p, "unknown") != 0) {
            printf("%s%s", first ? "" : " ", p);
            first = 0;
        }
    }
    if (opt_i) {
        if (!opt_a || strcmp(i_val, "unknown") != 0) {
            printf("%s%s", first ? "" : " ", i_val);
            first = 0;
        }
    }
    if (opt_o) {
        printf("%s%s", first ? "" : " ", "GNU/Linux");
        first = 0;
    }
    printf("\n");

    return 0;
}
