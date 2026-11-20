/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <getopt.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "applets.h"
#include "applets/archive/archive_codec.h"
#include "bx/diag.h"
#include "lib/args_common.h"
#include "lib/cli_common.h"
#include "lib/copy_metadata.h"
#include "lib/fd_ops.h"
#include "lib/path_ops.h"
#include "lib/xreadwrite.h"

static void input_error(void* user, const char* progname, const char* message) {
    const char* name = *(const char**)user;
    fprintf(stderr, "%s: %s: %s\n", progname, name, message);
}

static int decode(const char* name, int source_fd, int output_fd, const char* output_label,
                  const struct bx_archive_codec* codec, struct bx_diag_ctx* diag,
                  bool* output_error) {
    const struct bx_archive_codec_input_options options = {
        .archive_path = name,
        .required_codec = codec,
        .seek_mode = BX_ARCHIVE_CODEC_SEEK_DISABLE,
        .xz_memory_limit = UINT64_C(256) << 20,
    };
    struct bx_archive_codec_input* input;
    bool opened = source_fd < 0 ? bx_archive_codec_input_open(&input, &options, diag) :
                                 bx_archive_codec_input_open_fd(&input, source_fd, &options, diag);
    if (!opened)
        return 1;

    const char* label = strcmp(name, "-") == 0 ? "(stdin)" : name;
    struct bx_diag_ctx reader_diag = {
        .progname = diag->progname, .emit = input_error,
        .emit_user = &label,
    };
    int status = 0;
    unsigned char buffer[16384];
    for (;;) {
        size_t len;
        if (!bx_archive_codec_input_read_some(input, buffer, sizeof(buffer), &len, &reader_diag) ||
            (!len && !bx_archive_codec_input_finish_success(input, &reader_diag))) {
            status = codec == bx_archive_codec_bzip2() && bx_archive_codec_input_decode_failed(input) ? 2 : 1;
            break;
        }
        if (!len)
            break;
        if (!bx_xwrite_all(output_fd, buffer, len)) {
            bx_perror_path(diag, output_label);
            *output_error = true;
            status = 1;
            break;
        }
    }
    bx_archive_codec_input_close(input);
    return status;
}

static bool same_source(const struct stat* a, const struct stat* b) {
    return S_ISREG(b->st_mode) && a->st_dev == b->st_dev && a->st_ino == b->st_ino &&
           a->st_size == b->st_size && a->st_ctim.tv_sec == b->st_ctim.tv_sec &&
           a->st_ctim.tv_nsec == b->st_ctim.tv_nsec && a->st_mtim.tv_sec == b->st_mtim.tv_sec &&
           a->st_mtim.tv_nsec == b->st_mtim.tv_nsec;
}

static int decode_to_new_file(const char* name, const char* suffix,
                              const struct bx_archive_codec* codec, struct bx_diag_ctx* diag) {
    struct bx_path_split parts = bx_path_split(name, false);
    size_t suffix_len = strlen(suffix);
    if (parts.basename_length <= suffix_len ||
        strcmp(parts.basename + parts.basename_length - suffix_len, suffix) != 0) {
        bx_diag(diag, "%s: expected suffix %s", name, suffix);
        return 1;
    }
    char* parent = parts.parent_length ? strndup(name, parts.parent_length) : NULL;
    char* target = strndup(parts.basename, parts.basename_length - suffix_len);
    int dir = -1, source = -1, output = -1;
    int status = 1;
    if (!target || (parts.parent_length && !parent)) {
        bx_perror_path(diag, name);
        goto done;
    }
    if (strcmp(target, ".") == 0 || strcmp(target, "..") == 0) {
        bx_diag(diag, "%s: invalid destination name", name);
        goto done;
    }
    dir = bx_fd_open_cloexec(parent ? parent : ".", O_RDONLY | O_DIRECTORY, 0);
    struct stat st, current;
    if (dir < 0 || fstatat(dir, parts.basename, &st, AT_SYMLINK_NOFOLLOW) < 0) {
        bx_perror_path(diag, name);
        goto done;
    }
    if (!S_ISREG(st.st_mode)) {
        bx_diag(diag, "%s: input is not a regular file", name);
        goto done;
    }
    source = bx_fd_openat_regular_verified(dir, parts.basename, &st, &st);
    if (source < 0) {
        bx_perror_path(diag, name);
        goto done;
    }
    if (fstatat(dir, target, &current, AT_SYMLINK_NOFOLLOW) == 0) {
        errno = EEXIST;
        bx_perror_path(diag, target);
        goto done;
    }
    if (errno != ENOENT || (output = bx_fd_open_anonymous_file_at(dir)) < 0) {
        bx_perror_path(diag, target);
        goto done;
    }
    bool output_error = false;
    status = decode(name, source, output, target, codec, diag, &output_error);
    if (status != 0)
        goto done;
    status = 1;
    struct stat metadata = st;
    metadata.st_mode &= 0777;
    if (!bx_copy_fd_metadata(-1, output, &metadata, BX_PRESERVE_MODE | BX_PRESERVE_TIMESTAMPS) ||
        bx_fd_fsync(output) < 0) {
        bx_perror_path(diag, target);
        goto done;
    }
    if (fstatat(dir, parts.basename, &current, AT_SYMLINK_NOFOLLOW) < 0 ||
        !same_source(&st, &current)) {
        bx_diag(diag, "%s: input changed; output not published", name);
        goto done;
    }
    // Publication cannot replace an existing directory entry. Until here
    // closing the anonymous descriptor discards any partial output.
    if (bx_fd_publish_anonymous_file_at(output, dir, target) < 0) {
        bx_perror_path(diag, target);
        goto done;
    }
    status = 0;
done:
    bx_fd_cleanup(&output);
    bx_fd_cleanup(&source);
    bx_fd_cleanup(&dir);
    free(parent);
    free(target);
    return status;
}

static int archive_cat(int argc, char** argv, const char* progname,
                       const struct bx_archive_codec* codec, const char* suffix) {
    struct bx_diag_ctx diag = {.progname = progname};
    static const struct option options[] = {
        {"decompress", no_argument, NULL, 'd'},
        {"stdout", no_argument, NULL, 'c'},
        {"help", no_argument, NULL, 'h'},
        {"keep", no_argument, NULL, 'k'},
        {0},
    };
    int opt, operand = 1;
    bool to_stdout = suffix == NULL;
    bool keep = false;
    bx_args_getopt_reset();
    while ((opt = bx_args_getopt_long_interspersed(argc, argv, "-:cdhk", options, &operand)) != -1) {
        switch (opt) {
        case 'c':
            to_stdout = true;
            break;
        case 'd':
            break;
        case 'k':
            keep = true;
            break;
        case 'h':
            printf("Usage: %s [-cdk] [--] [FILE...]\n", progname);
            if (suffix)
                printf("Use -k to decode %s files to new files (without their suffix).\n"
                       "Use -c for standard output. Replacement and source deletion are not supported.\n",
                       suffix);
            else
                printf("Decompress %s data to standard output without changing the input files.\n",
                       bx_archive_codec_name(codec));
            puts("With no files or a '-' operand, read standard input and write to standard output.");
            return 0;
        default:
            bx_cli_diag_unrecognized_option(&diag, optopt, optind, argc, argv);
            return 1;
        }
    }
    bool output_error = false;
    if (optind == argc)
        return decode("-", -1, STDOUT_FILENO, "standard output", codec, &diag, &output_error);
    if (!to_stdout && !keep) {
        for (int i = optind; i < argc; i++)
            if (strcmp(argv[i], "-") != 0) {
                bx_diag(&diag, "use -k to keep the input or -c for standard output");
                return 1;
            }
    }
    int status = 0;
    for (int i = optind; i < argc && !output_error; i++) {
        int result = to_stdout || strcmp(argv[i], "-") == 0 ?
                     decode(argv[i], -1, STDOUT_FILENO, "standard output", codec, &diag, &output_error) :
                     decode_to_new_file(argv[i], suffix, codec, &diag);
        if (status < result)
            status = result;
    }
    return status;
}

int bx_bzcat_main(int argc, char** argv) {
    return archive_cat(argc, argv, "bzcat", bx_archive_codec_bzip2(), NULL);
}

int bx_xzcat_main(int argc, char** argv) {
    return archive_cat(argc, argv, "xzcat", bx_archive_codec_xz(), NULL);
}

int bx_bunzip2_main(int argc, char** argv) {
    return archive_cat(argc, argv, "bunzip2", bx_archive_codec_bzip2(), ".bz2");
}

int bx_unxz_main(int argc, char** argv) {
    return archive_cat(argc, argv, "unxz", bx_archive_codec_xz(), ".xz");
}
