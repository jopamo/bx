#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <grp.h>
#include <inttypes.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/vfs.h>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/magic.h>
#endif

#include "applets.h"
#include "bx/diag.h"
#include "bx/libbx.h"
#include "lib/cli_common.h"
#include "lib/args_common.h"
#include "lib/path_quote.h"

struct bx_stat_options {
    const char* progname;
    bool dereference;
    bool file_system;
    bool terse;
    const char* format;
    bool format_interpret_escapes;
    bool show_help;
    bool show_version;
};

struct bx_stat_file_format_ctx {
    const char* path;
    const struct stat* st;
    const struct bx_stat_options* options;
};

struct bx_stat_fs_format_ctx {
    const char* path;
    const struct statfs* fs;
};

typedef bool (*bx_stat_format_conversion_fn)(char conv, void* ctx, struct bx_diag_ctx* diag);

static const char* const BX_STAT_TERSE_FILE_FORMAT = "%n %s %b %f %u %g %D %i %h %t %T %X %Y %Z %W %o";
static const char* const BX_STAT_TERSE_FILESYSTEM_FORMAT = "%n %i %l %t %s %S %b %f %a %c %d";

static void bx_stat_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... FILE...\n", progname);
    fprintf(stream, "Display file status.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -c, --format=FORMAT  use FORMAT (appends newline)\n");
    fprintf(stream, "      --printf=FORMAT  use FORMAT with backslash escapes (no newline)\n");
    fprintf(stream, "  -f, --file-system    display file system status\n");
    fprintf(stream, "  -L, --dereference  follow links\n");
    fprintf(stream, "  -t, --terse          print status in terse form\n");
    fprintf(stream, "      --help           display this help and exit\n");
    fprintf(stream, "      --version        output version information and exit\n");
}

static bool bx_stat_parse_options(int argc, char** argv, struct bx_stat_options* options, int* first_operand, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"format", required_argument, NULL, 'c'}, {"printf", required_argument, NULL, 3}, {"file-system", no_argument, NULL, 'f'}, {"dereference", no_argument, NULL, 'L'},
        {"terse", no_argument, NULL, 't'},        {"help", no_argument, NULL, 1},         {"version", no_argument, NULL, 2},       {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "stat");
    diag->progname = options->progname;

    bx_args_getopt_reset();

    while (true) {
        int c = bx_args_getopt_long(argc, argv, "+c:fLt", long_options, NULL);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 'c':
                options->format = optarg;
                options->format_interpret_escapes = false;
                break;
            case 'f':
                options->file_system = true;
                break;
            case 'L':
                options->dereference = true;
                break;
            case 't':
                options->terse = true;
                break;
            case 3:
                options->format = optarg;
                options->format_interpret_escapes = true;
                break;
            case 1:
                options->show_help = true;
                return true;
            case 2:
                options->show_version = true;
                return true;
            case '?':
                bx_cli_diag_unrecognized_option(diag, optopt, optind, argc, argv);
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

static uint64_t bx_stat_fsid_value(const void* fsid_ptr, size_t fsid_size) {
    if (fsid_size >= (sizeof(uint32_t) * 2u)) {
        uint32_t words[2] = {0, 0};
        memcpy(words, fsid_ptr, sizeof(words));
        return ((uint64_t)words[0] << 32u) | (uint64_t)words[1];
    }

    uint64_t value = 0;
    size_t copy_len = sizeof(value);
    if (fsid_size < copy_len) {
        copy_len = fsid_size;
    }
    memcpy(&value, fsid_ptr, copy_len);
    return value;
}

static uintmax_t bx_stat_fs_fragment_size(const struct statfs* fs) {
    return (fs->f_frsize > 0) ? (uintmax_t)fs->f_frsize : (uintmax_t)fs->f_bsize;
}

static const char* bx_stat_fs_type_name(long type) {
    unsigned long value = (unsigned long)type;

#ifdef TMPFS_MAGIC
    if (value == (unsigned long)TMPFS_MAGIC) {
        return "tmpfs";
    }
#endif
#ifdef EXT2_SUPER_MAGIC
    if (value == (unsigned long)EXT2_SUPER_MAGIC) {
        return "ext";
    }
#endif
#ifdef BTRFS_SUPER_MAGIC
    if (value == (unsigned long)BTRFS_SUPER_MAGIC) {
        return "btrfs";
    }
#endif
#ifdef XFS_SUPER_MAGIC
    if (value == (unsigned long)XFS_SUPER_MAGIC) {
        return "xfs";
    }
#endif
#ifdef NFS_SUPER_MAGIC
    if (value == (unsigned long)NFS_SUPER_MAGIC) {
        return "nfs";
    }
#endif
#ifdef CIFS_MAGIC_NUMBER
    if (value == (unsigned long)CIFS_MAGIC_NUMBER) {
        return "cifs";
    }
#endif
#ifdef PROC_SUPER_MAGIC
    if (value == (unsigned long)PROC_SUPER_MAGIC) {
        return "proc";
    }
#endif
#ifdef SYSFS_MAGIC
    if (value == (unsigned long)SYSFS_MAGIC) {
        return "sysfs";
    }
#endif
#ifdef RAMFS_MAGIC
    if (value == (unsigned long)RAMFS_MAGIC) {
        return "ramfs";
    }
#endif
#ifdef DEVPTS_SUPER_MAGIC
    if (value == (unsigned long)DEVPTS_SUPER_MAGIC) {
        return "devpts";
    }
#endif
#ifdef OVERLAYFS_SUPER_MAGIC
    if (value == (unsigned long)OVERLAYFS_SUPER_MAGIC) {
        return "overlay";
    }
#endif
#ifdef FUSE_SUPER_MAGIC
    if (value == (unsigned long)FUSE_SUPER_MAGIC) {
        return "fuse";
    }
#endif
    return "unknown";
}

static bool bx_stat_putc(struct bx_diag_ctx* diag, unsigned char ch) {
    if (fputc((int)ch, stdout) == EOF) {
        bx_diag(diag, "write error: %s", strerror(errno));
        return false;
    }
    return true;
}

static bool bx_stat_puts(struct bx_diag_ctx* diag, const char* text) {
    if (fputs(text, stdout) == EOF) {
        bx_diag(diag, "write error: %s", strerror(errno));
        return false;
    }
    return true;
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

static bool bx_stat_print_quoted_path(struct bx_diag_ctx* diag, const char* text) {
    char* quoted = bx_path_quote_dup(text, BX_PATH_QUOTE_SINGLE_BACKSLASH);
    bool ok = bx_stat_puts(diag, quoted);
    free(quoted);
    return ok;
}

static bool bx_stat_print_file_quoted_name(const struct bx_stat_file_format_ctx* file_ctx, struct bx_diag_ctx* diag) {
    if (!bx_stat_print_quoted_path(diag, file_ctx->path)) {
        return false;
    }

    if (!file_ctx->options->dereference && S_ISLNK(file_ctx->st->st_mode)) {
        char* link_target = bx_stat_readlink_target(file_ctx->path);
        if (link_target != NULL) {
            bool ok = bx_stat_puts(diag, " -> ") && bx_stat_print_quoted_path(diag, link_target);
            free(link_target);
            if (!ok) {
                return false;
            }
        }
    }

    return true;
}

static bool bx_stat_print_human_timestamp(struct bx_diag_ctx* diag, const struct timespec* ts) {
    char buffer[96];
    bx_stat_format_timestamp(ts, buffer, sizeof(buffer));
    return bx_stat_puts(diag, buffer);
}

static bool bx_stat_print_file_conversion(char conv, void* ctx, struct bx_diag_ctx* diag) {
    const struct bx_stat_file_format_ctx* file_ctx = (const struct bx_stat_file_format_ctx*)ctx;
    const struct stat* st = file_ctx->st;

    switch (conv) {
        case 'a':
            return bx_stat_printf(diag, "%o", (unsigned int)(st->st_mode & 07777u));
        case 'A': {
            char mode_buffer[11];
            bx_stat_mode_to_string(st->st_mode, mode_buffer);
            return bx_stat_puts(diag, mode_buffer);
        }
        case 'b':
            return bx_stat_printf(diag, "%" PRIdMAX, (intmax_t)st->st_blocks);
        case 'B':
            return bx_stat_puts(diag, "512");
        case 'd':
            return bx_stat_printf(diag, "%" PRIuMAX, (uintmax_t)st->st_dev);
        case 'D':
            return bx_stat_printf(diag, "%" PRIxMAX, (uintmax_t)st->st_dev);
        case 'f':
            return bx_stat_printf(diag, "%" PRIxMAX, (uintmax_t)st->st_mode);
        case 'F':
            return bx_stat_puts(diag, bx_stat_file_type_description(st->st_mode));
        case 'g':
            return bx_stat_printf(diag, "%" PRIuMAX, (uintmax_t)st->st_gid);
        case 'G': {
            char gid_buffer[32];
            return bx_stat_puts(diag, bx_stat_group_name(st->st_gid, gid_buffer, sizeof(gid_buffer)));
        }
        case 'h':
            return bx_stat_printf(diag, "%" PRIuMAX, (uintmax_t)st->st_nlink);
        case 'i':
            return bx_stat_printf(diag, "%" PRIuMAX, (uintmax_t)st->st_ino);
        case 'n':
            return bx_stat_puts(diag, file_ctx->path);
        case 'N':
            return bx_stat_print_file_quoted_name(file_ctx, diag);
        case 'o':
            return bx_stat_printf(diag, "%" PRIdMAX, (intmax_t)st->st_blksize);
        case 's':
            return bx_stat_printf(diag, "%" PRIdMAX, (intmax_t)st->st_size);
        case 't':
            return bx_stat_printf(diag, "%" PRIxMAX, (uintmax_t)major(st->st_rdev));
        case 'T':
            return bx_stat_printf(diag, "%" PRIxMAX, (uintmax_t)minor(st->st_rdev));
        case 'u':
            return bx_stat_printf(diag, "%" PRIuMAX, (uintmax_t)st->st_uid);
        case 'U': {
            char uid_buffer[32];
            return bx_stat_puts(diag, bx_stat_user_name(st->st_uid, uid_buffer, sizeof(uid_buffer)));
        }
        case 'w':
            return bx_stat_puts(diag, "-");
        case 'W':
            return bx_stat_puts(diag, "0");
        case 'x':
            return bx_stat_print_human_timestamp(diag, &st->st_atim);
        case 'X':
            return bx_stat_printf(diag, "%" PRIdMAX, (intmax_t)st->st_atim.tv_sec);
        case 'y':
            return bx_stat_print_human_timestamp(diag, &st->st_mtim);
        case 'Y':
            return bx_stat_printf(diag, "%" PRIdMAX, (intmax_t)st->st_mtim.tv_sec);
        case 'z':
            return bx_stat_print_human_timestamp(diag, &st->st_ctim);
        case 'Z':
            return bx_stat_printf(diag, "%" PRIdMAX, (intmax_t)st->st_ctim.tv_sec);
        default:
            return bx_stat_putc(diag, '?');
    }
}

static bool bx_stat_print_fs_conversion(char conv, void* ctx, struct bx_diag_ctx* diag) {
    const struct bx_stat_fs_format_ctx* fs_ctx = (const struct bx_stat_fs_format_ctx*)ctx;
    const struct statfs* fs = fs_ctx->fs;

    switch (conv) {
        case 'a':
            return bx_stat_printf(diag, "%" PRIuMAX, (uintmax_t)fs->f_bavail);
        case 'b':
            return bx_stat_printf(diag, "%" PRIuMAX, (uintmax_t)fs->f_blocks);
        case 'c':
            return bx_stat_printf(diag, "%" PRIuMAX, (uintmax_t)fs->f_files);
        case 'd':
            return bx_stat_printf(diag, "%" PRIuMAX, (uintmax_t)fs->f_ffree);
        case 'f':
            return bx_stat_printf(diag, "%" PRIuMAX, (uintmax_t)fs->f_bfree);
        case 'i':
            return bx_stat_printf(diag, "%" PRIx64, bx_stat_fsid_value(&fs->f_fsid, sizeof(fs->f_fsid)));
        case 'l':
            return bx_stat_printf(diag, "%" PRIuMAX, (uintmax_t)fs->f_namelen);
        case 'n':
            return bx_stat_puts(diag, fs_ctx->path);
        case 's':
            return bx_stat_printf(diag, "%" PRIuMAX, (uintmax_t)fs->f_bsize);
        case 'S':
            return bx_stat_printf(diag, "%" PRIuMAX, bx_stat_fs_fragment_size(fs));
        case 't':
            return bx_stat_printf(diag, "%" PRIxMAX, (uintmax_t)(unsigned long)fs->f_type);
        case 'T':
            return bx_stat_puts(diag, bx_stat_fs_type_name(fs->f_type));
        default:
            return bx_stat_putc(diag, '?');
    }
}

static int bx_stat_hex_value(unsigned char ch) {
    if (ch >= '0' && ch <= '9') {
        return (int)(ch - '0');
    }
    if (ch >= 'a' && ch <= 'f') {
        return 10 + (int)(ch - 'a');
    }
    if (ch >= 'A' && ch <= 'F') {
        return 10 + (int)(ch - 'A');
    }
    return -1;
}

static bool bx_stat_emit_escape(struct bx_diag_ctx* diag, const char** cursor) {
    const unsigned char* p = (const unsigned char*)*cursor;
    if (*p == '\0') {
        return bx_stat_putc(diag, '\\');
    }

    unsigned char esc = *p++;
    switch (esc) {
        case 'a':
            *cursor = (const char*)p;
            return bx_stat_putc(diag, '\a');
        case 'b':
            *cursor = (const char*)p;
            return bx_stat_putc(diag, '\b');
        case 'f':
            *cursor = (const char*)p;
            return bx_stat_putc(diag, '\f');
        case 'n':
            *cursor = (const char*)p;
            return bx_stat_putc(diag, '\n');
        case 'r':
            *cursor = (const char*)p;
            return bx_stat_putc(diag, '\r');
        case 't':
            *cursor = (const char*)p;
            return bx_stat_putc(diag, '\t');
        case 'v':
            *cursor = (const char*)p;
            return bx_stat_putc(diag, '\v');
        case '\\':
            *cursor = (const char*)p;
            return bx_stat_putc(diag, '\\');
        case 'x': {
            unsigned int value = 0;
            int digits = 0;
            while (isxdigit(*p) && digits < 2) {
                int nibble = bx_stat_hex_value(*p);
                value = (value * 16u) + (unsigned int)nibble;
                p++;
                digits++;
            }
            *cursor = (const char*)p;
            if (digits == 0) {
                return bx_stat_putc(diag, 'x');
            }
            return bx_stat_putc(diag, (unsigned char)(value & 0xFFu));
        }
        default:
            if (esc >= '0' && esc <= '7') {
                unsigned int value = (unsigned int)(esc - '0');
                int digits = 1;
                while (*p >= '0' && *p <= '7' && digits < 3) {
                    value = (value * 8u) + (unsigned int)(*p - '0');
                    p++;
                    digits++;
                }
                *cursor = (const char*)p;
                return bx_stat_putc(diag, (unsigned char)(value & 0xFFu));
            }
            *cursor = (const char*)p;
            return bx_stat_putc(diag, esc);
    }
}

static bool bx_stat_render_format(const char* format, bool interpret_escapes, bool append_newline, bx_stat_format_conversion_fn conversion_fn, void* conversion_ctx, struct bx_diag_ctx* diag) {
    const char* p = format;
    while (*p != '\0') {
        unsigned char ch = (unsigned char)*p++;
        if (interpret_escapes && ch == '\\') {
            if (!bx_stat_emit_escape(diag, &p)) {
                return false;
            }
            continue;
        }

        if (ch != '%') {
            if (!bx_stat_putc(diag, ch)) {
                return false;
            }
            continue;
        }

        char conv = *p;
        if (conv == '\0') {
            if (!bx_stat_putc(diag, '%')) {
                return false;
            }
            break;
        }
        p++;

        if (conv == '%') {
            if (!bx_stat_putc(diag, '%')) {
                return false;
            }
            continue;
        }

        if (!conversion_fn(conv, conversion_ctx, diag)) {
            return false;
        }
    }

    if (append_newline) {
        return bx_stat_putc(diag, '\n');
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

static bool bx_stat_print_fs_info(const char* path, const struct statfs* fs, struct bx_diag_ctx* diag) {
    if (!bx_stat_printf(diag, "  File: \"%s\"\n", path)) {
        return false;
    }
    if (!bx_stat_printf(diag, "    ID: %" PRIx64 " Namelen: %" PRIuMAX " Type: %s\n", bx_stat_fsid_value(&fs->f_fsid, sizeof(fs->f_fsid)), (uintmax_t)fs->f_namelen,
                        bx_stat_fs_type_name(fs->f_type))) {
        return false;
    }
    if (!bx_stat_printf(diag, "Block size: %" PRIuMAX "       Fundamental block size: %" PRIuMAX "\n", (uintmax_t)fs->f_bsize, bx_stat_fs_fragment_size(fs))) {
        return false;
    }
    if (!bx_stat_printf(diag, "Blocks: Total: %" PRIuMAX "   Free: %" PRIuMAX "   Available: %" PRIuMAX "\n", (uintmax_t)fs->f_blocks, (uintmax_t)fs->f_bfree, (uintmax_t)fs->f_bavail)) {
        return false;
    }
    if (!bx_stat_printf(diag, "Inodes: Total: %" PRIuMAX "   Free: %" PRIuMAX "\n", (uintmax_t)fs->f_files, (uintmax_t)fs->f_ffree)) {
        return false;
    }
    return true;
}

static bool bx_stat_print_file_using_format(const char* path,
                                            const struct stat* st,
                                            const struct bx_stat_options* options,
                                            const char* format,
                                            bool interpret_escapes,
                                            bool append_newline,
                                            struct bx_diag_ctx* diag) {
    struct bx_stat_file_format_ctx format_ctx = {
        .path = path,
        .st = st,
        .options = options,
    };

    return bx_stat_render_format(format, interpret_escapes, append_newline, bx_stat_print_file_conversion, &format_ctx, diag);
}

static bool bx_stat_print_fs_using_format(const char* path, const struct statfs* fs, const char* format, bool interpret_escapes, bool append_newline, struct bx_diag_ctx* diag) {
    struct bx_stat_fs_format_ctx format_ctx = {
        .path = path,
        .fs = fs,
    };

    return bx_stat_render_format(format, interpret_escapes, append_newline, bx_stat_print_fs_conversion, &format_ctx, diag);
}

static const char* bx_stat_effective_format(const struct bx_stat_options* options, bool* interpret_escapes_out, bool* append_newline_out) {
    if (options->format != NULL) {
        *interpret_escapes_out = options->format_interpret_escapes;
        *append_newline_out = !options->format_interpret_escapes;
        return options->format;
    }
    if (options->terse) {
        *interpret_escapes_out = false;
        *append_newline_out = true;
        return options->file_system ? BX_STAT_TERSE_FILESYSTEM_FORMAT : BX_STAT_TERSE_FILE_FORMAT;
    }

    *interpret_escapes_out = false;
    *append_newline_out = false;
    return NULL;
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
        bx_cli_print_version(options.progname);
        return 0;
    }

    if (first_operand >= argc) {
        bx_diag(&diag, "missing operand");
        return diag.exit_status;
    }

    bool format_interpret_escapes = false;
    bool format_append_newline = false;
    const char* format = bx_stat_effective_format(&options, &format_interpret_escapes, &format_append_newline);

    bool printed_entry = false;
    for (int i = first_operand; i < argc; i++) {
        if (options.file_system) {
            struct statfs fs;
            if (statfs(argv[i], &fs) != 0) {
                bx_perror_path(&diag, argv[i]);
                continue;
            }

            if (format == NULL && printed_entry) {
                if (!bx_stat_printf(&diag, "\n")) {
                    break;
                }
            }

            if (format != NULL) {
                if (!bx_stat_print_fs_using_format(argv[i], &fs, format, format_interpret_escapes, format_append_newline, &diag)) {
                    break;
                }
            }
            else if (!bx_stat_print_fs_info(argv[i], &fs, &diag)) {
                break;
            }

            printed_entry = true;
            continue;
        }

        struct stat st;
        int rc = options.dereference ? stat(argv[i], &st) : lstat(argv[i], &st);
        if (rc != 0) {
            bx_perror_path(&diag, argv[i]);
            continue;
        }

        if (format == NULL && printed_entry) {
            if (!bx_stat_printf(&diag, "\n")) {
                break;
            }
        }

        if (format != NULL) {
            if (!bx_stat_print_file_using_format(argv[i], &st, &options, format, format_interpret_escapes, format_append_newline, &diag)) {
                break;
            }
        }
        else if (!bx_stat_print_file_info(argv[i], &st, &options, &diag)) {
            break;
        }
        printed_entry = true;
    }

    if (fflush(stdout) == EOF) {
        bx_diag(&diag, "write error: %s", strerror(errno));
    }

    return diag.exit_status;
}
