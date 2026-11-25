#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>
#include "applets.h"
#include "bx/diag.h"
#include "lib/cli_common.h"
#include "lib/option_scan.h"

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
    static const struct bx_option_spec specs[] = {
        {'a', 'a', "all", BX_OPTION_NO_VALUE},
        {'s', 's', "kernel-name", BX_OPTION_NO_VALUE},
        {'n', 'n', "nodename", BX_OPTION_NO_VALUE},
        {'r', 'r', "kernel-release", BX_OPTION_NO_VALUE},
        {'v', 'v', "kernel-version", BX_OPTION_NO_VALUE},
        {'m', 'm', "machine", BX_OPTION_NO_VALUE},
        {'p', 'p', "processor", BX_OPTION_NO_VALUE},
        {'i', 'i', "hardware-platform", BX_OPTION_NO_VALUE},
        {'o', 'o', "operating-system", BX_OPTION_NO_VALUE},
        {'h', 0, "help", BX_OPTION_NO_VALUE},
        {'V', 0, "version", BX_OPTION_NO_VALUE},
    };

    const char* progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "uname");
    struct bx_diag_ctx diag = {
        .progname = progname,
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };
    int opt_a = 0, opt_s = 0, opt_n = 0, opt_r = 0, opt_v = 0, opt_m = 0, opt_p = 0, opt_i = 0, opt_o = 0;

    const char* extra = NULL;
    struct bx_option_cursor cursor;
    bx_option_cursor_init(&cursor, argv, argc > 0 ? 1u : 0u, (size_t)argc, BX_OPTION_ORDERED | BX_OPTION_ABBREVIATE, specs, sizeof(specs) / sizeof(specs[0]));
    struct bx_option_result option;
    while ((option = bx_option_next(&cursor)).event != BX_OPTION_END) {
        if (option.event == BX_OPTION_OPERAND) {
            if (!extra)
                extra = option.token;
            continue;
        }
        if (option.event != BX_OPTION_MATCH) {
            if (option.short_name)
                bx_diag(&diag, "invalid option -- '%c'", option.short_name);
            else
                bx_diag(&diag, "unrecognized option '%s'", option.token);
            bx_cli_print_try_help(progname);
            return 1;
        }
        switch (option.spec->id) {
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
            default:
                return 1;
        }
    }

    if (extra) {
        bx_cli_diag_extra_operand(&diag, extra);
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
        const char* os_name = strcmp(u.sysname, "k1g4") == 0 ? "k1g4" : "GNU/Linux";
        printf("%s%s", first ? "" : " ", os_name);
    }
    printf("\n");

    return 0;
}
