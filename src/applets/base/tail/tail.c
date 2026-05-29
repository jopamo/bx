#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include "applets.h"
#include "bx/diag.h"
#include "lib/size_parse.h"
#include "lib/time_parse.h"
#include "lib/args_common.h"

typedef struct {
    long long lines;
    long long bytes;
    bool follow;
    bool quiet;
    bool verbose;
    bool zero_terminated;
    struct timespec sleep_interval;
} tail_opts_t;

static bool bx_tail_parse_magnitude(const char* text, long long* value_out) {
    if (text == NULL || text[0] == '\0') {
        return false;
    }

    intmax_t value = 0;
    if (!bx_size_parse_scaled_count(text, &value)) {
        return false;
    }
    if (value < 0 || value > (intmax_t)LLONG_MAX) {
        return false;
    }

    *value_out = (long long)value;
    return true;
}

static bool bx_tail_parse_count_argument(const char* text, long long* value_out) {
    if (text == NULL || text[0] == '\0') {
        return false;
    }

    bool from_start = false;
    const char* magnitude = text;
    if (magnitude[0] == '+') {
        from_start = true;
        magnitude++;
        if (magnitude[0] == '\0') {
            return false;
        }
    }
    else if (magnitude[0] == '-') {
        magnitude++;
        if (magnitude[0] == '\0') {
            return false;
        }
    }

    intmax_t value = 0;
    if (!bx_size_parse_scaled_count(magnitude, &value)) {
        return false;
    }
    if (value < 0 || value > (intmax_t)LLONG_MAX) {
        return false;
    }

    *value_out = from_start ? (long long)value : -(long long)value;
    return true;
}

static bool bx_tail_parse_legacy_lines_option(const char* arg, long long* lines_out) {
    if (arg == NULL || (arg[0] != '-' && arg[0] != '+') || arg[1] == '\0') {
        return false;
    }

    if (arg[0] == '-' && arg[1] == '-') {
        return false;
    }

    long long value = 0;
    if (!bx_tail_parse_magnitude(arg + 1, &value)) {
        return false;
    }

    *lines_out = (arg[0] == '+') ? value : -value;
    return true;
}

static bool bx_tail_parse_sleep_interval(const char* text, struct timespec* interval_out) {
    struct bx_time_duration_parse_result result = {
        .seconds = 0.0,
        .infinite = false,
    };

    if (!bx_time_parse_duration_seconds(text, NULL, &result) || result.infinite) {
        return false;
    }

    return bx_time_seconds_to_timespec(result.seconds, interval_out);
}

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

    tail_opts_t opts = {
        .lines = -10,
        .bytes = 0,
        .follow = false,
        .quiet = false,
        .verbose = false,
        .zero_terminated = false,
        .sleep_interval = {.tv_sec = 1, .tv_nsec = 0},
    };
    struct bx_diag_ctx diag = {.progname = "tail", .exit_status = 0};
    int option_start = 1;
    if (argc > 1) {
        long long legacy_lines = 0;
        if (bx_tail_parse_legacy_lines_option(argv[1], &legacy_lines)) {
            opts.lines = legacy_lines;
            opts.bytes = 0;
            option_start = 2;
        }
    }

    bx_args_getopt_reset_at(option_start);

    int c;
    while ((c = bx_args_getopt_long(argc, argv, "c:n:f::qvs:z", long_options, NULL)) != -1) {
        switch (c) {
            case 'c':
                if (!bx_tail_parse_count_argument(optarg, &opts.bytes)) {
                    bx_diag(&diag, "invalid number of bytes: '%s'", optarg);
                    return 1;
                }
                opts.lines = 0;
                break;
            case 'n':
                if (!bx_tail_parse_count_argument(optarg, &opts.lines)) {
                    bx_diag(&diag, "invalid number of lines: '%s'", optarg);
                    return 1;
                }
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
                if (!bx_tail_parse_sleep_interval(optarg, &opts.sleep_interval)) {
                    bx_diag(&diag, "invalid sleep interval: '%s'", optarg != NULL ? optarg : "");
                    return 1;
                }
                break;
            case 'h':
                printf("Usage: %s [OPTION]... [FILE]...\n", argv[0]);
                // ...
                return 0;
            case 'V':
                printf("tail (bx) %s\n", BX_VERSION);
                return 0;
            case '?':
                if (optopt == 'c' || optopt == 'n' || optopt == 's') {
                    bx_diag(&diag, "option requires an argument -- '%c'", optopt);
                }
                else if (optopt != 0) {
                    bx_diag(&diag, "invalid option -- '%c'", optopt);
                }
                else if (optind > 0 && optind <= argc && argv[optind - 1] != NULL) {
                    bx_diag(&diag, "unrecognized option '%s'", argv[optind - 1]);
                }
                else {
                    bx_diag(&diag, "unrecognized option");
                }
                return 1;
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
            const char* path = argv[i];
            if ((multiple && !opts.quiet) || opts.verbose) {
                printf("%s==> %s <==\n", (i > optind) ? "\n" : "", path);
            }

            FILE* f = (strcmp(path, "-") == 0) ? stdin : fopen(path, "r");
            if (!f) {
                bx_perror_path(&diag, path);
                continue;
            }
            if (opts.bytes != 0)
                tail_bytes(f, opts.bytes);
            else
                tail_lines(f, opts.lines, opts.zero_terminated ? '\0' : '\n');

            if (opts.follow) {
                // Simple follow
                while (1) {
                    int ch;
                    while ((ch = getc(f)) != EOF)
                        putchar(ch);
                    fflush(stdout);
                    nanosleep(&opts.sleep_interval, NULL);
                    clearerr(f);
                }
            }

            if (f != stdin)
                fclose(f);
        }
    }

    return diag.exit_status;
}
