#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "applets.h"
#include "diag.h"

struct bx_shred_options {
    const char* progname;
    bool force;
    unsigned int iterations;
    bool size_specified;
    uintmax_t size_bytes;
    bool remove_file;
    bool verbose;
    bool zero_final;
    bool show_help;
    bool show_version;
};

static const char* bx_shred_progname(const char* argv0) {
    if (argv0 == NULL || argv0[0] == '\0') {
        return "shred";
    }

    const char* base = strrchr(argv0, '/');
    if (base != NULL && base[1] != '\0') {
        return base + 1;
    }

    return argv0;
}

static void bx_shred_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... FILE...\n", progname);
    fprintf(stream, "Overwrite the specified FILE(s), then optionally remove them.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -f, --force           change permissions to allow writing if needed\n");
    fprintf(stream, "  -n, --iterations=N    overwrite N times instead of the default (3)\n");
    fprintf(stream, "  -s, --size=N          shred this many bytes (supports K/M/... suffixes)\n");
    fprintf(stream, "  -u, --remove          remove each file after overwriting\n");
    fprintf(stream, "  -v, --verbose         explain what is being done\n");
    fprintf(stream, "  -x, --exact           accepted for GNU compatibility\n");
    fprintf(stream, "  -z, --zero            add a final overwrite with zeros\n");
    fprintf(stream, "      --help            display this help and exit\n");
    fprintf(stream, "      --version         output version information and exit\n");
}

static void bx_shred_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

static bool bx_shred_parse_size_suffix(const char* suffix, uintmax_t* multiplier_out) {
    if (suffix == NULL || multiplier_out == NULL) {
        return false;
    }

    if (suffix[0] == '\0') {
        *multiplier_out = 1;
        return true;
    }

    static const char scale_letters[] = "KMGTPEZYRQ";
    char normalized = (char)toupper((unsigned char)suffix[0]);
    const char* letter_pos = strchr(scale_letters, normalized);
    if (letter_pos == NULL) {
        return false;
    }

    size_t suffix_len = strlen(suffix);
    uintmax_t base = 0;
    if (suffix_len == 1) {
        base = 1024;
    }
    else if (suffix_len == 2 && (suffix[1] == 'B' || suffix[1] == 'b')) {
        base = 1000;
    }
    else if (suffix_len == 3 && (suffix[1] == 'i' || suffix[1] == 'I') && (suffix[2] == 'B' || suffix[2] == 'b')) {
        base = 1024;
    }
    else {
        return false;
    }

    unsigned int power = (unsigned int)(letter_pos - scale_letters) + 1u;
    uintmax_t multiplier = 1;
    for (unsigned int i = 0; i < power; i++) {
        if (multiplier > UINTMAX_MAX / base) {
            return false;
        }
        multiplier *= base;
    }

    *multiplier_out = multiplier;
    return true;
}

static bool bx_shred_parse_size(const char* text, uintmax_t* size_out) {
    if (text == NULL || text[0] == '\0' || size_out == NULL) {
        return false;
    }

    errno = 0;
    char* end = NULL;
    uintmax_t value = strtoumax(text, &end, 10);
    if (errno == ERANGE || end == text || end == NULL) {
        return false;
    }

    uintmax_t multiplier = 1;
    if (!bx_shred_parse_size_suffix(end, &multiplier)) {
        return false;
    }

    if (value > UINTMAX_MAX / multiplier) {
        return false;
    }

    *size_out = value * multiplier;
    return true;
}

static bool bx_shred_parse_iterations(const char* text, unsigned int* iterations_out) {
    if (text == NULL || text[0] == '\0' || iterations_out == NULL) {
        return false;
    }

    errno = 0;
    char* end = NULL;
    uintmax_t value = strtoumax(text, &end, 10);
    if (errno == ERANGE || end == text || end == NULL || end[0] != '\0' || value > UINT_MAX) {
        return false;
    }

    *iterations_out = (unsigned int)value;
    return true;
}

static bool bx_shred_parse_options(int argc, char** argv, struct bx_shred_options* options, int* first_operand, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"force", no_argument, NULL, 'f'},      {"iterations", required_argument, NULL, 'n'},
        {"size", required_argument, NULL, 's'}, {"remove", no_argument, NULL, 'u'},
        {"verbose", no_argument, NULL, 'v'},    {"exact", no_argument, NULL, 'x'},
        {"zero", no_argument, NULL, 'z'},       {"help", no_argument, NULL, 1},
        {"version", no_argument, NULL, 2},      {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_shred_progname((argc > 0) ? argv[0] : NULL);
    options->iterations = 3u;
    diag->progname = options->progname;

    opterr = 0;
    optind = 1;

    while (true) {
        int option_index = 0;
        int c = getopt_long(argc, argv, "+:fn:s:uvxz", long_options, &option_index);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 'f':
                options->force = true;
                break;
            case 'n':
                if (!bx_shred_parse_iterations(optarg, &options->iterations)) {
                    bx_diag(diag, "invalid number of iterations '%s'", (optarg != NULL) ? optarg : "");
                    return false;
                }
                break;
            case 's':
                if (!bx_shred_parse_size(optarg, &options->size_bytes)) {
                    bx_diag(diag, "invalid file size '%s'", (optarg != NULL) ? optarg : "");
                    return false;
                }
                options->size_specified = true;
                break;
            case 'u':
                options->remove_file = true;
                break;
            case 'v':
                options->verbose = true;
                break;
            case 'x':
                break;
            case 'z':
                options->zero_final = true;
                break;
            case 1:
                options->show_help = true;
                break;
            case 2:
                options->show_version = true;
                break;
            case ':':
                if (optopt != 0) {
                    bx_diag(diag, "option requires an argument -- '%c'", optopt);
                }
                else if (optind > 0 && optind <= argc && argv[optind - 1] != NULL) {
                    bx_diag(diag, "option requires an argument -- '%s'", argv[optind - 1]);
                }
                else {
                    bx_diag(diag, "option requires an argument");
                }
                return false;
            case '?':
                if (optopt != 0) {
                    bx_diag(diag, "invalid option -- '%c'", optopt);
                }
                else if (optind > 0 && optind <= argc && argv[optind - 1] != NULL) {
                    bx_diag(diag, "unrecognized option '%s'", argv[optind - 1]);
                }
                else {
                    bx_diag(diag, "unrecognized option");
                }
                return false;
            default:
                return false;
        }
    }

    *first_operand = optind;
    return true;
}

static bool bx_shred_fill_random(int random_fd, unsigned char* buffer, size_t count, struct bx_diag_ctx* diag) {
    size_t done = 0;
    while (done < count) {
        ssize_t nread = read(random_fd, buffer + done, count - done);
        if (nread < 0) {
            if (errno == EINTR) {
                continue;
            }
            bx_perror_path(diag, "/dev/urandom");
            return false;
        }
        if (nread == 0) {
            bx_diag(diag, "/dev/urandom: unexpected end of file");
            return false;
        }
        done += (size_t)nread;
    }
    return true;
}

static bool bx_shred_write_all(int fd, const unsigned char* buffer, size_t count, const char* path, struct bx_diag_ctx* diag) {
    size_t done = 0;
    while (done < count) {
        ssize_t nwritten = write(fd, buffer + done, count - done);
        if (nwritten < 0) {
            if (errno == EINTR) {
                continue;
            }
            bx_perror_path(diag, path);
            return false;
        }
        if (nwritten == 0) {
            bx_diag(diag, "%s: short write", path);
            return false;
        }
        done += (size_t)nwritten;
    }
    return true;
}

static bool bx_shred_overwrite_pass(int fd, const char* path, uintmax_t bytes, int random_fd, bool zero_fill, struct bx_diag_ctx* diag) {
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
        else if (!bx_shred_fill_random(random_fd, buffer, chunk, diag)) {
            return false;
        }

        if (!bx_shred_write_all(fd, buffer, chunk, path, diag)) {
            return false;
        }
        remaining -= (uintmax_t)chunk;
    }

    if (fsync(fd) != 0) {
        bx_perror_path(diag, path);
        return false;
    }

    return true;
}

static int bx_shred_open_target(const char* path, bool force, struct bx_diag_ctx* diag) {
    int fd = open(path, O_WRONLY);
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

    fd = open(path, O_WRONLY);
    if (fd < 0) {
        bx_perror_path(diag, path);
    }
    return fd;
}

static bool bx_shred_apply_path(const char* path, const struct bx_shred_options* options, int random_fd, struct bx_diag_ctx* diag) {
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
    }

    unsigned int total_passes = options->iterations + (options->zero_final ? 1u : 0u);

    if (ok) {
        for (unsigned int i = 0; i < options->iterations; i++) {
            bx_info(diag, "%s: pass %u/%u (random)", path, i + 1u, total_passes);
            if (!bx_shred_overwrite_pass(fd, path, bytes_to_overwrite, random_fd, false, diag)) {
                ok = false;
                break;
            }
        }
    }

    if (ok && options->zero_final) {
        bx_info(diag, "%s: pass %u/%u (zero)", path, options->iterations + 1u, total_passes);
        if (!bx_shred_overwrite_pass(fd, path, bytes_to_overwrite, random_fd, true, diag)) {
            ok = false;
        }
    }

    if (close(fd) != 0) {
        bx_perror_path(diag, path);
        ok = false;
    }

    if (ok && options->remove_file) {
        bx_info(diag, "%s: removing", path);
        if (unlink(path) != 0) {
            bx_perror_path(diag, path);
            ok = false;
        }
    }

    return ok;
}

int bx_shred_main(int argc, char** argv) {
    struct bx_shred_options options;
    struct bx_diag_ctx diag = {
        .progname = "shred",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };
    int first_operand = 0;

    if (!bx_shred_parse_options(argc, argv, &options, &first_operand, &diag)) {
        return diag.exit_status != 0 ? diag.exit_status : 1;
    }

    diag.verbose = options.verbose;

    if (options.show_help) {
        bx_shred_print_help(stdout, options.progname);
        return 0;
    }

    if (options.show_version) {
        bx_shred_print_version(options.progname);
        return 0;
    }

    int operand_count = argc - first_operand;
    if (operand_count <= 0) {
        bx_diag(&diag, "missing operand");
        return diag.exit_status;
    }

    int random_fd = -1;
    if (options.iterations > 0) {
        random_fd = open("/dev/urandom", O_RDONLY);
        if (random_fd < 0) {
            bx_perror_path(&diag, "/dev/urandom");
            return diag.exit_status;
        }
    }

    for (int i = first_operand; i < argc; i++) {
        (void)bx_shred_apply_path(argv[i], &options, random_fd, &diag);
    }

    if (random_fd >= 0 && close(random_fd) != 0) {
        bx_perror_path(&diag, "/dev/urandom");
    }

    return diag.exit_status;
}
