#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "applets.h"
#include "bx/diag.h"
#include "lib/fd_ops.h"
#include "lib/size_parse.h"
#include "lib/xreadwrite.h"

enum cmp_status { CMP_EQUAL = 0, CMP_DIFFERENT = 1, CMP_ERROR = 2 };
enum cmp_mode { CMP_FIRST, CMP_LIST, CMP_SILENT };

struct cmp_input {
    const char* name;
    int fd;
    size_t start, end;
    unsigned char data[16384];
};

static int cmp_skip(struct cmp_input* input, uintmax_t skip) {
    if (skip == 0)
        return 0;
    struct stat st;
    off_t offset = (off_t)skip;
    if (offset >= 0 && (uintmax_t)offset == skip &&
        fstat(input->fd, &st) == 0 && S_ISREG(st.st_mode) &&
        lseek(input->fd, offset, SEEK_CUR) != (off_t)-1)
        return 0;

    while (skip != 0) {
        size_t count = skip < sizeof(input->data) ? (size_t)skip : sizeof(input->data);
        ssize_t n = bx_xread(input->fd, input->data, count);
        if (n <= 0)
            return (int)n;
        skip -= (uintmax_t)n;
    }
    return 0;
}

static int cmp_refill(struct cmp_input* input, uintmax_t remaining) {
    if (input->start != input->end)
        return 0;
    size_t count = remaining < sizeof(input->data) ? (size_t)remaining : sizeof(input->data);
    ssize_t n = bx_xread(input->fd, input->data, count);
    if (n < 0)
        return -1;
    input->start = 0;
    input->end = (size_t)n;
    return 0;
}

static int cmp_compare(struct cmp_input* a, struct cmp_input* b, enum cmp_mode mode, uintmax_t limit, struct bx_diag_ctx* diag) {
    uintmax_t bytes = 0, line = 1;
    int status = CMP_EQUAL;
    while (bytes < limit) {
        struct cmp_input* failed = NULL;
        if (cmp_refill(a, limit - bytes) < 0)
            failed = a;
        else if (cmp_refill(b, limit - bytes) < 0)
            failed = b;
        if (failed != NULL) {
            if (mode != CMP_SILENT)
                bx_perror_path(diag, failed->name);
            return CMP_ERROR;
        }

        size_t left = a->end - a->start, right = b->end - b->start;
        size_t count = left < right ? left : right;
        if (count == 0) {
            if (left == right)
                break;
            if (mode != CMP_SILENT) {
                const char* name = left == 0 ? a->name : b->name;
                if (bytes == 0)
                    bx_diag(diag, "EOF on '%s' which is empty", name);
                else
                    bx_diag(diag, "EOF on '%s' after byte %" PRIuMAX, name, bytes);
            }
            return CMP_DIFFERENT;
        }

        const unsigned char* x = a->data + a->start;
        const unsigned char* y = b->data + b->start;
        bool differs = memcmp(x, y, count) != 0;
        if (differs && mode == CMP_SILENT)
            return CMP_DIFFERENT;
        if (differs || mode == CMP_FIRST) {
            for (size_t i = 0; i < count; i++) {
                if (differs && x[i] != y[i]) {
                    if (mode == CMP_FIRST) {
                        if (printf("%s %s differ: char %" PRIuMAX ", line %" PRIuMAX "\n",
                                   a->name, b->name, bytes + i + 1, line) < 0)
                            return CMP_ERROR;
                        return CMP_DIFFERENT;
                    }
                    if (printf("%" PRIuMAX " %3o %3o\n", bytes + i + 1, (unsigned)x[i], (unsigned)y[i]) < 0)
                        return CMP_ERROR;
                    status = CMP_DIFFERENT;
                }
                if (mode == CMP_FIRST && x[i] == '\n' && line != UINTMAX_MAX)
                    line++;
            }
        }
        a->start += count;
        b->start += count;
        bytes += count;
    }
    return status;
}

int bx_cmp_main(int argc, char** argv) {
    static const struct option options[] = {
        {"ignore-initial", required_argument, NULL, 'i'},
        {"verbose", no_argument, NULL, 'l'},
        {"bytes", required_argument, NULL, 'n'},
        {"quiet", no_argument, NULL, 's'},
        {"silent", no_argument, NULL, 's'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    struct bx_diag_ctx diag = {.progname = "cmp"};
    uintmax_t skip[2] = {0, 0}, limit = UINTMAX_MAX;
    enum cmp_mode mode = CMP_FIRST;
    int ch;
    while ((ch = getopt_long(argc, argv, "i:ln:s", options, NULL)) != -1) {
        switch (ch) {
            case 'i': {
                const char* colon = strchr(optarg, ':');
                char* first = colon != NULL ? strndup(optarg, (size_t)(colon - optarg)) : NULL;
                bool valid = colon != NULL ?
                    first != NULL && bx_size_parse_uint(first, &skip[0]) &&
                        bx_size_parse_uint(colon + 1, &skip[1]) :
                    bx_size_parse_uint(optarg, &skip[0]);
                free(first);
                if (!valid) {
                    bx_diag(&diag, "invalid initial offset: '%s'", optarg);
                    return CMP_ERROR;
                }
                if (colon == NULL)
                    skip[1] = skip[0];
                break;
            }
            case 'n':
                if (!bx_size_parse_uint(optarg, &limit)) {
                    bx_diag(&diag, "invalid byte limit: '%s'", optarg);
                    return CMP_ERROR;
                }
                break;
            case 'l':
            case 's':
                if ((ch == 'l' && mode == CMP_SILENT) || (ch == 's' && mode == CMP_LIST)) {
                    bx_diag(&diag, "-l and -s are incompatible");
                    return CMP_ERROR;
                }
                mode = ch == 'l' ? CMP_LIST : CMP_SILENT;
                break;
            case 'h':
                return puts("usage: cmp [-l|-s] [-i SKIP[:SKIP]] [-n LIMIT] FILE1 [FILE2 [SKIP1 [SKIP2]]]\n"
                            "SKIP and LIMIT are decimal byte counts.") < 0 ? CMP_ERROR : CMP_EQUAL;
            default:
                return CMP_ERROR;
        }
    }

    int operands = argc - optind;
    if (operands < 1 || operands > 4) {
        bx_diag(&diag, "expected one or two files and optional initial offsets");
        return CMP_ERROR;
    }
    for (int i = 2; i < operands; i++) {
        if (!bx_size_parse_uint(argv[optind + i], &skip[i - 2])) {
            bx_diag(&diag, "invalid initial offset: '%s'", argv[optind + i]);
            return CMP_ERROR;
        }
    }
    struct cmp_input input[2] = {
        {.name = argv[optind], .fd = -1},
        {.name = operands > 1 ? argv[optind + 1] : "-", .fd = -1},
    };
    bool stdin_file[] = {strcmp(input[0].name, "-") == 0, strcmp(input[1].name, "-") == 0};
    if (stdin_file[0] && stdin_file[1]) {
        if (skip[0] == skip[1])
            return CMP_EQUAL;
        bx_diag(&diag, "cannot compare standard input at two different offsets");
        return CMP_ERROR;
    }

    int status = CMP_ERROR;
    for (size_t i = 0; i < 2; i++) {
        input[i].fd = stdin_file[i] ? STDIN_FILENO : bx_fd_open_cloexec(input[i].name, O_RDONLY, 0);
        if (input[i].fd < 0 || (limit != 0 && cmp_skip(&input[i], skip[i]) < 0)) {
            if (mode != CMP_SILENT)
                bx_perror_path(&diag, input[i].name);
            goto done;
        }
    }
    status = cmp_compare(&input[0], &input[1], mode, limit, &diag);
    if (mode != CMP_SILENT && (fflush(stdout) == EOF || ferror(stdout))) {
        bx_perror_path(&diag, "standard output");
        status = CMP_ERROR;
    }
done:
    for (size_t i = 0; i < 2; i++) {
        if (!stdin_file[i] && input[i].fd >= 0)
            bx_fd_cleanup(&input[i].fd);
    }
    return status;
}
