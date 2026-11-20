#define _GNU_SOURCE

#include <stdio.h>

#include "applets.h"
#include "bx/diag.h"
#include "lib/cli_common.h"

static void usage(FILE* out, const char* name) {
    fprintf(out, "Usage: %s --file --size SIZE [-L LABEL] [-U UUID] [--] NEWFILE\n"
                 "Create a new, allocated Linux swap file. Never overwrite an existing target.\n",
            name);
}

#ifdef __linux__
#include <getopt.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "lib/fd_ops.h"
#include "lib/path_ops.h"
#include "lib/random_bytes.h"
#include "lib/size_parse.h"
#include "lib/xreadwrite.h"

enum {
    SWAP_VERSION_OFFSET = 1024,
    SWAP_UUID_OFFSET = 1036,
    SWAP_LABEL_OFFSET = 1052,
    SWAP_LABEL_BYTES = 16,
    SWAP_MIN_PAGES = 10,
};

static int hex_digit(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static bool parse_uuid(const char* text, unsigned char* uuid) {
    if (strlen(text) != 36)
        return false;
    for (size_t i = 0; i < 16; i++) {
        if ((i == 4 || i == 6 || i == 8 || i == 10) && *text++ != '-')
            return false;
        int hi = hex_digit(*text++);
        int lo = hex_digit(*text++);
        if (hi < 0 || lo < 0)
            return false;
        uuid[i] = (unsigned char)((hi << 4) | lo);
    }
    return true;
}

static bool create_swap(const char* target, const unsigned char* header, size_t page, off_t size,
                        struct bx_diag_ctx* diag) {
    struct bx_path_split parts = bx_path_split(target, false);
    if (parts.basename_length == 0 || strcmp(parts.basename, ".") == 0 ||
        strcmp(parts.basename, "..") == 0) {
        bx_diag(diag, "invalid swap file path: '%s'", target);
        return false;
    }
    char* parent = parts.parent_length ? strndup(target, parts.parent_length) : NULL;
    if (parts.parent_length && parent == NULL) {
        bx_perror_path(diag, target);
        return false;
    }
    int dir = bx_fd_open_cloexec(parent ? parent : ".", O_RDONLY | O_DIRECTORY, 0);
    int error = errno;
    free(parent);
    errno = error;

    int fd = -1;
    bool ok = false;
    if (dir < 0)
        goto fail;
    /* Avoid allocating space unnecessarily if the name already exists. */
    struct stat existing;
    if (fstatat(dir, parts.basename, &existing, AT_SYMLINK_NOFOLLOW) == 0) {
        errno = EEXIST;
        goto fail;
    }
    if (errno != ENOENT)
        goto fail;
    fd = bx_fd_open_anonymous_file_at(dir);
    if (fd < 0 || fallocate(fd, 0, 0, size) < 0)
        goto fail;
    enum bx_xwrite_status written = bx_xwrite_all_status(fd, header, page);
    if (written != BX_XWRITE_OK) {
        if (written == BX_XWRITE_ZERO)
            errno = EIO;
        goto fail;
    }
    /* linkat is the commit point. It cannot replace an existing name. */
    if (fsync(fd) < 0 || linkat(fd, "", dir, parts.basename, AT_EMPTY_PATH) < 0)
        goto fail;
    ok = true;
    goto done;
fail:
    bx_perror_path(diag, target);
done:
    bx_fd_cleanup(&fd);
    bx_fd_cleanup(&dir);
    return ok;
}
#endif

int bx_mkswap_main(int argc, char** argv) {
    int handled = bx_cli_maybe_handle_help_or_version(argc, argv, "mkswap", "-h", "-V", usage);
    if (handled >= 0)
        return handled;
    struct bx_diag_ctx diag = {.progname = "mkswap"};
#ifdef __linux__
    static const struct option opts[] = {
        {"file", no_argument, NULL, 'F'}, {"size", required_argument, NULL, 's'},
        {"label", required_argument, NULL, 'L'}, {"uuid", required_argument, NULL, 'U'},
        {NULL, 0, NULL, 0},
    };
    const char* label = "", *uuid_text = NULL;
    uintmax_t bytes = 0;
    bool create = false;
    optind = 1;
    opterr = 0;
    for (int opt; (opt = getopt_long(argc, argv, "+:Fs:L:U:", opts, NULL)) != -1;) {
        switch (opt) {
        case 'F': create = true; break;
        case 's':
            if (!bx_size_parse_scaled_uint(optarg, &bytes) || bytes > INT64_MAX) {
                bx_diag(&diag, "invalid swap size: '%s'", optarg);
                return 1;
            }
            break;
        case 'L': label = optarg; break;
        case 'U': uuid_text = optarg; break;
        case ':':
            bx_cli_diag_option_requires_arg(&diag, optopt, optind, argc, argv);
            return 1;
        default:
            bx_cli_diag_unrecognized_option(&diag, optopt, optind, argc, argv);
            return 1;
        }
    }
    if (!create || argc - optind != 1) {
        usage(stderr, diag.progname);
        return 1;
    }
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size < 2048 || page_size > 65536) {
        bx_diag(&diag, "unsupported Linux page size");
        return 1;
    }
    size_t page = (size_t)page_size;
    uintmax_t pages = bytes / page;
    if (pages < SWAP_MIN_PAGES || pages > UINT32_MAX || bytes % page != 0) {
        bx_diag(&diag, "size must contain 10 to %" PRIu32 " whole pages of %zu bytes", UINT32_MAX, page);
        return 1;
    }
    size_t label_len = strlen(label);
    if (label_len > SWAP_LABEL_BYTES) {
        bx_diag(&diag, "swap label exceeds %d bytes", SWAP_LABEL_BYTES);
        return 1;
    }
    unsigned char uuid[16];
    if (uuid_text != NULL) {
        if (!parse_uuid(uuid_text, uuid)) {
            bx_diag(&diag, "invalid swap UUID: '%s'", uuid_text);
            return 1;
        }
    } else {
        if (!bx_random_bytes(uuid, sizeof(uuid))) {
            bx_perror_path(&diag, "swap UUID");
            return 1;
        }
        uuid[6] = (unsigned char)((uuid[6] & 0x0f) | 0x40);
        uuid[8] = (unsigned char)((uuid[8] & 0x3f) | 0x80);
    }
    unsigned char* header = calloc(page, 1);
    if (header == NULL) {
        bx_perror_path(&diag, "swap header");
        return 1;
    }
    /* Linux swap v1 uses host byte order for its numeric fields. */
    uint32_t fields[] = {1, (uint32_t)(pages - 1), 0};
    memcpy(header + SWAP_VERSION_OFFSET, fields, sizeof(fields));
    memcpy(header + SWAP_UUID_OFFSET, uuid, sizeof(uuid));
    memcpy(header + SWAP_LABEL_OFFSET, label, label_len);
    memcpy(header + page - 10, "SWAPSPACE2", 10);
    bool ok = create_swap(argv[optind], header, page, (off_t)bytes, &diag);
    free(header);
    return ok ? 0 : 1;
#else
    bx_diag(&diag, "Linux swap formatting is not supported on this platform");
    return 1;
#endif
}
