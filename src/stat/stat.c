#include <errno.h>
#include <getopt.h>
#include <grp.h>
#include <inttypes.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#include "applets.h"
#include "diag.h"
#include "libbx.h"

struct bx_stat_options {
    const char* progname;
    bool dereference;
    bool show_help;
    bool show_version;
};

static const char* bx_stat_progname(const char* argv0) {
    if (argv0 == NULL || argv0[0] == '\0') {
        return "stat";
    }

    const char* base = strrchr(argv0, '/');
    if (base != NULL && base[1] != '\0') {
        return base + 1;
    }
    return argv0;
}

static void bx_stat_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... FILE...\n", progname);
    fprintf(stream, "Display file status.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -L, --dereference  follow links\n");
    fprintf(stream, "      --help         display this help and exit\n");
    fprintf(stream, "      --version      output version information and exit\n");
}

static void bx_stat_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

static bool bx_stat_parse_options(int argc, char** argv, struct bx_stat_options* options, int* first_operand, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"dereference", no_argument, NULL, 'L'},
        {"help", no_argument, NULL, 1},
        {"version", no_argument, NULL, 2},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_stat_progname((argc > 0) ? argv[0] : NULL);
    diag->progname = options->progname;

    opterr = 0;
    optind = 1;

    while (true) {
        int c = getopt_long(argc, argv, "+L", long_options, NULL);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 'L':
                options->dereference = true;
                break;
            case 1:
                options->show_help = true;
                return true;
            case 2:
                options->show_version = true;
                return true;
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

static char bx_stat_mode_type_char(mode_t mode) {
    if (S_ISREG(mode)) {
        return '-';
    }
    if (S_ISDIR(mode)) {
        return 'd';
    }
    if (S_ISLNK(mode)) {
        return 'l';
    }
    if (S_ISCHR(mode)) {
        return 'c';
    }
    if (S_ISBLK(mode)) {
        return 'b';
    }
    if (S_ISFIFO(mode)) {
        return 'p';
    }
#ifdef S_ISSOCK
    if (S_ISSOCK(mode)) {
        return 's';
    }
#endif
    return '?';
}

static const char* bx_stat_file_type_description(mode_t mode) {
    if (S_ISREG(mode)) {
        return "regular file";
    }
    if (S_ISDIR(mode)) {
        return "directory";
    }
    if (S_ISLNK(mode)) {
        return "symbolic link";
    }
    if (S_ISCHR(mode)) {
        return "character special file";
    }
    if (S_ISBLK(mode)) {
        return "block special file";
    }
    if (S_ISFIFO(mode)) {
        return "fifo";
    }
#ifdef S_ISSOCK
    if (S_ISSOCK(mode)) {
        return "socket";
    }
#endif
    return "unknown";
}

static void bx_stat_mode_to_string(mode_t mode, char out[11]) {
    out[0] = bx_stat_mode_type_char(mode);
    out[1] = (mode & S_IRUSR) ? 'r' : '-';
    out[2] = (mode & S_IWUSR) ? 'w' : '-';
    out[3] = (mode & S_IXUSR) ? 'x' : '-';
    out[4] = (mode & S_IRGRP) ? 'r' : '-';
    out[5] = (mode & S_IWGRP) ? 'w' : '-';
    out[6] = (mode & S_IXGRP) ? 'x' : '-';
    out[7] = (mode & S_IROTH) ? 'r' : '-';
    out[8] = (mode & S_IWOTH) ? 'w' : '-';
    out[9] = (mode & S_IXOTH) ? 'x' : '-';

    if (mode & S_ISUID) {
        out[3] = (mode & S_IXUSR) ? 's' : 'S';
    }
    if (mode & S_ISGID) {
        out[6] = (mode & S_IXGRP) ? 's' : 'S';
    }
#ifdef S_ISVTX
    if (mode & S_ISVTX) {
        out[9] = (mode & S_IXOTH) ? 't' : 'T';
    }
#endif

    out[10] = '\0';
}

static char* bx_stat_readlink_target(const char* path) {
    size_t capacity = 128;
    char* buffer = xmalloc(capacity + 1u);

    while (true) {
        ssize_t nread = readlink(path, buffer, capacity);
        if (nread < 0) {
            free(buffer);
            return NULL;
        }

        if ((size_t)nread < capacity) {
            buffer[nread] = '\0';
            return buffer;
        }

        if (capacity > (SIZE_MAX / 2u) - 1u) {
            free(buffer);
            errno = ENOMEM;
            return NULL;
        }
        capacity *= 2u;
        buffer = xrealloc(buffer, capacity + 1u);
    }
}

static const char* bx_stat_user_name(uid_t uid, char* numeric_buffer, size_t numeric_buffer_len) {
    struct passwd* pw = getpwuid(uid);
    if (pw != NULL && pw->pw_name != NULL && pw->pw_name[0] != '\0') {
        return pw->pw_name;
    }

    (void)snprintf(numeric_buffer, numeric_buffer_len, "%" PRIuMAX, (uintmax_t)uid);
    return numeric_buffer;
}

static const char* bx_stat_group_name(gid_t gid, char* numeric_buffer, size_t numeric_buffer_len) {
    struct group* gr = getgrgid(gid);
    if (gr != NULL && gr->gr_name != NULL && gr->gr_name[0] != '\0') {
        return gr->gr_name;
    }

    (void)snprintf(numeric_buffer, numeric_buffer_len, "%" PRIuMAX, (uintmax_t)gid);
    return numeric_buffer;
}

static void bx_stat_format_timestamp(const struct timespec* ts, char* buffer, size_t buffer_len) {
    struct tm local_tm;
    if (localtime_r(&ts->tv_sec, &local_tm) != NULL) {
        char datetime_buffer[64];
        char tz_buffer[32];
        size_t datetime_len = strftime(datetime_buffer, sizeof(datetime_buffer), "%Y-%m-%d %H:%M:%S", &local_tm);
        size_t tz_len = strftime(tz_buffer, sizeof(tz_buffer), "%z", &local_tm);
        if (datetime_len > 0 && tz_len > 0) {
            int written = snprintf(buffer, buffer_len, "%s.%09ld %s", datetime_buffer, ts->tv_nsec, tz_buffer);
            if (written >= 0 && (size_t)written < buffer_len) {
                return;
            }
        }
        else if (datetime_len > 0) {
            int written = snprintf(buffer, buffer_len, "%s.%09ld", datetime_buffer, ts->tv_nsec);
            if (written >= 0 && (size_t)written < buffer_len) {
                return;
            }
        }
    }

    (void)snprintf(buffer, buffer_len, "%" PRIdMAX ".%09ld", (intmax_t)ts->tv_sec, ts->tv_nsec);
}

static bool bx_stat_printf(struct bx_diag_ctx* diag, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int rc = vprintf(fmt, ap);
    va_end(ap);

    if (rc < 0) {
        bx_diag(diag, "write error: %s", strerror(errno));
        return false;
    }
    return true;
}

static bool bx_stat_print_file_info(const char* path, const struct stat* st, const struct bx_stat_options* options, struct bx_diag_ctx* diag) {
    char mode_string[11];
    char uid_buffer[32];
    char gid_buffer[32];
    char atime_buffer[96];
    char mtime_buffer[96];
    char ctime_buffer[96];
    const char* uid_name;
    const char* gid_name;
    char* link_target = NULL;

    if (!options->dereference && S_ISLNK(st->st_mode)) {
        link_target = bx_stat_readlink_target(path);
    }

    if (link_target != NULL) {
        if (!bx_stat_printf(diag, "  File: %s -> %s\n", path, link_target)) {
            free(link_target);
            return false;
        }
        free(link_target);
    }
    else if (!bx_stat_printf(diag, "  File: %s\n", path)) {
        return false;
    }

    bx_stat_mode_to_string(st->st_mode, mode_string);
    bx_stat_format_timestamp(&st->st_atim, atime_buffer, sizeof(atime_buffer));
    bx_stat_format_timestamp(&st->st_mtim, mtime_buffer, sizeof(mtime_buffer));
    bx_stat_format_timestamp(&st->st_ctim, ctime_buffer, sizeof(ctime_buffer));
    uid_name = bx_stat_user_name(st->st_uid, uid_buffer, sizeof(uid_buffer));
    gid_name = bx_stat_group_name(st->st_gid, gid_buffer, sizeof(gid_buffer));

    if (!bx_stat_printf(diag, "  Size: %" PRIdMAX "\tBlocks: %" PRIdMAX "\tIO Block: %" PRIdMAX "\t%s\n", (intmax_t)st->st_size, (intmax_t)st->st_blocks, (intmax_t)st->st_blksize,
                        bx_stat_file_type_description(st->st_mode))) {
        return false;
    }

    if (!bx_stat_printf(diag, "Device: %" PRIuMAX ",%" PRIuMAX "\tInode: %" PRIuMAX "\tLinks: %" PRIuMAX "\n", (uintmax_t)major(st->st_dev), (uintmax_t)minor(st->st_dev), (uintmax_t)st->st_ino,
                        (uintmax_t)st->st_nlink)) {
        return false;
    }

    if (!bx_stat_printf(diag, "Access: (%04o/%s)  Uid: (%5" PRIuMAX "/%s)   Gid: (%5" PRIuMAX "/%s)\n", (unsigned int)(st->st_mode & 07777u), mode_string, (uintmax_t)st->st_uid, uid_name,
                        (uintmax_t)st->st_gid, gid_name)) {
        return false;
    }

    if (!bx_stat_printf(diag, "Access: %s\n", atime_buffer)) {
        return false;
    }
    if (!bx_stat_printf(diag, "Modify: %s\n", mtime_buffer)) {
        return false;
    }
    if (!bx_stat_printf(diag, "Change: %s\n", ctime_buffer)) {
        return false;
    }
    if (!bx_stat_printf(diag, " Birth: -\n")) {
        return false;
    }

    return true;
}

int bx_stat_main(int argc, char** argv) {
    struct bx_stat_options options;
    struct bx_diag_ctx diag = {
        .progname = "stat",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };
    int first_operand = 0;

    if (!bx_stat_parse_options(argc, argv, &options, &first_operand, &diag)) {
        return diag.exit_status != 0 ? diag.exit_status : 1;
    }

    if (options.show_help) {
        bx_stat_print_help(stdout, options.progname);
        return 0;
    }

    if (options.show_version) {
        bx_stat_print_version(options.progname);
        return 0;
    }

    if (first_operand >= argc) {
        bx_diag(&diag, "missing operand");
        return diag.exit_status;
    }

    bool printed_entry = false;
    for (int i = first_operand; i < argc; i++) {
        struct stat st;
        int rc = options.dereference ? stat(argv[i], &st) : lstat(argv[i], &st);
        if (rc != 0) {
            bx_perror_path(&diag, argv[i]);
            continue;
        }

        if (printed_entry) {
            if (!bx_stat_printf(&diag, "\n")) {
                break;
            }
        }

        if (!bx_stat_print_file_info(argv[i], &st, &options, &diag)) {
            break;
        }
        printed_entry = true;
    }

    if (fflush(stdout) == EOF) {
        bx_diag(&diag, "write error: %s", strerror(errno));
    }

    return diag.exit_status;
}
