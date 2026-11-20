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

static bool encode_input(void* user, const struct bx_archive_codec_stream_sink* sink,
                          struct bx_diag_ctx* diag) {
    const int fd = *(const int*)user;
    unsigned char buffer[16384];
    for (;;) {
        ssize_t n = bx_xread(fd, buffer, sizeof(buffer));
        if (n == 0)
            return true;
        if (n < 0) {
            bx_perror_path(diag, "read");
            return false;
        }
        if (!sink->write(sink->user, buffer, (size_t)n))
            return false; // The codec callback diagnoses output errors.
    }
}

struct encode_sink {
    int fd;
    bool* error;
};

static bool encode_output(void* user, const void* data, size_t len) {
    struct encode_sink* output = user;
    if (bx_xwrite_all(output->fd, data, len))
        return true;
    *output->error = true;
    return false;
}

static int encode(const char* name, int source_fd, int output_fd,
                  const struct bx_archive_codec* codec,
                  const struct bx_archive_codec_encode_options* options, struct bx_diag_ctx* diag,
                  bool* output_error) {
    bool standard = strcmp(name, "-") == 0;
    bool owned = source_fd < 0 && !standard;
    int fd = owned ? bx_fd_open_read(name, diag) : source_fd < 0 ? STDIN_FILENO : source_fd;
    if (fd < 0)
        return 1;
    const char* label = standard ? "(stdin)" : name;
    struct bx_diag_ctx codec_diag = {
        .progname = diag->progname, .emit = input_error, .emit_user = &label,
    };
    struct encode_sink output = {.fd = output_fd, .error = output_error};
    const struct bx_archive_codec_stream_sink sink = {.user = &output, .write = encode_output};
    bool ok = bx_archive_codec_run_encode_stream(codec, options, encode_input, &fd, &sink, &codec_diag);
    if (owned && !bx_fd_close(&fd, name, diag))
        ok = false;
    return ok ? 0 : 1;
}

static bool same_source(const struct stat* a, const struct stat* b) {
    return S_ISREG(b->st_mode) && a->st_dev == b->st_dev && a->st_ino == b->st_ino &&
           a->st_size == b->st_size && a->st_ctim.tv_sec == b->st_ctim.tv_sec &&
           a->st_ctim.tv_nsec == b->st_ctim.tv_nsec && a->st_mtim.tv_sec == b->st_mtim.tv_sec &&
           a->st_mtim.tv_nsec == b->st_mtim.tv_nsec;
}

static int transform_to_new_file(const char* name, const char* suffix,
                                 const struct bx_archive_codec* codec,
                                 const struct bx_archive_codec_encode_options* encoding,
                                 struct bx_diag_ctx* diag) {
    struct bx_path_split parts = bx_path_split(name, false);
    size_t suffix_len = strlen(suffix);
    bool has_suffix = parts.basename_length >= suffix_len &&
                      strcmp(parts.basename + parts.basename_length - suffix_len, suffix) == 0;
    if (!parts.basename_length || (encoding ? has_suffix :
        (!has_suffix || parts.basename_length == suffix_len))) {
        if (encoding)
            bx_diag(diag, "%s: expected a filename without suffix %s", name, suffix);
        else
            bx_diag(diag, "%s: expected suffix %s", name, suffix);
        return 1;
    }
    char* parent = parts.parent_length ? strndup(name, parts.parent_length) : NULL;
    char* target = NULL;
    if (encoding) {
        target = malloc(parts.basename_length + suffix_len + 1);
        if (target) {
            memcpy(target, parts.basename, parts.basename_length);
            memcpy(target + parts.basename_length, suffix, suffix_len + 1);
        }
    } else
        target = strndup(parts.basename, parts.basename_length - suffix_len);
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
    status = encoding ? encode(name, source, output, codec, encoding, diag, &output_error) :
                        decode(name, source, output, target, codec, diag, &output_error);
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

static int archive_command(int argc, char** argv, const char* progname,
                           const struct bx_archive_codec* codec, const char* suffix,
                           bool compress) {
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
    bool writer = compress;
    bool keep = false;
    struct bx_archive_codec_encode_options encoding = {0};
    bx_args_getopt_reset();
    while ((opt = bx_args_getopt_long_interspersed(argc, argv, "-:cdhk0123456789", options, &operand)) != -1) {
        if (opt >= '0' && opt <= '9' && writer) {
            encoding.level_set = true;
            encoding.level = opt - '0';
            continue;
        }
        switch (opt) {
        case 'c':
            to_stdout = true;
            break;
        case 'd':
            compress = false;
            break;
        case 'k':
            keep = true;
            break;
        case 'h':
            printf("Usage: %s %s [--] [FILE...]\n", progname,
                   writer ? "[-cdk] [-LEVEL]" : "[-cdk]");
            if (writer)
                printf("Use -c to compress %s data to stdout or -dc to decode it.\n"
                       "Use -k to write a new file without overwriting or deleting the input.\n"
                       "Use a single-digit compression level (%d to 9).\n",
                       bx_archive_codec_name(codec), codec == bx_archive_codec_xz() ? 0 : 1);
            else if (suffix)
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
    if (encoding.level_set &&
        (!compress || !bx_archive_codec_level_valid(codec, encoding.level))) {
        bx_diag(&diag, "invalid compression level for this mode: %d", encoding.level);
        return 1;
    }
    bool output_error = false;
    if (optind == argc)
        return compress ? encode("-", -1, STDOUT_FILENO, codec, &encoding, &diag, &output_error) :
                          decode("-", -1, STDOUT_FILENO, "standard output", codec, &diag, &output_error);
    if (!to_stdout && !keep) {
        for (int i = optind; i < argc; i++)
            if (strcmp(argv[i], "-") != 0) {
                bx_diag(&diag, "use -k to keep the input or -c for standard output");
                return 1;
            }
    }
    int status = 0;
    for (int i = optind; i < argc && !output_error; i++) {
        int result;
        if (to_stdout || strcmp(argv[i], "-") == 0)
            result = compress ?
                encode(argv[i], -1, STDOUT_FILENO, codec, &encoding, &diag, &output_error) :
                decode(argv[i], -1, STDOUT_FILENO, "standard output", codec, &diag, &output_error);
        else
            result = transform_to_new_file(argv[i], suffix, codec, compress ? &encoding : NULL, &diag);
        if (status < result)
            status = result;
    }
    return status;
}

int bx_bzcat_main(int argc, char** argv) {
    return archive_command(argc, argv, "bzcat", bx_archive_codec_bzip2(), NULL, false);
}

int bx_xzcat_main(int argc, char** argv) {
    return archive_command(argc, argv, "xzcat", bx_archive_codec_xz(), NULL, false);
}

int bx_bunzip2_main(int argc, char** argv) {
    return archive_command(argc, argv, "bunzip2", bx_archive_codec_bzip2(), ".bz2", false);
}

int bx_unxz_main(int argc, char** argv) {
    return archive_command(argc, argv, "unxz", bx_archive_codec_xz(), ".xz", false);
}

int bx_bzip2_main(int argc, char** argv) {
    return archive_command(argc, argv, "bzip2", bx_archive_codec_bzip2(), ".bz2", true);
}

int bx_xz_main(int argc, char** argv) {
    return archive_command(argc, argv, "xz", bx_archive_codec_xz(), ".xz", true);
}
