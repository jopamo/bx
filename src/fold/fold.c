#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>

#include "applets.h"
#include "diag.h"
#include "libbx.h"

struct bx_fold_options {
    const char* progname;
    size_t width;
    bool bytes;
    bool spaces;
    bool show_help;
    bool show_version;
};

static void bx_fold_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... [FILE]...\n", progname);
    fprintf(stream, "Wrap input lines in each FILE, writing to standard output.\n");
    fprintf(stream, "\n");
    fprintf(stream, "With no FILE, or when FILE is -, read standard input.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -b, --bytes         count bytes rather than columns\n");
    fprintf(stream, "  -s, --spaces        break at spaces\n");
    fprintf(stream, "  -w, --width=WIDTH   use WIDTH columns instead of 80\n");
    fprintf(stream, "      --help     display this help and exit\n");
    fprintf(stream, "      --version  output version information and exit\n");
}

static void bx_fold_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

static bool bx_fold_parse_options(int argc, char** argv, struct bx_fold_options* options, int* first_operand, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"bytes", no_argument, NULL, 'b'}, {"spaces", no_argument, NULL, 's'}, {"width", required_argument, NULL, 'w'},
        {"help", no_argument, NULL, 1},    {"version", no_argument, NULL, 2},  {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = "fold";
    options->width = 80;
    diag->progname = options->progname;

    opterr = 0;
    optind = 1;

    while (true) {
        int option_index = 0;
        int c = getopt_long(argc, argv, "bsw:", long_options, &option_index);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 'b':
                options->bytes = true;
                break;
            case 's':
                options->spaces = true;
                break;
            case 'w':
                options->width = strtoul(optarg, NULL, 10);
                if (options->width == 0) {
                    bx_diag(diag, "invalid width: '%s'", optarg);
                    return false;
                }
                break;
            case 1:
                options->show_help = true;
                return true;
            case 2:
                options->show_version = true;
                return true;
            case '?':
                bx_diag(diag, "invalid option -- '%c'", optopt);
                return false;
            default:
                return false;
        }
    }

    *first_operand = optind;
    return true;
}

static void fold_file(FILE* f, struct bx_fold_options* options) {
    char* line = NULL;
    size_t line_cap = 0;
    ssize_t len;

    while ((len = getline(&line, &line_cap, f)) != -1) {
        size_t col = 0;
        size_t last_space_pos = 0;
        size_t start = 0;

        for (size_t i = 0; i < (size_t)len; i++) {
            int c = (unsigned char)line[i];

            if (c == '\n') {
                fwrite(&line[start], 1, i - start + 1, stdout);
                start = i + 1;
                col = 0;
                last_space_pos = 0;
                continue;
            }

            size_t next_col = col;
            if (options->bytes) {
                next_col++;
            }
            else {
                if (c == '\t')
                    next_col = (col / 8 + 1) * 8;
                else if (c == '\b')
                    next_col = (col > 0) ? col - 1 : 0;
                else if (c == '\r')
                    next_col = 0;
                else
                    next_col++;
            }

            if (next_col > options->width && col > 0) {
                // Time to wrap
                size_t wrap_pos = i;
                if (options->spaces && last_space_pos > start) {
                    wrap_pos = last_space_pos;
                    fwrite(&line[start], 1, wrap_pos - start, stdout);
                    putchar('\n');
                    start = wrap_pos;
                    // Skip the space that caused the break
                    if (line[start] == ' ') {
                        start++;
                    }
                    // Re-calculate column for the new line starting from start to i
                    col = 0;
                    for (size_t j = start; j <= i; j++) {
                        int c2 = (unsigned char)line[j];
                        if (options->bytes)
                            col++;
                        else {
                            if (c2 == '\t')
                                col = (col / 8 + 1) * 8;
                            else if (c2 == '\b')
                                col = (col > 0) ? col - 1 : 0;
                            else if (c2 == '\r')
                                col = 0;
                            else
                                col++;
                        }
                    }
                    last_space_pos = 0;
                }
                else {
                    fwrite(&line[start], 1, i - start, stdout);
                    putchar('\n');
                    start = i;
                    col = next_col - col;  // Column of char c on new line
                    if (c == '\t' && !options->bytes)
                        col = 8;  // Reset tab on new line
                    // Wait, if it was a tab, col becomes 8.
                    // Actually, simpler to just re-scanchar c
                    col = 0;
                    if (options->bytes)
                        col = 1;
                    else {
                        if (c == '\t')
                            col = 8;
                        else if (c == '\b')
                            col = 0;
                        else if (c == '\r')
                            col = 0;
                        else
                            col = 1;
                    }
                    last_space_pos = 0;
                }
            }
            else {
                col = next_col;
                if (isspace(c) && c != '\r' && c != '\t' && c != '\b') {
                    last_space_pos = i + 1;
                }
            }
        }
        if (start < (size_t)len) {
            fwrite(&line[start], 1, (size_t)len - start, stdout);
        }
    }
    free(line);
}

int bx_fold_main(int argc, char** argv) {
    struct bx_fold_options options;
    struct bx_diag_ctx diag = {.progname = "fold", .exit_status = 0};
    int first_operand = 0;

    if (!bx_fold_parse_options(argc, argv, &options, &first_operand, &diag))
        return 1;
    if (options.show_help) {
        bx_fold_print_help(stdout, options.progname);
        return 0;
    }
    if (options.show_version) {
        bx_fold_print_version(options.progname);
        return 0;
    }

    int num_files = argc - first_operand;
    for (int i = 0; i < num_files || (i == 0 && num_files == 0); i++) {
        const char* filename = (num_files == 0) ? "-" : argv[first_operand + i];
        FILE* f = strcmp(filename, "-") == 0 ? stdin : fopen(filename, "r");
        if (!f) {
            bx_diag(&diag, "%s: %s", filename, strerror(errno));
            continue;
        }
        fold_file(f, &options);
        if (f != stdin)
            fclose(f);
    }

    return diag.exit_status;
}
