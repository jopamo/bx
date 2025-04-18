#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include "applets.h"
#include "diag.h"

typedef struct {
    long long lines;
    long long bytes;
    bool follow;
    bool quiet;
    bool verbose;
    bool zero_terminated;
    double sleep_interval;
} tail_opts_t;

static void tail_bytes(FILE* f, long long n) {
    if (n > 0) {
        // Output starting with byte n (1-indexed)
        if (fseek(f, n - 1, SEEK_SET) == 0) {
            int c;
            while ((c = getc(f)) != EOF)
                putchar(c);
        }
        else {
            // Not seekable, skip n-1 bytes
            for (long long i = 0; i < n - 1; i++) {
                if (getc(f) == EOF)
                    break;
            }
            int c;
            while ((c = getc(f)) != EOF)
                putchar(c);
        }
    }
    else if (n < 0) {
        // Output last -n bytes
        n = -n;
        if (fseek(f, -n, SEEK_END) == 0) {
            int c;
            while ((c = getc(f)) != EOF)
                putchar(c);
        }
        else {
            // Not seekable, use a buffer
            char* buf = malloc(n);
            if (!buf)
                return;
            size_t head = 0;
            size_t total = 0;
            int c;
            while ((c = getc(f)) != EOF) {
                buf[head] = c;
                head = (head + 1) % n;
                total++;
            }
            size_t start = (total > (size_t)n) ? head : 0;
            size_t count = (total > (size_t)n) ? (size_t)n : total;
            for (size_t i = 0; i < count; i++) {
                putchar(buf[(start + i) % n]);
            }
            free(buf);
        }
    }
}

static void tail_lines(FILE* f, long long n, char delimiter) {
    if (n > 0) {
        // Output starting with line n (1-indexed)
        long long current_line = 1;
        int c;
        while (current_line < n && (c = getc(f)) != EOF) {
            if (c == delimiter)
                current_line++;
        }
        while ((c = getc(f)) != EOF)
            putchar(c);
    }
    else if (n < 0) {
        // Output last -n lines
        n = -n;
        if (fseek(f, 0, SEEK_END) == 0) {
            long long pos = ftell(f);
            long long count = 0;
            while (pos > 0 && count <= n) {
                fseek(f, --pos, SEEK_SET);
                if (getc(f) == delimiter) {
                    if (++count > n)
                        break;
                }
            }
            // If we didn't find enough lines, we are at pos=0 (or slightly after)
            // If we found enough, we are at the delimiter of the n+1-th last line
            // or at the start if it's the last line and no delimiter before it.
            // Wait, if we found n+1-th delimiter, we should start AFTER it.
            if (count > n) {
                // We are after the delimiter because of the getc(f)
                // pos was the index of the delimiter.
            }
            else {
                fseek(f, 0, SEEK_SET);
            }
            int c;
            while ((c = getc(f)) != EOF)
                putchar(c);
        }
        else {
            // Not seekable, use a circular buffer of lines
            char** lines = calloc(n, sizeof(char*));
            size_t* lens = calloc(n, sizeof(size_t));
            size_t head = 0;
            size_t total = 0;

            char* line = NULL;
            size_t line_cap = 0;
            ssize_t len;
            while ((len = getdelim(&line, &line_cap, delimiter, f)) != -1) {
                if (lines[head])
                    free(lines[head]);
                lines[head] = malloc(len + 1);
                memcpy(lines[head], line, len + 1);
                lens[head] = len;
                head = (head + 1) % n;
                total++;
            }
            free(line);

            size_t start = (total > (size_t)n) ? head : 0;
            size_t count = (total > (size_t)n) ? (size_t)n : total;
            for (size_t i = 0; i < count; i++) {
                fwrite(lines[(start + i) % n], 1, lens[(start + i) % n], stdout);
                free(lines[(start + i) % n]);
            }
            free(lines);
            free(lens);
        }
    }
}

int bx_tail_main(int argc, char** argv) {
    static const struct option long_options[] = {{"bytes", required_argument, NULL, 'c'},
                                                 {"lines", required_argument, NULL, 'n'},
                                                 {"follow", optional_argument, NULL, 'f'},
                                                 {"quiet", no_argument, NULL, 'q'},
                                                 {"silent", no_argument, NULL, 'q'},
                                                 {"verbose", no_argument, NULL, 'v'},
                                                 {"zero-terminated", no_argument, NULL, 'z'},
                                                 {"sleep-interval", required_argument, NULL, 's'},
                                                 {"help", no_argument, NULL, 'h'},
                                                 {"version", no_argument, NULL, 'V'},
                                                 {NULL, 0, NULL, 0}};

    tail_opts_t opts = {.lines = -10, .bytes = 0, .follow = false, .quiet = false, .verbose = false, .zero_terminated = false, .sleep_interval = 1.0};
    int c;
    while ((c = getopt_long(argc, argv, "c:n:f::qvs:z", long_options, NULL)) != -1) {
        switch (c) {
            case 'c':
                opts.bytes = atoll(optarg[0] == '+' ? optarg + 1 : optarg);
                if (optarg[0] != '+')
                    opts.bytes = -opts.bytes;
                opts.lines = 0;
                break;
            case 'n':
                opts.lines = atoll(optarg[0] == '+' ? optarg + 1 : optarg);
                if (optarg[0] != '+')
                    opts.lines = -opts.lines;
                opts.bytes = 0;
                break;
            case 'f':
                opts.follow = true;
                break;
            case 'q':
                opts.quiet = true;
                opts.verbose = false;
                break;
            case 'v':
                opts.verbose = true;
                opts.quiet = false;
                break;
            case 'z':
                opts.zero_terminated = true;
                break;
            case 's':
                opts.sleep_interval = atof(optarg);
                break;
            case 'h':
                printf("Usage: %s [OPTION]... [FILE]...\n", argv[0]);
                // ...
                return 0;
            case 'V':
                printf("tail (bx) %s\n", BX_VERSION);
                return 0;
            default:
                return 1;
        }
    }

    if (optind == argc) {
        if (opts.bytes != 0)
            tail_bytes(stdin, opts.bytes);
        else
            tail_lines(stdin, opts.lines, opts.zero_terminated ? '\0' : '\n');
    }
    else {
        bool multiple = (argc - optind > 1);
        for (int i = optind; i < argc; i++) {
            if ((multiple && !opts.quiet) || opts.verbose) {
                printf("%s==> %s <==\n", (i > optind) ? "\n" : "", argv[i]);
            }
            FILE* f = fopen(argv[i], "r");
            if (!f) {
                bx_perror(argv[i]);
                continue;
            }
            if (opts.bytes != 0)
                tail_bytes(f, opts.bytes);
            else
                tail_lines(f, opts.lines, opts.zero_terminated ? '\0' : '\n');

            if (opts.follow) {
                // Simple follow
                struct timespec ts;
                ts.tv_sec = (time_t)opts.sleep_interval;
                ts.tv_nsec = (long)((opts.sleep_interval - (double)ts.tv_sec) * 1000000000.0);
                while (1) {
                    int ch;
                    while ((ch = getc(f)) != EOF)
                        putchar(ch);
                    fflush(stdout);
                    nanosleep(&ts, NULL);
                    clearerr(f);
                }
            }

            fclose(f);
        }
    }

    return 0;
}
