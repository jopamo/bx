#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"
#include "bx/libbx.h"
#include "lib/cli_common.h"
#include "lib/fopen_dash.h"
#include "lib/args_common.h"
#include "lib/line_writer.h"

struct bx_comm_options {
    const char* progname;
    bool suppress_1;
    bool suppress_2;
    bool suppress_3;
    bool check_order;
    bool no_check_order;
    const char* output_delimiter;
    bool total;
    bool zero_terminated;
    bool show_help;
    bool show_version;
};

static void bx_comm_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... FILE1 FILE2\n", progname);
    fprintf(stream, "Compare sorted files FILE1 and FILE2 line by line.\n");
    fprintf(stream, "\n");
    fprintf(stream, "When FILE1 or FILE2 (not both) is -, read standard input.\n");
    fprintf(stream, "\n");
    fprintf(stream, "With no options, produce three-column output.  Column one contains\n");
    fprintf(stream, "lines unique to FILE1, column two contains lines unique to FILE2,\n");
    fprintf(stream, "and column three contains lines common to both files.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -1                      suppress column 1 (lines unique to FILE1)\n");
    fprintf(stream, "  -2                      suppress column 2 (lines unique to FILE2)\n");
    fprintf(stream, "  -3                      suppress column 3 (lines common to both files)\n");
    fprintf(stream, "\n");
    fprintf(stream, "      --check-order       check that the input is correctly sorted, even\n");
    fprintf(stream, "                            if all input lines are paired\n");
    fprintf(stream, "      --nocheck-order     do not check that the input is correctly sorted\n");
    fprintf(stream, "      --output-delimiter=STR  separate columns with STR\n");
    fprintf(stream, "      --total             output a summary\n");
    fprintf(stream, "  -z, --zero-terminated   line delimiter is NUL, not newline\n");
    fprintf(stream, "      --help     display this help and exit\n");
    fprintf(stream, "      --version  output version information and exit\n");
    fprintf(stream, "\n");
    fprintf(stream, "Note, comparisons honor the rules specified by 'LC_COLLATE'.\n");
    fprintf(stream, "\n");
    fprintf(stream, "Examples:\n");
    fprintf(stream, "  %s -12 file1 file2  Print only lines present in both file1 and file2.\n", progname);
    fprintf(stream, "  %s -3 file1 file2   Print lines in file1 not in file2, and vice versa.\n", progname);
}

static bool bx_comm_parse_options(int argc, char** argv, struct bx_comm_options* options, int* first_operand, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"check-order", no_argument, NULL, 1},
        {"nocheck-order", no_argument, NULL, 2},
        {"output-delimiter", required_argument, NULL, 3},
        {"total", no_argument, NULL, 4},
        {"zero-terminated", no_argument, NULL, 'z'},
        {"help", no_argument, NULL, 5},
        {"version", no_argument, NULL, 6},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = "comm";
    options->output_delimiter = "\t";
    diag->progname = options->progname;

    bx_args_getopt_reset();

    while (true) {
        int option_index = 0;
        int c = bx_args_getopt_long(argc, argv, "+123z", long_options, &option_index);
        if (c == -1) {
            break;
        }

        switch (c) {
            case '1':
                options->suppress_1 = true;
                break;
            case '2':
                options->suppress_2 = true;
                break;
            case '3':
                options->suppress_3 = true;
                break;
            case 'z':
                options->zero_terminated = true;
                break;
            case 1:
                options->check_order = true;
                options->no_check_order = false;
                break;
            case 2:
                options->no_check_order = true;
                options->check_order = false;
                break;
            case 3:
                options->output_delimiter = optarg;
                break;
            case 4:
                options->total = true;
                break;
            case 5:
                options->show_help = true;
                return true;
            case 6:
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

struct line_buffer {
    char* data;
    size_t size;
    ssize_t len;
};

static void init_line_buffer(struct line_buffer* lb) {
    lb->data = NULL;
    lb->size = 0;
    lb->len = 0;
}

static void free_line_buffer(struct line_buffer* lb) {
    free(lb->data);
}

static ssize_t get_line(struct line_buffer* lb, FILE* stream, int delimiter) {
    lb->len = getdelim(&lb->data, &lb->size, delimiter, stream);
    return lb->len;
}

static bool bx_comm_write_error(struct bx_diag_ctx* diag) {
    bx_diag(diag, "write error: %s", strerror(errno));
    return false;
}

static bool print_column(struct bx_line_writer* writer, const char* line, ssize_t len, int col, struct bx_comm_options* options, struct bx_diag_ctx* diag) {
    if (col >= 2 && !options->suppress_1) {
        if (!bx_line_writer_puts(writer, options->output_delimiter)) {
            return bx_comm_write_error(diag);
        }
    }
    if (col >= 3 && !options->suppress_2) {
        if (!bx_line_writer_puts(writer, options->output_delimiter)) {
            return bx_comm_write_error(diag);
        }
    }
    if (len > 0 && !bx_line_writer_write(writer, line, (size_t)len)) {
        return bx_comm_write_error(diag);
    }
    return true;
}

static bool bx_comm_write_count(struct bx_line_writer* writer, unsigned long count, struct bx_diag_ctx* diag) {
    char buffer[32];
    int len = snprintf(buffer, sizeof(buffer), "%lu", count);
    if (len < 0 || (size_t)len >= sizeof(buffer)) {
        errno = EIO;
        return bx_comm_write_error(diag);
    }
    if (!bx_line_writer_write(writer, buffer, (size_t)len)) {
        return bx_comm_write_error(diag);
    }
    return true;
}

static bool bx_comm_write_text(struct bx_line_writer* writer, const char* text, struct bx_diag_ctx* diag) {
    if (!bx_line_writer_puts(writer, text)) {
        return bx_comm_write_error(diag);
    }
    return true;
}

static bool bx_comm_write_total(struct bx_line_writer* writer, unsigned long count1, unsigned long count2, unsigned long count3, const struct bx_comm_options* options, struct bx_diag_ctx* diag) {
    return bx_comm_write_count(writer, count1, diag)
        && bx_comm_write_text(writer, options->output_delimiter, diag)
        && bx_comm_write_count(writer, count2, diag)
        && bx_comm_write_text(writer, options->output_delimiter, diag)
        && bx_comm_write_count(writer, count3, diag)
        && bx_comm_write_text(writer, options->output_delimiter, diag)
        && bx_comm_write_text(writer, "total\n", diag);
}

static bool check_file_order(struct bx_diag_ctx* diag, int filenum, struct line_buffer* prev, struct line_buffer* curr, bool* ok, bool force_fatal) {
    if (prev->data && strcmp(prev->data, curr->data) > 0) {
        if (force_fatal) {
            bx_diag(diag, "file %d is not in sorted order", filenum);
            return false;
        }
        if (*ok) {
            bx_diag(diag, "file %d is not in sorted order", filenum);
            *ok = false;
        }
    }
    return true;
}

int bx_comm_main(int argc, char** argv) {
    struct bx_comm_options options;
    struct bx_diag_ctx diag = {
        .progname = "comm",
        .exit_status = 0,
    };
    int first_operand = 0;

    if (!bx_comm_parse_options(argc, argv, &options, &first_operand, &diag)) {
        return 1;
    }

    if (options.show_help) {
        bx_comm_print_help(stdout, options.progname);
        return 0;
    }
    if (options.show_version) {
        bx_cli_print_version(options.progname);
        return 0;
    }

    if (argc - first_operand != 2) {
        bx_diag(&diag, "missing operand");
        if (argc - first_operand > 2) {
            bx_diag(&diag, "extra operand '%s'", argv[first_operand + 2]);
        }
        return 1;
    }

    const char* file1_name = argv[first_operand];
    const char* file2_name = argv[first_operand + 1];

    if (strcmp(file1_name, "-") == 0 && strcmp(file2_name, "-") == 0) {
        bx_diag(&diag, "both files cannot be standard input");
        return 1;
    }

    bool f1_is_stdio = false;
    FILE* f1 = bx_fopen_dash(file1_name, "r", &f1_is_stdio);
    if (!f1) {
        bx_diag(&diag, "%s: %s", file1_name, strerror(errno));
        return 1;
    }

    bool f2_is_stdio = false;
    FILE* f2 = bx_fopen_dash(file2_name, "r", &f2_is_stdio);
    if (!f2) {
        bx_diag(&diag, "%s: %s", file2_name, strerror(errno));
        bx_fclose_nonstdio(f1, f1_is_stdio);
        return 1;
    }

    int delimiter = options.zero_terminated ? '\0' : '\n';
    struct line_buffer lb1, lb2;
    init_line_buffer(&lb1);
    init_line_buffer(&lb2);

    struct line_buffer prev1, prev2;
    init_line_buffer(&prev1);
    init_line_buffer(&prev2);

    ssize_t len1 = get_line(&lb1, f1, delimiter);
    ssize_t len2 = get_line(&lb2, f2, delimiter);

    unsigned long count1 = 0, count2 = 0, count3 = 0;
    bool order_ok1 = true;
    bool order_ok2 = true;
    char output_buffer[8192];
    struct bx_line_writer writer;
    bx_line_writer_init(&writer, STDOUT_FILENO, output_buffer, sizeof(output_buffer));

    while (len1 != -1 || len2 != -1) {
        int cmp;
        if (len1 != -1 && len2 != -1) {
            cmp = strcmp(lb1.data, lb2.data);
            if (cmp == 0) {
                if (options.check_order) {
                    if (!check_file_order(&diag, 1, &prev1, &lb1, &order_ok1, true))
                        goto cleanup;
                    if (!check_file_order(&diag, 2, &prev2, &lb2, &order_ok2, true))
                        goto cleanup;
                }
                if (!options.suppress_3) {
                    if (!print_column(&writer, lb1.data, lb1.len, 3, &options, &diag)) {
                        goto cleanup;
                    }
                }
                count3++;
                free(prev1.data);
                prev1 = lb1;
                init_line_buffer(&lb1);
                free(prev2.data);
                prev2 = lb2;
                init_line_buffer(&lb2);
                len1 = get_line(&lb1, f1, delimiter);
                len2 = get_line(&lb2, f2, delimiter);
            }
            else if (cmp < 0) {
                if (!options.no_check_order) {
                    if (!check_file_order(&diag, 1, &prev1, &lb1, &order_ok1, options.check_order))
                        goto cleanup;
                }
                if (!options.suppress_1) {
                    if (!print_column(&writer, lb1.data, lb1.len, 1, &options, &diag)) {
                        goto cleanup;
                    }
                }
                count1++;
                free(prev1.data);
                prev1 = lb1;
                init_line_buffer(&lb1);
                len1 = get_line(&lb1, f1, delimiter);
            }
            else {
                if (!options.no_check_order) {
                    if (!check_file_order(&diag, 2, &prev2, &lb2, &order_ok2, options.check_order))
                        goto cleanup;
                }
                if (!options.suppress_2) {
                    if (!print_column(&writer, lb2.data, lb2.len, 2, &options, &diag)) {
                        goto cleanup;
                    }
                }
                count2++;
                free(prev2.data);
                prev2 = lb2;
                init_line_buffer(&lb2);
                len2 = get_line(&lb2, f2, delimiter);
            }
        }
        else if (len1 != -1) {
            if (!options.no_check_order) {
                if (!check_file_order(&diag, 1, &prev1, &lb1, &order_ok1, options.check_order))
                    goto cleanup;
            }
            if (!options.suppress_1) {
                if (!print_column(&writer, lb1.data, lb1.len, 1, &options, &diag)) {
                    goto cleanup;
                }
            }
            count1++;
            free(prev1.data);
            prev1 = lb1;
            init_line_buffer(&lb1);
            len1 = get_line(&lb1, f1, delimiter);
        }
        else {
            if (!options.no_check_order) {
                if (!check_file_order(&diag, 2, &prev2, &lb2, &order_ok2, options.check_order))
                    goto cleanup;
            }
            if (!options.suppress_2) {
                if (!print_column(&writer, lb2.data, lb2.len, 2, &options, &diag)) {
                    goto cleanup;
                }
            }
            count2++;
            free(prev2.data);
            prev2 = lb2;
            init_line_buffer(&lb2);
            len2 = get_line(&lb2, f2, delimiter);
        }
    }

    if (!order_ok1 || !order_ok2) {
        fprintf(stderr, "comm: input is not in sorted order\n");
    }

    if (options.total) {
        if (!bx_comm_write_total(&writer, count1, count2, count3, &options, &diag)) {
            goto cleanup;
        }
    }

cleanup:
    if (bx_line_writer_error(&writer) == 0 && !bx_line_writer_flush(&writer)) {
        bx_comm_write_error(&diag);
    }
    free_line_buffer(&lb1);
    free_line_buffer(&lb2);
    free_line_buffer(&prev1);
    free_line_buffer(&prev2);
    bx_fclose_nonstdio(f1, f1_is_stdio);
    bx_fclose_nonstdio(f2, f2_is_stdio);

    return diag.exit_status;
}
