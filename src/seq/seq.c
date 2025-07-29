#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <stdbool.h>
#include <math.h>
#include "applets.h"
#include "diag.h"

static int count_decimal_places(const char* s) {
    const char* dot = strchr(s, '.');
    if (!dot) return 0;
    int n = 0;
    for (const char* p = dot + 1; *p >= '0' && *p <= '9'; p++)
        n++;
    return n;
}

int bx_seq_main(int argc, char** argv) {
    static const struct option long_options[] = {{"format", required_argument, NULL, 'f'}, {"separator", required_argument, NULL, 's'}, {"equal-width", no_argument, NULL, 'w'},
                                                  {"help", no_argument, NULL, 'h'},         {"version", no_argument, NULL, 'v'},         {NULL, 0, NULL, 0}};

    const char* format = NULL;
    const char* separator = "\n";
    bool equal_width = false;
    int c;
    while ((c = getopt_long(argc, argv, "+f:s:w", long_options, NULL)) != -1) {
        switch (c) {
            case 'f':
                format = optarg;
                break;
            case 's':
                separator = optarg;
                break;
            case 'w':
                equal_width = true;
                break;
            case 'h':
                printf("Usage: %s [OPTION]... LAST\n", argv[0]);
                printf("  or:  %s [OPTION]... FIRST LAST\n", argv[0]);
                printf("  or:  %s [OPTION]... FIRST INCREMENT LAST\n", argv[0]);
                printf("Print numbers from FIRST to LAST, in steps of INCREMENT.\n");
                printf("\n");
                printf("  -f, --format=FORMAT    use printf style floating-point FORMAT\n");
                printf("  -s, --separator=STRING use STRING to separate numbers (default: \\n)\n");
                printf("  -w, --equal-width      equalize width by padding with leading zeroes\n");
                printf("      --help              display this help and exit\n");
                printf("      --version           output version information and exit\n");
                return 0;
            case 'v':
                printf("seq (bx) %s\n", BX_VERSION);
                return 0;
            default:
                return 1;
        }
    }

    double first = 1, inc = 1, last;
    int num_args = argc - optind;
    if (num_args == 1) {
        last = atof(argv[optind]);
    }
    else if (num_args == 2) {
        first = atof(argv[optind]);
        last = atof(argv[optind + 1]);
    }
    else if (num_args == 3) {
        first = atof(argv[optind]);
        inc = atof(argv[optind + 1]);
        last = atof(argv[optind + 2]);
    }
    else {
        bx_err("missing operand");
        return 1;
    }

    if (inc == 0) {
        bx_err("invalid increment '0'");
        return 1;
    }

    int max_prec = 0;
    if (format == NULL) {
        max_prec = count_decimal_places(argv[optind]);
        if (num_args >= 2) { int p = count_decimal_places(argv[optind + 1]); if (p > max_prec) max_prec = p; }
        if (num_args >= 3) { int p = count_decimal_places(argv[optind + 2]); if (p > max_prec) max_prec = p; }
    }

    static char fmt_buf[64];
    if (format == NULL) {
        if (max_prec > 0)
            snprintf(fmt_buf, sizeof(fmt_buf), "%%.%df", max_prec);
        else
            snprintf(fmt_buf, sizeof(fmt_buf), "%%g");
        format = fmt_buf;
    }

    if (equal_width) {
        double max_v = fabs(first) > fabs(last) ? fabs(first) : fabs(last);
        int int_digits = 0;
        long long n = (long long)max_v;
        if (n == 0) int_digits = 1;
        else { while (n) { int_digits++; n /= 10; } }

        int total_width = int_digits;
        if (strchr(format, '.') || max_prec > 0) {
            int prec = max_prec;
            const char* dot = strchr(format, '.');
            if (dot) prec = atoi(dot + 1);
            if (prec > 0)
                total_width = int_digits + 1 + prec;
        }

        snprintf(fmt_buf, sizeof(fmt_buf), "%%0%d.%df", total_width, max_prec > 0 ? max_prec : 0);
    }

    double val = first;
    bool first_out = true;

    if (inc > 0) {
        while (val <= last + inc / 1000.0) {
            if (!first_out)
                fputs(separator, stdout);
            printf(format, val);
            first_out = false;
            val += inc;
        }
    }
    else {
        while (val >= last + inc / 1000.0) {
            if (!first_out)
                fputs(separator, stdout);
            printf(format, val);
            first_out = false;
            val += inc;
        }
    }
    if (!first_out && strcmp(separator, "\n") != 0)
        putchar('\n');
    else if (!first_out && strcmp(separator, "\n") == 0)
        putchar('\n');

    return 0;
}
