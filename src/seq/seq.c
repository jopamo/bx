#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <stdbool.h>
#include <math.h>
#include "applets.h"
#include "diag.h"

int bx_seq_main(int argc, char** argv) {
    static const struct option long_options[] = {{"format", required_argument, NULL, 'f'}, {"separator", required_argument, NULL, 's'}, {"equal-width", no_argument, NULL, 'w'},
                                                 {"help", no_argument, NULL, 'h'},         {"version", no_argument, NULL, 'v'},         {NULL, 0, NULL, 0}};

    const char* format = NULL;
    const char* separator = "\n";
    bool equal_width = false;
    int c;
    while ((c = getopt_long(argc, argv, "f:s:w", long_options, NULL)) != -1) {
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
                // ...
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

    if (!format)
        format = "%g";

    double val = first;
    bool first_out = true;

    // Simple loop for now. GNU seq has more complex termination rules to avoid floating point drift issues.
    if (inc > 0) {
        while (val <= last + inc / 1000.0) {  // Tiny epsilon
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
