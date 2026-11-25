#include <errno.h>
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
#include "lib/option_scan.h"
#include "lib/line_writer.h"

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

static bool bx_tail_write_error(struct bx_diag_ctx* diag) {
    bx_diag(diag, "write error: %s", strerror(errno));
    return false;
}

static bool bx_tail_putc(struct bx_line_writer* writer, int c, struct bx_diag_ctx* diag) {
    if (!bx_line_writer_putc(writer, (char)c)) {
        return bx_tail_write_error(diag);
    }
    return true;
}

static bool bx_tail_write(
    struct bx_line_writer* writer,
    const void* data,
    size_t length,
    struct bx_diag_ctx* diag
) {
    if (!bx_line_writer_write(writer, data, length)) {
        return bx_tail_write_error(diag);
    }
    return true;
}

static bool bx_tail_puts(struct bx_line_writer* writer, const char* text, struct bx_diag_ctx* diag) {
    if (!bx_line_writer_puts(writer, text)) {
        return bx_tail_write_error(diag);
    }
    return true;
}

static bool bx_tail_write_header(
    struct bx_line_writer* writer,
    const char* path,
    bool leading_newline,
    struct bx_diag_ctx* diag
) {
    if (leading_newline && !bx_tail_putc(writer, '\n', diag)) {
        return false;
    }
    return bx_tail_puts(writer, "==> ", diag) &&
           bx_tail_puts(writer, path, diag) &&
           bx_tail_puts(writer, " <==\n", diag);
}

static bool tail_bytes(FILE* f, long long n, struct bx_line_writer* writer, struct bx_diag_ctx* diag) {
    if (n > 0) {
        // Output starting with byte n (1-indexed)
        if (fseek(f, n - 1, SEEK_SET) == 0) {
            int c;
            while ((c = getc(f)) != EOF) {
                if (!bx_tail_putc(writer, c, diag)) {
                    return false;
                }
            }
        }
        else {
            // Not seekable, skip n-1 bytes
            for (long long i = 0; i < n - 1; i++) {
                if (getc(f) == EOF)
                    break;
            }
            int c;
            while ((c = getc(f)) != EOF) {
                if (!bx_tail_putc(writer, c, diag)) {
                    return false;
                }
            }
        }
    }
    else if (n < 0) {
        // Output last -n bytes
        n = -n;
        if (fseek(f, -n, SEEK_END) == 0) {
            int c;
            while ((c = getc(f)) != EOF) {
                if (!bx_tail_putc(writer, c, diag)) {
                    return false;
                }
            }
        }
        else {
            // Not seekable, use a buffer
            char* buf = malloc(n);
            if (!buf)
                return true;
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
                if (!bx_tail_putc(writer, buf[(start + i) % n], diag)) {
                    free(buf);
                    return false;
                }
            }
            free(buf);
        }
    }

    return true;
}

static bool tail_lines(
    FILE* f,
    long long n,
    char delimiter,
    struct bx_line_writer* writer,
    struct bx_diag_ctx* diag
) {
    if (n > 0) {
        // Output starting with line n (1-indexed)
        long long current_line = 1;
        int c;
        while (current_line < n && (c = getc(f)) != EOF) {
            if (c == delimiter)
                current_line++;
        }
        while ((c = getc(f)) != EOF) {
            if (!bx_tail_putc(writer, c, diag)) {
                return false;
            }
        }
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
            while ((c = getc(f)) != EOF) {
                if (!bx_tail_putc(writer, c, diag)) {
                    return false;
                }
            }
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
            bool ok = true;
            for (size_t i = 0; i < count; i++) {
                size_t slot = (start + i) % n;
                if (ok && !bx_tail_write(writer, lines[slot], lens[slot], diag)) {
                    ok = false;
                }
                free(lines[slot]);
                lines[slot] = NULL;
            }
            for (size_t i = 0; i < (size_t)n; i++)
                free(lines[i]);
            free(lines);
            free(lens);
            if (!ok) {
                return false;
            }
        }
    }

    return true;
}

int bx_tail_main(int argc, char** argv) {
    static const struct bx_option_spec specs[] = {
        {'c', 'c', "bytes", BX_OPTION_REQUIRED_VALUE},
        {'n', 'n', "lines", BX_OPTION_REQUIRED_VALUE},
        {'f', 'f', "follow", BX_OPTION_OPTIONAL_VALUE},
        {'q', 'q', "quiet", BX_OPTION_NO_VALUE},
        {'q', 0, "silent", BX_OPTION_NO_VALUE},
        {'v', 'v', "verbose", BX_OPTION_NO_VALUE},
        {'z', 'z', "zero-terminated", BX_OPTION_NO_VALUE},
        {'s', 's', "sleep-interval", BX_OPTION_REQUIRED_VALUE},
        {'h', 0, "help", BX_OPTION_NO_VALUE},
        {'V', 0, "version", BX_OPTION_NO_VALUE},
    };

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
    size_t option_start = argc > 0 ? 1u : 0u;
    if (argc > 1) {
        long long legacy_lines = 0;
        if (bx_tail_parse_legacy_lines_option(argv[1], &legacy_lines)) {
            opts.lines = legacy_lines;
            opts.bytes = 0;
            option_start = 2;
        }
    }

    const char** files = NULL;
    size_t file_count = 0;
    int status = 1;
    struct bx_option_cursor cursor;
    bx_option_cursor_init(&cursor, argv, option_start, (size_t)argc, BX_OPTION_ORDERED | BX_OPTION_ABBREVIATE, specs, sizeof(specs) / sizeof(specs[0]));
    struct bx_option_result option;
    while ((option = bx_option_next(&cursor)).event != BX_OPTION_END) {
        if (option.event == BX_OPTION_OPERAND) {
            if (!files) {
                files = calloc((size_t)argc - option.index, sizeof(*files));
                if (!files) {
                    bx_perror_path(&diag, "allocate input operands");
                    goto done;
                }
            }
            files[file_count++] = option.token;
            continue;
        }
        if (option.event != BX_OPTION_MATCH) {
            if (option.event == BX_OPTION_MISSING_VALUE && option.spec)
                bx_diag(&diag, "option requires an argument -- '%c'", option.spec->short_name);
            else if (option.short_name)
                bx_diag(&diag, "invalid option -- '%c'", option.short_name);
            else
                bx_diag(&diag, "unrecognized option '%s'", option.token);
            goto done;
        }
        switch (option.spec->id) {
            case 'c':
                if (!bx_tail_parse_count_argument(option.value, &opts.bytes)) {
                    bx_diag(&diag, "invalid number of bytes: '%s'", option.value);
                    goto done;
                }
                opts.lines = 0;
                break;
            case 'n':
                if (!bx_tail_parse_count_argument(option.value, &opts.lines)) {
                    bx_diag(&diag, "invalid number of lines: '%s'", option.value);
                    goto done;
                }
                opts.bytes = 0;
                break;
            case 'f':
                if (option.value && strcmp(option.value, "descriptor") != 0) {
                    bx_diag(&diag, "unsupported follow mode: '%s'", option.value);
                    goto done;
                }
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
                if (!bx_tail_parse_sleep_interval(option.value, &opts.sleep_interval)) {
                    bx_diag(&diag, "invalid sleep interval: '%s'", option.value);
                    goto done;
                }
                break;
            case 'h':
                printf("Usage: %s [OPTION]... [FILE]...\n", argv[0]);
                // ...
                status = 0;
                goto done;
            case 'V':
                printf("tail (bx) %s\n", BX_VERSION);
                status = 0;
                goto done;
            default:
                goto done;
        }
    }

    bool ok = true;
    char output_buffer[8192];
    struct bx_line_writer writer;
    bx_line_writer_init(&writer, STDOUT_FILENO, output_buffer, sizeof(output_buffer));

    if (file_count == 0) {
        if (opts.bytes != 0)
            ok = tail_bytes(stdin, opts.bytes, &writer, &diag);
        else
            ok = tail_lines(stdin, opts.lines, opts.zero_terminated ? '\0' : '\n', &writer, &diag);
    }
    else {
        bool multiple = file_count > 1;
        for (size_t i = 0; ok && i < file_count; i++) {
            const char* path = files[i];
            if ((multiple && !opts.quiet) || opts.verbose) {
                ok = bx_tail_write_header(&writer, path, i > 0, &diag);
            }

            FILE* f = (strcmp(path, "-") == 0) ? stdin : fopen(path, "r");
            if (!f) {
                bx_perror_path(&diag, path);
                continue;
            }
            if (ok) {
                if (opts.bytes != 0)
                    ok = tail_bytes(f, opts.bytes, &writer, &diag);
                else
                    ok = tail_lines(f, opts.lines, opts.zero_terminated ? '\0' : '\n', &writer, &diag);
            }

            if (ok && opts.follow) {
                // Simple follow
                while (1) {
                    int ch;
                    while ((ch = getc(f)) != EOF) {
                        if (!bx_tail_putc(&writer, ch, &diag)) {
                            ok = false;
                            break;
                        }
                    }
                    if (!ok || !bx_line_writer_flush(&writer)) {
                        if (ok) {
                            bx_tail_write_error(&diag);
                            ok = false;
                        }
                        break;
                    }
                    nanosleep(&opts.sleep_interval, NULL);
                    clearerr(f);
                }
            }

            if (f != stdin)
                fclose(f);
        }
    }

    if (ok && bx_line_writer_error(&writer) == 0 && !bx_line_writer_flush(&writer)) {
        bx_tail_write_error(&diag);
    }

    status = diag.exit_status;
done:
    free(files);
    return status;
}
