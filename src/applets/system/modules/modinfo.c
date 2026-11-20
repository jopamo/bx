#define _GNU_SOURCE
#include <stdio.h>

#include "applets.h"
#include "bx/diag.h"
#include "lib/cli_common.h"

static void usage(FILE* out, const char* name) {
    fprintf(out, "Usage: %s [-F FIELD] [--] FILE...\n"
                 "Print metadata from Linux module files (plain, gzip, xz, or zstd).\n", name);
}

#ifdef __linux__
#include <getopt.h>
#include <stdlib.h>
#include <string.h>

#include "applets/archive/archive_codec.h"
#include "lib/fd_ops.h"
#include "lib/xreadwrite.h"
#include "module_elf.h"

enum { MODULE_SIZE_LIMIT = 128 * 1024 * 1024 };

static unsigned char* read_module(const char* path, size_t* size) {
    int fd = bx_fd_open_cloexec(path, O_RDONLY | O_NONBLOCK, 0);
    if (fd < 0)
        return NULL;
    unsigned char* file = NULL;
    struct stat st;
    if (fstat(fd, &st) < 0)
        goto fail;
    if (!S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > MODULE_SIZE_LIMIT) {
        errno = st.st_size > MODULE_SIZE_LIMIT ? EFBIG : EINVAL;
        goto fail;
    }
    *size = (size_t)st.st_size;
    file = malloc(*size);
    if (file == NULL)
        goto fail;
    for (size_t pos = 0; pos < *size;) {
        ssize_t n = bx_xread(fd, file + pos, *size - pos);
        if (n <= 0) {
            if (n == 0)
                errno = EIO;
            goto fail;
        }
        pos += (size_t)n;
    }
    bx_fd_cleanup(&fd);
    return file;
fail: {
        int error = errno;
        free(file);
        bx_fd_cleanup(&fd);
        errno = error;
        return NULL;
    }
}

static bool print_info(const char* path, const char* data, size_t len, const char* field) {
    if (field != NULL && strcmp(field, "filename") == 0)
        return puts(path) != EOF;
    if (field == NULL && printf("filename:       %s\n", path) < 0)
        return false;
    size_t field_len = field ? strlen(field) : 0;
    for (const char* p = data; p < data + len; p += strlen(p) + 1) {
        if (*p == '\0')
            continue;
        const char* eq = strchr(p, '=');
        size_t key_len = (size_t)(eq - p);
        if (field == NULL) {
            if (printf("%.*s:%*s%s\n", (int)key_len, p,
                       key_len < 15 ? (int)(15 - key_len) : 1, "", eq + 1) < 0)
                return false;
        } else if (field_len == key_len && memcmp(field, p, key_len) == 0 && puts(eq + 1) == EOF) {
            return false;
        }
    }
    return true;
}
#endif

int bx_modinfo_main(int argc, char** argv) {
    int handled = bx_cli_maybe_handle_help_or_version(argc, argv, "modinfo", "-h", "-V", usage);
    if (handled >= 0)
        return handled;
    struct bx_diag_ctx diag = {.progname = "modinfo"};
#ifdef __linux__
    static const struct option opts[] = {
        {"field", required_argument, NULL, 'F'}, {NULL, 0, NULL, 0},
    };
    const char* field = NULL;
    opterr = 0;
    optind = 1;
    for (int opt; (opt = getopt_long(argc, argv, ":F:", opts, NULL)) != -1;) {
        if (opt == 'F') {
            if (*optarg == '\0') {
                bx_diag(&diag, "field must not be empty");
                return 1;
            }
            field = optarg;
        } else {
            if (opt == ':')
                bx_cli_diag_option_requires_arg(&diag, optopt, optind, argc, argv);
            else
                bx_cli_diag_unrecognized_option(&diag, optopt, optind, argc, argv);
            return 1;
        }
    }
    if (optind == argc) {
        usage(stderr, diag.progname);
        return 1;
    }
    int status = 0;
    for (int i = optind; i < argc; i++) {
        size_t size, len;
        const char* data;
        unsigned char* file = read_module(argv[i], &size);
        if (file == NULL) {
            bx_perror_path(&diag, argv[i]);
            status = 1;
            continue;
        }
        const struct bx_archive_codec* codec = bx_archive_codec_detect_magic(file, size);
        struct bx_archive_buffer decoded = {0};
        bool ready = true;
        if (codec == bx_archive_codec_bzip2()) {
            bx_diag(&diag, "'%s': unsupported module compression", argv[i]);
            ready = false;
        } else if (codec != NULL) {
            struct bx_archive_buffer input = {.data = file, .len = size};
            ready = bx_archive_codec_decode_buffer_limited(codec, &input, &decoded,
                                                          MODULE_SIZE_LIMIT, &diag);
            free(file);
            file = decoded.data;
            size = decoded.len;
        }
        if (!ready) {
            status = 1;
        } else if (!bx_module_elf_info(file, size, &data, &len)) {
            bx_diag(&diag, "'%s': expected an ELF module with valid .modinfo", argv[i]);
            status = 1;
        } else if (!print_info(argv[i], data, len, field) || fflush(stdout) == EOF) {
            bx_perror_path(&diag, "standard output");
            free(file);
            return 1;
        }
        free(file);
    }
    return status;
#else
    bx_diag(&diag, "Linux module metadata is not supported on this platform");
    return 1;
#endif
}
