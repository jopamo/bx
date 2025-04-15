#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include "applets.h"
#include "diag.h"

int bx_sleep_main(int argc, char** argv) {
    if (argc < 2) {
        bx_err("missing operand");
        printf("Try '%s --help' for more information.\n", argv[0]);
        return 1;
    }

    if (argc == 2) {
        if (strcmp(argv[1], "--help") == 0) {
            printf("Usage: %s NUMBER[SUFFIX]...\n", argv[0]);
            printf("Pause for NUMBER seconds.  SUFFIX may be 's' for seconds (the default),\n");
            printf("'m' for minutes, 'h' for hours or 'd' for days.  NUMBER need not be an\n");
            printf("integer.  Given two or more arguments, pause for the amount of time\n");
            printf("specified by the sum of their values.\n");
            printf("\n");
            printf("      --help          display this help and exit\n");
            printf("      --version       output version information and exit\n");
            return 0;
        }
        if (strcmp(argv[1], "--version") == 0) {
            printf("sleep (bx) %s\n", BX_VERSION);
            return 0;
        }
    }

    double total_seconds = 0;
    for (int i = 1; i < argc; i++) {
        char* end;
        errno = 0;
        double val = strtod(argv[i], &end);
        if (end == argv[i] || errno != 0) {
            bx_err("invalid time interval '%s'", argv[i]);
            return 1;
        }

        if (*end != '\0') {
            if (strcmp(end, "s") == 0) {
                // val is in seconds
            }
            else if (strcmp(end, "m") == 0) {
                val *= 60.0;
            }
            else if (strcmp(end, "h") == 0) {
                val *= 3600.0;
            }
            else if (strcmp(end, "d") == 0) {
                val *= 86400.0;
            }
            else {
                bx_err("invalid time interval '%s'", argv[i]);
                return 1;
            }
        }

        if (val < 0) {
            bx_err("invalid time interval '%s'", argv[i]);
            return 1;
        }

        total_seconds += val;
    }

    struct timespec ts;
    ts.tv_sec = (time_t)total_seconds;
    ts.tv_nsec = (long)((total_seconds - (double)ts.tv_sec) * 1000000000.0);

    // Handle potential rounding issues
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    if (ts.tv_nsec < 0) {
        ts.tv_nsec = 0;
    }

    while (nanosleep(&ts, &ts) == -1) {
        if (errno != EINTR) {
            bx_perror("nanosleep");
            return 1;
        }
    }

    return 0;
}
