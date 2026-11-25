/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <fcntl.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "applets.h"
#include "applets/archive/archive_codec.h"
#include "bx/diag.h"
#include "lib/copy_metadata.h"
#include "lib/dir_path.h"
#include "lib/fd_ops.h"
#include "lib/option_scan.h"
#include "lib/path_ops.h"
#include "lib/xreadwrite.h"

static void input_error(void* user, const char* progname, const char* message) {
    const char* name = *(const char**)user;
    fprintf(stderr, "%s: %s: %s\n", progname, name, message);
}

static bool legacy_lzma(const struct bx_archive_codec* codec, const char* name) {
    size_t len = strlen(name);
    return codec == bx_archive_codec_xz() && len >= sizeof(".lzma") - 1 &&
           strcmp(name + len - (sizeof(".lzma") - 1), ".lzma") == 0;
}

static int decode(const char* name, int source_fd, int output_fd, const char* output_label,
                  const struct bx_archive_codec* codec, struct bx_diag_ctx* diag,
                  bool* output_error) {
    const struct bx_archive_codec_input_options options = {
        .archive_path = name,
        .required_codec = codec,
        .seek_mode = BX_ARCHIVE_CODEC_SEEK_DISABLE,
        .xz_memory_limit = UINT64_C(256) << 20,
        .xz_lzma_alone = legacy_lzma(codec, name),
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

static bool controlled_parent(int dir, const char* name, struct bx_diag_ctx* diag) {
    struct stat st;
    if (fstat(dir, &st) < 0) {
        bx_perror_path(diag, name);
        return false;
    }
    if (st.st_uid != geteuid() || (st.st_mode & (S_IWGRP | S_IWOTH))) {
        bx_diag(diag, "%s: replacement or source removal requires a directory owned by the caller and not writable by others", name);
        return false;
    }
    return true;
}

static bool publish_replacement(int fd, int parent, const char* target, struct bx_diag_ctx* diag) {
    // Normal cancellation can interrupt the long transform. Defer it only
    // while the completed output has a named staging entry to clean up.
    sigset_t signals, saved;
    sigemptyset(&signals);
    sigaddset(&signals, SIGHUP);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &signals, &saved) != 0) {
        bx_perror_path(diag, "could not block signals for publication");
        return false;
    }
    char name[] = ".bx-compress-XXXXXX";
    bool published = false;
    if (!bx_dir_path_mkdtemp_at(parent, name)) {
        bx_perror_path(diag, target);
        goto done;
    }
    int staging = bx_fd_openat_child_nofollow(parent, name, O_RDONLY | O_DIRECTORY, 0);
    bool linked = staging >= 0 && bx_fd_publish_anonymous_file_at(fd, staging, "output") == 0;
    published = linked && bx_fd_renameat_child(staging, "output", parent, target) == 0;
    if (!published)
        bx_perror_path(diag, target);
    if (linked && !published && bx_fd_unlinkat_child(staging, "output", 0) < 0)
        bx_perror_path(diag, name);
    bx_fd_cleanup(&staging);
    if (bx_fd_unlinkat_child(parent, name, AT_REMOVEDIR) < 0) {
        bx_perror_path(diag, name);
        published = false;
    }
done:
    if (sigprocmask(SIG_SETMASK, &saved, NULL) != 0) {
        bx_perror_path(diag, "could not restore the signal mask");
        return false;
    }
    return published;
}

struct archive_operand {
    const char* name;
    struct bx_path_split parts; /* Populated only for checked named outputs. */
    size_t suffix_len;
};

static bool preflight_transform_name(struct archive_operand* operand, const char* suffix,
                                     const struct bx_archive_codec* codec, bool compress,
                                     struct bx_diag_ctx* diag) {
    const char* name = operand->name;
    struct bx_path_split parts = bx_path_split(name, false);
    if (!compress && legacy_lzma(codec, name))
        suffix = ".lzma";
    size_t suffix_len = strlen(suffix);
    bool has_suffix = parts.basename_length >= suffix_len &&
                      strcmp(parts.basename + parts.basename_length - suffix_len, suffix) == 0;
    if (!parts.basename_length || (compress ? has_suffix :
        (!has_suffix || parts.basename_length == suffix_len))) {
        if (compress)
            bx_diag(diag, "%s: expected a filename without suffix %s", name, suffix);
        else
            bx_diag(diag, "%s: expected suffix %s", name, suffix);
        return false;
    }
    if (bx_path_is_dot_or_dotdot(parts.basename)) {
        bx_diag(diag, "%s: invalid input name", name);
        return false;
    }
    if (!compress) {
        size_t target_len = parts.basename_length - suffix_len;
        if ((target_len == 1 && parts.basename[0] == '.') ||
            (target_len == 2 && memcmp(parts.basename, "..", 2) == 0)) {
            bx_diag(diag, "%s: invalid destination name", name);
            return false;
        }
    }
    operand->parts = parts;
    operand->suffix_len = suffix_len;
    return true;
}

static int transform_to_new_file(const struct archive_operand* operand, const char* suffix,
                                 const struct bx_archive_codec* codec,
                                 const struct bx_archive_codec_encode_options* encoding,
                                 bool replace, bool keep, struct bx_diag_ctx* diag) {
    const char* name = operand->name;
    struct bx_path_split parts = operand->parts;
    size_t suffix_len = operand->suffix_len;
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
    dir = bx_fd_open_cloexec(parent ? parent : ".", O_RDONLY | O_DIRECTORY, 0);
    struct stat st, current;
    if (dir < 0 || fstatat(dir, parts.basename, &st, AT_SYMLINK_NOFOLLOW) < 0) {
        bx_perror_path(diag, name);
        goto done;
    }
    if ((replace || !keep) && !controlled_parent(dir, name, diag))
        goto done;
    if (!S_ISREG(st.st_mode)) {
        bx_diag(diag, "%s: input is not a regular file", name);
        goto done;
    }
    source = bx_fd_openat_regular_verified(dir, parts.basename, &st, &st);
    if (source < 0) {
        bx_perror_path(diag, name);
        goto done;
    }
    if (!keep && (st.st_uid != geteuid() || (st.st_mode & (S_IWGRP | S_IWOTH)) ||
                  (!replace && st.st_nlink != 1))) {
        bx_diag(diag, "%s: refusing to remove a shared input; use -k", name);
        goto done;
    }
    if (!replace) {
        if (fstatat(dir, target, &current, AT_SYMLINK_NOFOLLOW) == 0)
            errno = EEXIST;
        if (errno != ENOENT) {
            bx_perror_path(diag, target);
            goto done;
        }
    }
    if ((output = bx_fd_open_anonymous_file_at(dir)) < 0) {
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
    if ((replace || !keep) && !controlled_parent(dir, name, diag))
        goto done;
    // Until publication, closing the anonymous descriptor discards partial output.
    if (replace) {
        if (!publish_replacement(output, dir, target, diag))
            goto done;
    } else if (bx_fd_publish_anonymous_file_at(output, dir, target) < 0) {
        bx_perror_path(diag, target);
        goto done;
    }
    if (!keep) {
        // Commit the new directory entry before attempting any source removal.
        // The directory is caller-controlled, not a lock against same-user edits.
        struct stat written, published;
        if (bx_fd_fsync(dir) < 0 || fstat(output, &written) < 0 ||
            fstatat(dir, target, &published, AT_SYMLINK_NOFOLLOW) < 0 ||
            fstatat(dir, parts.basename, &current, AT_SYMLINK_NOFOLLOW) < 0) {
            bx_diag(diag, "%s: could not confirm publication; input not removed: %s", name, bx_strerror(errno));
            goto done;
        }
        if (!same_source(&written, &published) || !same_source(&st, &current)) {
            bx_diag(diag, "%s: input or output changed; input not removed", name);
            goto done;
        }
        if (bx_fd_unlinkat_child(dir, parts.basename, 0) < 0 || bx_fd_fsync(dir) < 0) {
            bx_perror_path(diag, name);
            goto done;
        }
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
    static const struct bx_option_spec specs[] = {
        {'d', 'd', "decompress", BX_OPTION_NO_VALUE},
        {'c', 'c', "stdout", BX_OPTION_NO_VALUE},
        {'f', 'f', "force", BX_OPTION_NO_VALUE},
        {'h', 'h', "help", BX_OPTION_NO_VALUE},
        {'k', 'k', "keep", BX_OPTION_NO_VALUE},
        {'0', '0', NULL, BX_OPTION_NO_VALUE},
        {'1', '1', NULL, BX_OPTION_NO_VALUE},
        {'2', '2', NULL, BX_OPTION_NO_VALUE},
        {'3', '3', NULL, BX_OPTION_NO_VALUE},
        {'4', '4', NULL, BX_OPTION_NO_VALUE},
        {'5', '5', NULL, BX_OPTION_NO_VALUE},
        {'6', '6', NULL, BX_OPTION_NO_VALUE},
        {'7', '7', NULL, BX_OPTION_NO_VALUE},
        {'8', '8', NULL, BX_OPTION_NO_VALUE},
        {'9', '9', NULL, BX_OPTION_NO_VALUE},
    };
    bool to_stdout = suffix == NULL;
    bool writer = compress;
    bool keep = false;
    bool replace = false;
    struct bx_archive_codec_encode_options encoding = {0};
    struct archive_operand* paths = argc > 1 ? calloc((size_t)(argc - 1), sizeof(*paths)) : NULL;
    if (argc > 1 && paths == NULL) {
        bx_diag(&diag, "memory exhausted");
        return 1;
    }
    size_t count = 0;
    int status = 1;
    struct bx_option_cursor cursor;
    bx_option_cursor_init(&cursor, argv, argc > 0 ? 1u : 0u, (size_t)argc,
                          BX_OPTION_ORDERED | BX_OPTION_ABBREVIATE, specs, sizeof(specs) / sizeof(specs[0]));
    struct bx_option_result option;
    while ((option = bx_option_next(&cursor)).event != BX_OPTION_END) {
        if (option.event == BX_OPTION_OPERAND) {
            paths[count++].name = option.token;
            continue;
        }
        if (option.event != BX_OPTION_MATCH) {
            if (option.short_name)
                bx_diag(&diag, "invalid option -- '%c'", option.short_name);
            else
                bx_diag(&diag, "unrecognized option '%s'", option.token);
            goto done;
        }
        int opt = option.spec->id;
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
        case 'f':
            if (!suffix) {
                bx_diag(&diag, "unrecognized option '--force'");
                goto done;
            }
            replace = true;
            break;
        case 'k':
            keep = true;
            break;
        case 'h':
            printf("Usage: %s %s [--] [FILE...]\n", progname,
                   writer ? "[-cdkf] [-LEVEL]" : suffix ? "[-cdkf]" : "[-cd]");
            if (writer)
                printf("Use -c to compress %s data to stdout or -dc to decode it.\n"
                       "Use -k to write a new file without overwriting or deleting the input.\n"
                       "Use a single-digit compression level (%d to 9).\n",
                       bx_archive_codec_name(codec), codec == bx_archive_codec_xz() ? 0 : 1);
            else if (suffix)
                printf("Use -k to decode %s files to new files (without their suffix).\n"
                       "Use -c for standard output.\n",
                       suffix);
            else
                printf("Decompress %s data to standard output without changing the input files.\n",
                       bx_archive_codec_name(codec));
            if (suffix)
                puts("Use -k to keep the source and -f to replace output. Source removal and\n"
                     "replacement require a directory you own that others cannot write.");
            puts("With no files or a '-' operand, read standard input and write to standard output.");
            if (codec == bx_archive_codec_xz())
                puts("Named .lzma inputs select the legacy LZMA decoder.");
            status = 0;
            goto done;
        default:
            bx_diag(&diag, "invalid option -- '%c'", option.short_name);
            goto done;
        }
    }
    if (encoding.level_set &&
        (!compress || !bx_archive_codec_level_valid(codec, encoding.level))) {
        bx_diag(&diag, "invalid compression level for this mode: %d", encoding.level);
        goto done;
    }
    if (replace && (to_stdout || count == 0)) {
        bx_diag(&diag, "replacement requires named file output");
        goto done;
    }
    for (size_t i = 0; i < count; i++) {
        if (!paths[i].name[0]) {
            bx_diag(&diag, "empty input path");
            goto done;
        }
        bool from_stdin = strcmp(paths[i].name, "-") == 0;
        if (replace && from_stdin) {
            bx_diag(&diag, "replacement requires named file output");
            goto done;
        }
        if (!to_stdout && !from_stdin && !preflight_transform_name(&paths[i], suffix, codec, compress, &diag))
            goto done;
    }
    bool output_error = false;
    if (count == 0) {
        status = compress ? encode("-", -1, STDOUT_FILENO, codec, &encoding, &diag, &output_error) :
                          decode("-", -1, STDOUT_FILENO, "standard output", codec, &diag, &output_error);
        goto done;
    }
    status = 0;
    for (size_t i = 0; i < count && !output_error; i++) {
        int result;
        if (to_stdout || paths[i].parts.basename == NULL)
            result = compress ?
                encode(paths[i].name, -1, STDOUT_FILENO, codec, &encoding, &diag, &output_error) :
                decode(paths[i].name, -1, STDOUT_FILENO, "standard output", codec, &diag, &output_error);
        else
            result = transform_to_new_file(&paths[i], suffix, codec, compress ? &encoding : NULL, replace, keep, &diag);
        if (status < result)
            status = result;
    }
done:
    free(paths);
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
