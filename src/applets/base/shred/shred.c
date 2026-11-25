#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"
#include "lib/cli_common.h"
#include "lib/fd_ops.h"
#include "lib/option_scan.h"
#include "lib/size_parse.h"
#include "lib/xreadwrite.h"

enum bx_shred_remove_mode {
    BX_SHRED_REMOVE_UNLINK = 0,
    BX_SHRED_REMOVE_WIPE,
    BX_SHRED_REMOVE_WIPESYNC,
};

enum {
    BX_SHRED_OPT_HELP = 256,
    BX_SHRED_OPT_VERSION,
    BX_SHRED_OPT_REMOVE,
    BX_SHRED_OPT_RANDOM_SOURCE,
};

struct bx_shred_options {
    const char* progname;
    bool force;
    unsigned int iterations;
    const char* random_source_path;
    bool random_source_specified;
    bool exact;
    bool size_specified;
    uintmax_t size_bytes;
    bool remove_file;
    enum bx_shred_remove_mode remove_mode;
    bool verbose;
    bool zero_final;
    bool show_help;
    bool show_version;
    const char** paths;
    size_t path_count;
};

static void bx_shred_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... FILE...\n", progname);
    fprintf(stream, "Overwrite the specified FILE(s), then optionally remove them.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -f, --force           change permissions to allow writing if needed\n");
    fprintf(stream, "  -n, --iterations=N    overwrite N times instead of the default (3)\n");
    fprintf(stream, "      --random-source=FILE  get random bytes from FILE\n");
    fprintf(stream, "  -s, --size=N          shred this many bytes (supports K/M/... suffixes)\n");
    fprintf(stream, "  -u                    remove each file after overwriting\n");
    fprintf(stream, "      --remove[=HOW]    remove using HOW: unlink, wipe, or wipesync\n");
    fprintf(stream, "  -v, --verbose         explain what is being done\n");
    fprintf(stream, "  -x, --exact           do not round file sizes up to the next full block\n");
    fprintf(stream, "  -z, --zero            add a final overwrite with zeros\n");
    fprintf(stream, "      --help            display this help and exit\n");
    fprintf(stream, "      --version         output version information and exit\n");
}

static bool bx_shred_parse_size(const char* text, uintmax_t* size_out) {
    return bx_size_parse_block_size(text, size_out);
}

static bool bx_shred_parse_iterations(const char* text, unsigned int* iterations_out) {
    if (text == NULL || text[0] == '\0' || iterations_out == NULL) {
        return false;
    }

    const char* digits = text;
    while (isspace((unsigned char)*digits)) {
        digits++;
    }
    if (digits[0] == '+') {
        digits++;
    }
    else if (digits[0] == '-') {
        return false;
    }

    uintmax_t value = 0;
    if (!bx_size_parse_uint(digits, &value) || value > UINT_MAX) {
        return false;
    }

    *iterations_out = (unsigned int)value;
    return true;
}

static bool bx_shred_round_up_to_block(uintmax_t size_bytes, uintmax_t block_size, uintmax_t* rounded_size_out) {
    if (rounded_size_out == NULL) {
        return false;
    }

    if (block_size <= 1 || size_bytes == 0) {
        *rounded_size_out = size_bytes;
        return true;
    }

    uintmax_t remainder = size_bytes % block_size;
    if (remainder == 0) {
        *rounded_size_out = size_bytes;
        return true;
    }

    uintmax_t extra = block_size - remainder;
    if (size_bytes > UINTMAX_MAX - extra) {
        return false;
    }

    *rounded_size_out = size_bytes + extra;
    return true;
}

static bool bx_shred_parse_remove_mode(const char* text, enum bx_shred_remove_mode* remove_mode_out) {
    if (text == NULL || remove_mode_out == NULL) {
        return false;
    }

    if (strcmp(text, "unlink") == 0) {
        *remove_mode_out = BX_SHRED_REMOVE_UNLINK;
        return true;
    }
    if (strcmp(text, "wipe") == 0) {
        *remove_mode_out = BX_SHRED_REMOVE_WIPE;
        return true;
    }
    if (strcmp(text, "wipesync") == 0) {
        *remove_mode_out = BX_SHRED_REMOVE_WIPESYNC;
        return true;
    }
    return false;
}

static const char* bx_shred_remove_mode_name(enum bx_shred_remove_mode remove_mode) {
    switch (remove_mode) {
        case BX_SHRED_REMOVE_UNLINK:
            return "unlink";
        case BX_SHRED_REMOVE_WIPE:
            return "wipe";
        case BX_SHRED_REMOVE_WIPESYNC:
            return "wipesync";
    }

    return "unlink";
}

static bool bx_shred_parse_options(int argc, char** argv, struct bx_shred_options* options, struct bx_diag_ctx* diag) {
    static const struct bx_option_spec specs[] = {
        {'f', 'f', "force", BX_OPTION_NO_VALUE},
        {'n', 'n', "iterations", BX_OPTION_REQUIRED_VALUE},
        {BX_SHRED_OPT_REMOVE, 0, "remove", BX_OPTION_OPTIONAL_VALUE},
        {BX_SHRED_OPT_RANDOM_SOURCE, 0, "random-source", BX_OPTION_REQUIRED_VALUE},
        {'s', 's', "size", BX_OPTION_REQUIRED_VALUE},
        {'v', 'v', "verbose", BX_OPTION_NO_VALUE},
        {'x', 'x', "exact", BX_OPTION_NO_VALUE},
        {'z', 'z', "zero", BX_OPTION_NO_VALUE},
        {BX_SHRED_OPT_HELP, 0, "help", BX_OPTION_NO_VALUE},
        {BX_SHRED_OPT_VERSION, 0, "version", BX_OPTION_NO_VALUE},
        {'u', 'u', NULL, BX_OPTION_NO_VALUE},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "shred");
    options->iterations = 3u;
    options->random_source_path = "/dev/urandom";
    options->remove_mode = BX_SHRED_REMOVE_WIPESYNC;
    diag->progname = options->progname;

    struct bx_option_cursor cursor;
    size_t end = argc > 0 ? (size_t)argc : 0;
    bx_option_cursor_init(&cursor, argv, end != 0, end, BX_OPTION_ABBREVIATE,
                          specs, sizeof(specs) / sizeof(specs[0]));
    for (;;) {
        struct bx_option_result result = bx_option_next(&cursor);
        if (result.event == BX_OPTION_END)
            return true;
        if (result.event == BX_OPTION_OPERAND) {
            if (options->paths == NULL) {
                size_t capacity = end - 1;
                if (capacity > SIZE_MAX / sizeof(*options->paths) ||
                    (options->paths = malloc(capacity * sizeof(*options->paths))) == NULL) {
                    bx_diag(diag, "memory exhausted");
                    return false;
                }
            }
            options->paths[options->path_count++] = result.token;
            continue;
        }
        if (result.event != BX_OPTION_MATCH) {
            if (result.event == BX_OPTION_MISSING_VALUE) {
                if (result.short_name != 0)
                    bx_diag(diag, "option requires an argument -- '%c'", result.short_name);
                else
                    bx_diag(diag, "option requires an argument -- '%s'", result.token);
            }
            else if (result.short_name != 0)
                bx_diag(diag, "invalid option -- '%c'", result.short_name);
            else
                bx_diag(diag, "unrecognized option '%s'", result.token);
            return false;
        }

        const char* value = result.value;
        switch (result.spec->id) {
            case 'f':
                options->force = true;
                break;
            case 'n':
                if (!bx_shred_parse_iterations(value, &options->iterations)) {
                    bx_diag(diag, "invalid number of iterations '%s'", value);
                    return false;
                }
                break;
            case 's':
                if (!bx_shred_parse_size(value, &options->size_bytes)) {
                    bx_diag(diag, "invalid file size '%s'", value);
                    return false;
                }
                options->size_specified = true;
                break;
            case 'u':
                options->remove_file = true;
                options->remove_mode = BX_SHRED_REMOVE_WIPESYNC;
                break;
            case 'v':
                options->verbose = true;
                break;
            case 'x':
                options->exact = true;
                break;
            case 'z':
                options->zero_final = true;
                break;
            case BX_SHRED_OPT_REMOVE:
                options->remove_file = true;
                if (value == NULL) {
                    options->remove_mode = BX_SHRED_REMOVE_WIPESYNC;
                    break;
                }
                if (!bx_shred_parse_remove_mode(value, &options->remove_mode)) {
                    bx_diag(diag, "invalid argument '%s' for '--remove'", value);
                    return false;
                }
                break;
            case BX_SHRED_OPT_RANDOM_SOURCE:
                options->random_source_path = value;
                options->random_source_specified = true;
                break;
            case BX_SHRED_OPT_HELP:
                options->show_help = true;
                break;
            case BX_SHRED_OPT_VERSION:
                options->show_version = true;
                break;
            default:
                return false;
        }
    }
}

static bool bx_shred_fill_random(int random_fd, const char* random_source_path, unsigned char* buffer, size_t count, struct bx_diag_ctx* diag) {
    size_t done = 0;
    while (done < count) {
        ssize_t nread = bx_xread(random_fd, buffer + done, count - done);
        if (nread < 0) {
            bx_perror_path(diag, random_source_path);
            return false;
        }
        if (nread == 0) {
            bx_diag(diag, "%s: unexpected end of file", random_source_path);
            return false;
        }
        done += (size_t)nread;
    }
    return true;
}

static bool bx_shred_write_all(int fd, const unsigned char* buffer, size_t count, const char* path, struct bx_diag_ctx* diag) {
    enum bx_xwrite_status status = bx_xwrite_all_status(fd, buffer, count);
    if (status == BX_XWRITE_OK) {
        return true;
    }
    if (status == BX_XWRITE_ZERO) {
        bx_diag(diag, "%s: short write", path);
        return false;
    }
    bx_perror_path(diag, path);
    return false;
}

static bool bx_shred_overwrite_pass(int fd, const char* path, uintmax_t bytes, int random_fd, const char* random_source_path, bool zero_fill, struct bx_diag_ctx* diag) {
    if (lseek(fd, 0, SEEK_SET) < 0) {
        bx_perror_path(diag, path);
        return false;
    }

    unsigned char buffer[65536];
    uintmax_t remaining = bytes;
    while (remaining > 0) {
        size_t chunk = (remaining > (uintmax_t)sizeof(buffer)) ? sizeof(buffer) : (size_t)remaining;
        if (zero_fill) {
            memset(buffer, 0, chunk);
        }
        else if (!bx_shred_fill_random(random_fd, random_source_path, buffer, chunk, diag)) {
            return false;
        }

        if (!bx_shred_write_all(fd, buffer, chunk, path, diag)) {
            return false;
        }
        remaining -= (uintmax_t)chunk;
    }

    if (bx_fd_fsync(fd) != 0) {
        bx_perror_path(diag, path);
        return false;
    }

    return true;
}

static int bx_shred_open_target(const char* path, bool force, struct bx_diag_ctx* diag) {
    int fd = bx_fd_open_cloexec(path, O_WRONLY, 0);
    if (fd >= 0) {
        return fd;
    }

    if (!force || errno != EACCES) {
        bx_perror_path(diag, path);
        return -1;
    }

    struct stat st;
    if (stat(path, &st) != 0) {
        bx_perror_path(diag, path);
        return -1;
    }

    if (chmod(path, st.st_mode | S_IWUSR) != 0) {
        bx_perror_path(diag, path);
        return -1;
    }

    fd = bx_fd_open_cloexec(path, O_WRONLY, 0);
    if (fd < 0) {
        bx_perror_path(diag, path);
    }
    return fd;
}

static bool bx_shred_remove_path(const char* path, enum bx_shred_remove_mode remove_mode, struct bx_diag_ctx* diag) {
    switch (remove_mode) {
        case BX_SHRED_REMOVE_UNLINK:
        case BX_SHRED_REMOVE_WIPE:
        case BX_SHRED_REMOVE_WIPESYNC:
            break;
    }

    if (bx_fd_unlinkat(AT_FDCWD, path, 0) != 0) {
        bx_perror_path(diag, path);
        return false;
    }

    return true;
}

static bool bx_shred_apply_path(const char* path, const struct bx_shred_options* options, int random_fd, const char* random_source_path, struct bx_diag_ctx* diag) {
    int fd = bx_shred_open_target(path, options->force, diag);
    if (fd < 0) {
        return false;
    }

    bool ok = true;
    uintmax_t bytes_to_overwrite = 0;
    struct stat st;
    if (fstat(fd, &st) != 0) {
        bx_perror_path(diag, path);
        ok = false;
    }
    else if (!S_ISREG(st.st_mode)) {
        bx_diag(diag, "%s: not a regular file", path);
        ok = false;
    }
    else if (options->size_specified) {
        bytes_to_overwrite = options->size_bytes;
    }
    else if (st.st_size < 0) {
        bx_diag(diag, "%s: invalid file size", path);
        ok = false;
    }
    else {
        bytes_to_overwrite = (uintmax_t)st.st_size;
        if (!options->exact && st.st_blksize > 1) {
            uintmax_t rounded_bytes = 0;
            if (!bx_shred_round_up_to_block(bytes_to_overwrite, (uintmax_t)st.st_blksize, &rounded_bytes)) {
                bx_diag(diag, "%s: file size too large to round to block boundary", path);
                ok = false;
            }
            else {
                bytes_to_overwrite = rounded_bytes;
            }
        }
    }

    unsigned int total_passes = options->iterations + (options->zero_final ? 1u : 0u);

    if (ok) {
        for (unsigned int i = 0; i < options->iterations; i++) {
            bx_info(diag, "%s: pass %u/%u (random)", path, i + 1u, total_passes);
            if (!bx_shred_overwrite_pass(fd, path, bytes_to_overwrite, random_fd, random_source_path, false, diag)) {
                ok = false;
                break;
            }
        }
    }

    if (ok && options->zero_final) {
        bx_info(diag, "%s: pass %u/%u (zero)", path, options->iterations + 1u, total_passes);
        if (!bx_shred_overwrite_pass(fd, path, bytes_to_overwrite, random_fd, random_source_path, true, diag)) {
            ok = false;
        }
    }

    if (!bx_fd_close(&fd, path, diag)) {
        ok = false;
    }

    if (ok && options->remove_file) {
        bx_info(diag, "%s: removing (%s)", path, bx_shred_remove_mode_name(options->remove_mode));
        if (!bx_shred_remove_path(path, options->remove_mode, diag)) {
            ok = false;
        }
    }

    return ok;
}

static int bx_shred_run(const struct bx_shred_options* options, struct bx_diag_ctx* diag) {
    diag->verbose = options->verbose;

    if (options->show_help) {
        bx_shred_print_help(stdout, options->progname);
        return 0;
    }

    if (options->show_version) {
        bx_cli_print_version(options->progname);
        return 0;
    }

    if (options->path_count == 0) {
        bx_diag(diag, "missing operand");
        return diag->exit_status;
    }

    int random_fd = -1;
    if (options->iterations > 0 || options->random_source_specified) {
        random_fd = bx_fd_open_cloexec(options->random_source_path, O_RDONLY, 0);
        if (random_fd < 0) {
            bx_perror_path(diag, options->random_source_path);
            return diag->exit_status;
        }
    }

    for (size_t i = 0; i < options->path_count; i++) {
        (void)bx_shred_apply_path(options->paths[i], options, random_fd, options->random_source_path, diag);
    }

    (void)bx_fd_close(&random_fd, options->random_source_path, diag);

    return diag->exit_status;
}

int bx_shred_main(int argc, char** argv) {
    struct bx_shred_options options;
    struct bx_diag_ctx diag = {0};
    int status;
    if (bx_shred_parse_options(argc, argv, &options, &diag))
        status = bx_shred_run(&options, &diag);
    else
        status = diag.exit_status != 0 ? diag.exit_status : 1;
    free(options.paths);
    return status;
}
