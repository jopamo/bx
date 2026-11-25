#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>
#include <locale.h>
#include <stdbool.h>
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#include "applets.h"
#include "bx/diag.h"
#include "lib/byte_count.h"
#include "lib/cli_common.h"
#include "lib/line_writer.h"
#include "lib/option_scan.h"

typedef struct {
    unsigned long long lines;
    unsigned long long words;
    unsigned long long chars;
    unsigned long long bytes;
    unsigned long long max_line_width;
} wc_counts_t;

struct wc_count_options {
    bool lines;
    bool words;
    bool chars;
    bool bytes;
    bool max_line_width;
};

static bool wc_count_regular_bytes(FILE* f, unsigned long long* count) {
    struct stat st;
    int fd = fileno(f);
    off_t current = ftello(f);
    if (fd < 0 || current < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        return false;
    }
    if (fseeko(f, 0, SEEK_END) != 0) {
        return false;
    }
    off_t end = ftello(f);
    if (end < 0) {
        (void)fseeko(f, current, SEEK_SET);
        return false;
    }
    *count = end > current ? (unsigned long long)(end - current) : 0u;
    return true;
}

static void wc_count(FILE* f, wc_counts_t* res, const struct wc_count_options* options) {
    memset(res, 0, sizeof(*res));
    if (!options->lines && !options->words && !options->max_line_width && (options->bytes || options->chars)) {
        unsigned long long count = 0u;
        if (wc_count_regular_bytes(f, &count)) {
            res->bytes = count;
            res->chars = count;
            return;
        }
    }

    uint8_t buffer[65536];
    unsigned long long current_line_width = 0u;
    bool in_word = false;

    while (true) {
        size_t len = fread(buffer, 1u, sizeof(buffer), f);
        if (len == 0u) {
            break;
        }
        if (options->bytes) {
            res->bytes += (unsigned long long)len;
        }
        if (options->chars) {
            res->chars += (unsigned long long)len;
        }
        if (options->lines) {
            res->lines += bx_byte_count(buffer, len, '\n');
        }
        if (!options->words && !options->max_line_width) {
            continue;
        }

        for (size_t i = 0u; i < len; i++) {
            int c = buffer[i];
            if (options->max_line_width) {
                if (c == '\n') {
                    if (current_line_width > res->max_line_width) {
                        res->max_line_width = current_line_width;
                    }
                    current_line_width = 0u;
                }
                else if (c == '\t') {
                    current_line_width += 8u - (current_line_width % 8u);
                }
                else if (isprint(c)) {
                    current_line_width++;
                }
            }

            if (options->words) {
                if (isspace(c)) {
                    in_word = false;
                }
                else if (!in_word) {
                    in_word = true;
                    res->words++;
                }
            }
        }
    }
    if (options->max_line_width && current_line_width > res->max_line_width) {
        res->max_line_width = current_line_width;
    }
}

static int count_digits(unsigned long long n) {
    if (n == 0) return 1;
    int d = 0;
    while (n) { d++; n /= 10; }
    return d;
}

static void value_str(unsigned long long n, char* buf) {
    sprintf(buf, "%llu", n);
}

static bool wc_write_count(struct bx_line_writer* writer, bool leading_space, int width, unsigned long long val) {
    char buf[32];
    char field[64];
    int len;

    value_str(val, buf);
    len = snprintf(field, sizeof(field), leading_space ? " %*s" : "%*s", width, buf);
    return len >= 0 && (size_t)len < sizeof(field) && bx_line_writer_write(writer, field, (size_t)len);
}

static bool wc_write_count_raw(struct bx_line_writer* writer, unsigned long long val) {
    char buf[32];

    value_str(val, buf);
    return bx_line_writer_puts(writer, buf);
}

static bool wc_write_counts(
    struct bx_line_writer* writer,
    const wc_counts_t* counts,
    int width,
    bool pad_first,
    bool opt_l,
    bool opt_w,
    bool opt_c,
    bool opt_m,
    bool opt_L
) {
    bool emitted = false;

    if (opt_l) {
        if (pad_first ? !wc_write_count(writer, emitted, width, counts->lines) : !wc_write_count_raw(writer, counts->lines))
            return false;
        emitted = true;
    }
    if (opt_w) {
        if (pad_first ? !wc_write_count(writer, emitted, width, counts->words) : !wc_write_count_raw(writer, counts->words))
            return false;
        emitted = true;
    }
    if (opt_m) {
        if (pad_first ? !wc_write_count(writer, emitted, width, counts->chars) : !wc_write_count_raw(writer, counts->chars))
            return false;
        emitted = true;
    }
    if (opt_c) {
        if (pad_first ? !wc_write_count(writer, emitted, width, counts->bytes) : !wc_write_count_raw(writer, counts->bytes))
            return false;
        emitted = true;
    }
    if (opt_L) {
        if (pad_first ? !wc_write_count(writer, emitted, width, counts->max_line_width) : !wc_write_count_raw(writer, counts->max_line_width))
            return false;
    }

    return true;
}

static int compute_number_width(size_t num_files, const char* const* files) {
    int width = 1;
    int minimum_width = 1;
    unsigned long long regular_total = 0;
    bool any_non_regular = false;

    for (size_t i = 0; i < num_files; i++) {
        const char* name = files[i];
        if (strcmp(name, "-") == 0) {
            any_non_regular = true;
            continue;
        }
        struct stat st;
        if (stat(name, &st) != 0) {
            continue;
        }
        if (!S_ISREG(st.st_mode)) {
            any_non_regular = true;
        }
        else {
            if (regular_total + (unsigned long long)st.st_size < regular_total) {
                regular_total = ~0ULL;
                break;
            }
            regular_total += (unsigned long long)st.st_size;
        }
    }

    if (any_non_regular)
        minimum_width = 7;

    width = count_digits(regular_total);
    if (width < minimum_width)
        width = minimum_width;

    return width;
}

int bx_wc_main(int argc, char** argv) {
    static const struct bx_option_spec specs[] = {
        {'c', 'c', "bytes", BX_OPTION_NO_VALUE},
        {'m', 'm', "chars", BX_OPTION_NO_VALUE},
        {'l', 'l', "lines", BX_OPTION_NO_VALUE},
        {'w', 'w', "words", BX_OPTION_NO_VALUE},
        {'L', 'L', "max-line-length", BX_OPTION_NO_VALUE},
        {'h', 0, "help", BX_OPTION_NO_VALUE},
        {'v', 0, "version", BX_OPTION_NO_VALUE},
    };

    const char* progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "wc");
    struct bx_diag_ctx diag = {.progname = progname, .exit_status = 0};
    bool opt_c = false, opt_m = false, opt_l = false, opt_w = false, opt_L = false;
    const char** paths = argc > 1 ? calloc((size_t)(argc - 1), sizeof(*paths)) : NULL;
    if (argc > 1 && paths == NULL) {
        bx_diag(&diag, "memory exhausted");
        return diag.exit_status;
    }
    size_t num_files = 0;
    struct bx_option_cursor cursor;
    bx_option_cursor_init(&cursor, argv, argc > 0 ? 1u : 0u, (size_t)argc,
                          BX_OPTION_ORDERED | BX_OPTION_ABBREVIATE, specs, sizeof(specs) / sizeof(specs[0]));
    struct bx_option_result option;
    while ((option = bx_option_next(&cursor)).event != BX_OPTION_END) {
        if (option.event == BX_OPTION_OPERAND) {
            paths[num_files++] = option.token;
            continue;
        }
        if (option.event != BX_OPTION_MATCH) {
            if (option.short_name)
                bx_diag(&diag, "invalid option -- '%c'", option.short_name);
            else
                bx_diag(&diag, "unrecognized option '%s'", option.token);
            goto done;
        }
        switch (option.spec->id) {
            case 'c':
                opt_c = true;
                break;
            case 'm':
                opt_m = true;
                break;
            case 'l':
                opt_l = true;
                break;
            case 'w':
                opt_w = true;
                break;
            case 'L':
                opt_L = true;
                break;
            case 'h':
                printf("Usage: %s [OPTION]... [FILE]...\n", progname);
                printf("Print newline, word, and byte counts for each FILE, and a total line if\n");
                printf("more than one FILE is specified.  A word is a non-zero-length sequence of\n");
                printf("characters delimited by white space.\n");
                printf("\n");
                printf("  -c, --bytes            print the byte counts\n");
                printf("  -m, --chars            print the character counts\n");
                printf("  -l, --lines            print the newline counts\n");
                printf("  -L, --max-line-length  print the maximum display width\n");
                printf("  -w, --words            print the word counts\n");
                printf("      --help          display this help and exit\n");
                printf("      --version       output version information and exit\n");
                goto done;
            case 'v':
                bx_cli_print_version(progname);
                goto done;
        }
    }

    if (!(opt_c || opt_m || opt_l || opt_w || opt_L)) {
        opt_l = opt_w = opt_c = true;
    }

    int field_count = (opt_l ? 1 : 0) + (opt_w ? 1 : 0) + (opt_c ? 1 : 0) + (opt_m ? 1 : 0) + (opt_L ? 1 : 0);
    const struct wc_count_options count_options = {
        .lines = opt_l,
        .words = opt_w,
        .chars = opt_m,
        .bytes = opt_c,
        .max_line_width = opt_L,
    };

    if (num_files == 0) {
        int width = field_count > 1 ? 7 : 1;
        wc_counts_t res;
        char output_buffer[8192];
        struct bx_line_writer writer;

        wc_count(stdin, &res, &count_options);
        bx_line_writer_init(&writer, STDOUT_FILENO, output_buffer, sizeof(output_buffer));

        if (!wc_write_counts(&writer, &res, width, field_count > 1, opt_l, opt_w, opt_c, opt_m, opt_L)
            || !bx_line_writer_putc(&writer, '\n')
            || !bx_line_writer_flush(&writer)) {
            diag.exit_status = 1;
        }
        goto done;
    }

    wc_counts_t* counts = calloc(num_files, sizeof(*counts));
    bool* file_ok = calloc(num_files, sizeof(*file_ok));
    if (!counts || !file_ok) {
        bx_diag(&diag, "memory exhausted");
        goto free_counts;
    }

    size_t ncounts = 0;
    for (size_t i = 0; i < num_files; i++) {
        const char* name = paths[i];
        FILE* f;
        if (strcmp(name, "-") == 0) {
            f = stdin;
        }
        else {
            f = fopen(name, "r");
            if (!f) {
                bx_perror_path(&diag, name);
                continue;
            }
        }
        wc_count(f, &counts[ncounts], &count_options);
        if (f != stdin)
            fclose(f);
        file_ok[i] = true;
        ncounts++;
    }

    wc_counts_t total = {0};
    for (size_t i = 0; i < ncounts; i++) {
        total.lines += counts[i].lines;
        total.words += counts[i].words;
        total.chars += counts[i].chars;
        total.bytes += counts[i].bytes;
        if (counts[i].max_line_width > total.max_line_width)
            total.max_line_width = counts[i].max_line_width;
    }

    bool show_total = num_files > 1;
    int width = compute_number_width(num_files, paths);
    char output_buffer[8192];
    struct bx_line_writer writer;
    bx_line_writer_init(&writer, STDOUT_FILENO, output_buffer, sizeof(output_buffer));

    size_t out_idx = 0;
    for (size_t i = 0; i < num_files; i++) {
        if (!file_ok[i])
            continue;
        wc_counts_t* r = &counts[out_idx++];

        if (!wc_write_counts(&writer, r, width, true, opt_l, opt_w, opt_c, opt_m, opt_L)
            || !bx_line_writer_putc(&writer, ' ')
            || !bx_line_writer_put_line(&writer, paths[i])) {
            diag.exit_status = 1;
            goto free_counts;
        }
    }

    if (show_total) {
        if (!wc_write_counts(&writer, &total, width, true, opt_l, opt_w, opt_c, opt_m, opt_L)
            || !bx_line_writer_puts(&writer, " total\n")) {
            diag.exit_status = 1;
            goto free_counts;
        }
    }

    if (!bx_line_writer_flush(&writer)) {
        diag.exit_status = 1;
    }

free_counts:
    free(counts);
    free(file_ok);
done:
    free(paths);
    return diag.exit_status;
}
