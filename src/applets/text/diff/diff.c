/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "applets.h"
#include "applets/text/edit/vim/src/xdiff/xdiff.h"
#include "bx/diag.h"
#include "lib/args_common.h"
#include "lib/cli_common.h"
#include "lib/fd_ops.h"
#include "lib/output_quote.h"
#include "lib/text_buffer.h"

/* Bound the input and per-line memory of the interactive editor's engine. */
enum {
    DIFF_BYTES_MAX = 8 * 1024 * 1024,
    DIFF_LINES_MAX = 100000,
    DIFF_MINIMAL_LINES_MAX = 4096,
};

static void usage(FILE* stream) {
    fputs("Usage: diff [-d] [-u | -U LINES] [--] FILE1 FILE2\n"
          "Compare two files in unified format (default: three context lines).\n"
          "-d (or --minimal) performs a minimal comparison, limited to 4096 text lines.\n"
          "One operand may be '-' for standard input. Directories are not supported.\n"
          "Limits per input: 8 MiB and 100000 text lines.\n", stream);
}

static bool read_input(const char* path, struct bx_text_buffer* data, struct bx_diag_ctx* diag) {
    bool standard = strcmp(path, "-") == 0;
    int fd = standard ? STDIN_FILENO : bx_fd_open_cloexec(path, O_RDONLY | O_NOCTTY, 0);
    if (fd < 0) {
        bx_perror_path(diag, path);
        return false;
    }
    bool success = false;
    struct stat st;
    if (fstat(fd, &st) < 0)
        bx_perror_path(diag, path);
    else if (S_ISDIR(st.st_mode))
        bx_diag(diag, "%s: directory comparison is not supported", path);
    else if (S_ISREG(st.st_mode) && st.st_size > DIFF_BYTES_MAX)
        bx_diag(diag, "%s: input exceeds 8 MiB", path);
    else {
        char block[16384];
        for (;;) {
            ssize_t n = read(fd, block, sizeof(block));
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0) {
                bx_perror_path(diag, path);
                break;
            }
            if (n == 0) {
                success = true;
                break;
            }
            if ((size_t)n > DIFF_BYTES_MAX - data->length) {
                bx_diag(diag, "%s: input exceeds 8 MiB", path);
                break;
            }
            if (!bx_text_buffer_append_span(data, block, (size_t)n)) {
                bx_perror_path(diag, path);
                break;
            }
        }
    }
    if (!standard)
        bx_fd_cleanup(&fd);
    return success;
}

static bool within_line_limit(const struct bx_text_buffer* data, size_t limit) {
    size_t lines = data->length != 0 && data->data[data->length - 1] != '\n';
    const char* next = data->data;
    size_t remaining = data->length;
    while (remaining != 0) {
        const char* found = memchr(next, '\n', remaining);
        if (!found)
            break;
        if (++lines > limit)
            return false;
        remaining -= (size_t)(found - next) + 1u;
        next = found + 1;
    }
    return true;
}

struct diff_output {
    const char* left;
    const char* right;
    bool header;
};

static int emit_lines(void* private, mmbuffer_t* lines, int count) {
    struct diff_output* output = private;
    if (!output->header) {
        if (printf("--- %s\n+++ %s\n", output->left, output->right) < 0)
            return -1;
        output->header = true;
    }
    for (int i = 0; i < count; i++) {
        size_t length = (size_t)lines[i].size;
        if (fwrite(lines[i].ptr, 1, length, stdout) != length)
            return -1;
    }
    return 0;
}

static char* quote_path(const char* path) {
    /* Tabs delimit optional timestamps; controls and quotes cannot appear
     * literally in a header that another tool may parse. */
    for (const unsigned char* p = (const unsigned char*)path; *p; p++) {
        if (*p < ' ' || *p >= 127 || *p == '"' || *p == '\\')
            return bx_output_quote_dup(path, BX_OUTPUT_QUOTE_C);
    }
    return strdup(path);
}

int bx_diff_main(int argc, char** argv) {
    struct bx_diag_ctx diag = {.progname = "diff"};
    static const struct option options[] = {
        {"help", no_argument, NULL, 'h'},
        {"minimal", no_argument, NULL, 'd'},
        {"unified", required_argument, NULL, 'U'},
        {NULL, 0, NULL, 0},
    };
    int opt, operand = 1;
    size_t context = 3;
    bool minimal = false;
    bx_args_getopt_reset();
    while ((opt = bx_args_getopt_long_interspersed(argc, argv, "-:dhuU:", options, &operand)) != -1) {
        switch (opt) {
        case 'd':
            minimal = true;
            break;
        case 'h':
            usage(stdout);
            return 0;
        case 'u':
            context = 3;
            break;
        case 'U':
            if (!bx_args_parse_size_range(optarg, 0, DIFF_LINES_MAX, &context)) {
                bx_diag(&diag, "invalid context size: '%s'", optarg);
                return 2;
            }
            break;
        case ':':
            bx_cli_diag_option_requires_arg(&diag, optopt, optind, argc, argv);
            return 2;
        default:
            bx_cli_diag_unrecognized_option(&diag, optopt, optind, argc, argv);
            return 2;
        }
    }
    if (argc - optind != 2 ||
        (strcmp(argv[optind], "-") == 0 && strcmp(argv[optind + 1], "-") == 0)) {
        usage(stderr);
        return 2;
    }

    const char* left = argv[optind];
    const char* right = argv[optind + 1];
    struct bx_text_buffer a = {0}, b = {0};
    char* left_label = NULL;
    char* right_label = NULL;
    int status = 2;
    // Minimal search skips xdiff's early cutoffs, so it needs a lower line cap.
    size_t line_limit = minimal ? DIFF_MINIMAL_LINES_MAX : DIFF_LINES_MAX;
    if (!read_input(left, &a, &diag) || !read_input(right, &b, &diag))
        goto done;
    if (a.length == b.length && (!a.length || memcmp(a.data, b.data, a.length) == 0)) {
        status = 0;
        goto done;
    }
    left_label = quote_path(left);
    right_label = quote_path(right);
    if (!left_label || !right_label) {
        bx_perror_path(&diag, "file label");
        goto done;
    }
    if ((a.length && memchr(a.data, '\0', a.length)) ||
        (b.length && memchr(b.data, '\0', b.length))) {
        if (printf("Binary files %s and %s differ\n", left_label, right_label) >= 0)
            status = 1;
    }
    else if (!within_line_limit(&a, line_limit) || !within_line_limit(&b, line_limit)) {
        bx_diag(&diag, "input exceeds %zu text lines", line_limit);
        goto done;
    }
    else {
        mmfile_t old = {.ptr = a.data ? a.data : "", .size = (long)a.length};
        mmfile_t new = {.ptr = b.data ? b.data : "", .size = (long)b.length};
        xpparam_t params = {.flags = minimal ? XDF_NEED_MINIMAL : 0};
        xdemitconf_t config = {.ctxlen = (long)context};
        struct diff_output output = {.left = left_label, .right = right_label};
        xdemitcb_t callbacks = {.priv = &output, .out_line = emit_lines};
        if (xdl_diff(&old, &new, &params, &config, &callbacks) == 0)
            status = 1;
        else if (!ferror(stdout))
            bx_diag(&diag, "could not compute the difference");
    }
    if (fflush(stdout) == EOF || ferror(stdout)) {
        bx_perror_path(&diag, "standard output");
        status = 2;
    }
done:
    free(left_label);
    free(right_label);
    bx_text_buffer_destroy(&a);
    bx_text_buffer_destroy(&b);
    return status;
}
