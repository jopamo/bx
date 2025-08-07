#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <getopt.h>
#include "applets.h"
#include "bx/diag.h"

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

    int opt_a = 0, opt_s = 0, opt_n = 0, opt_r = 0, opt_v = 0, opt_m = 0, opt_p = 0, opt_i = 0, opt_o = 0;
    int c;

    opterr = 0;
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
                printf("Usage: %s [OPTION]...\n", argv[0]);
                printf("Print certain system information.  With no OPTION, same as -s.\n");
                printf("\n");
                printf("  -a, --all                print all information, in the following order,\n");
                printf("                             except omit -p and -i if unknown:\n");
                printf("  -s, --kernel-name        print the kernel name\n");
                printf("  -n, --nodename           print the network node hostname\n");
                printf("  -r, --kernel-release     print the kernel release\n");
                printf("  -v, --kernel-version     print the kernel version\n");
                printf("  -m, --machine            print the machine hardware name\n");
                printf("  -p, --processor          print the processor type (non-portable)\n");
                printf("  -i, --hardware-platform  print the hardware platform (non-portable)\n");
                printf("  -o, --operating-system   print the operating system\n");
                printf("      --help          display this help and exit\n");
                printf("      --version       output version information and exit\n");
                return 0;
            case 'V':
                printf("uname (bx) %s\n", BX_VERSION);
                return 0;
            case '?':
                bx_err("invalid option -- '%c'", optopt);
                printf("Try '%s --help' for more information.\n", argv[0]);
                return 1;
            default:
                return 1;
        }
    }

    if (opt_a) {
        opt_s = opt_n = opt_r = opt_v = opt_m = opt_p = opt_i = opt_o = 1;
    }

    if (!(opt_s || opt_n || opt_r || opt_v || opt_m || opt_p || opt_i || opt_o)) {
        opt_s = 1;
    }

    struct utsname u;
    if (uname(&u) == -1) {
        bx_perror("uname");
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
