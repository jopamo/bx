#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"
#include "lib/cli_common.h"
#include "lib/fd_ops.h"
#include "lib/time_parse.h"
#include "lib/args_common.h"

enum bx_touch_time_source {
    BX_TOUCH_TIME_SOURCE_NOW = 0,
    BX_TOUCH_TIME_SOURCE_DATE,
    BX_TOUCH_TIME_SOURCE_TIMESTAMP,
    BX_TOUCH_TIME_SOURCE_REFERENCE,
};

struct bx_touch_options {
    const char* progname;
    bool update_atime;
    bool update_mtime;
    bool no_create;
    bool no_dereference;
    enum bx_touch_time_source time_source;
    struct timespec explicit_atime;
    struct timespec explicit_mtime;
    bool show_help;
    bool show_version;
};

static void bx_touch_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... FILE...\n", progname);
    fprintf(stream, "Update the access and modification times of each FILE to now.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -a               change only the access time\n");
    fprintf(stream, "  -d, --date=STRING  parse STRING and use it instead of current time\n");
    fprintf(stream, "  -h, --no-dereference  affect each symbolic link instead of any referenced file\n");
    fprintf(stream, "  -m               change only the modification time\n");
    fprintf(stream, "  -r, --reference=FILE  use FILE times instead of current time\n");
    fprintf(stream, "  -t STAMP         use [[CC]YY]MMDDhhmm[.ss] instead of current time\n");
    fprintf(stream, "  -c, --no-create  do not create any files\n");
    fprintf(stream, "      --help       display this help and exit\n");
    fprintf(stream, "      --version    output version information and exit\n");
}

static bool bx_touch_time_source_is_compatible(enum bx_touch_time_source current, enum bx_touch_time_source requested) {
    return current == BX_TOUCH_TIME_SOURCE_NOW || current == requested;
}

static void bx_touch_set_explicit_times(struct bx_touch_options* options, const struct timespec* atime, const struct timespec* mtime) {
    options->explicit_atime = *atime;
    options->explicit_mtime = *mtime;
}

static bool bx_touch_parse_date_literal(const char* value, struct timespec* timestamp_out) {
    const char* p = value;
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    long nsec = 0;
    bool have_seconds = false;

    while (isspace((unsigned char)*p)) {
        p++;
    }

    if (strlen(p) < 10) {
        return false;
    }

    if (!bx_time_parse_fixed_width_int(p, 0, 4, &year) || p[4] != '-' || !bx_time_parse_fixed_width_int(p, 5, 2, &month) || p[7] != '-' || !bx_time_parse_fixed_width_int(p, 8, 2, &day)) {
        return false;
    }
    p += 10;

    if (*p == ' ' || *p == 'T') {
        p++;
        if (strlen(p) < 5) {
            return false;
        }
        if (!bx_time_parse_fixed_width_int(p, 0, 2, &hour) || p[2] != ':' || !bx_time_parse_fixed_width_int(p, 3, 2, &minute)) {
            return false;
        }
        p += 5;

        if (*p == ':') {
            p++;
            if (strlen(p) < 2) {
                return false;
            }
            if (!bx_time_parse_fixed_width_int(p, 0, 2, &second)) {
                return false;
            }
            p += 2;
            have_seconds = true;
        }
    }
    else if (*p != '\0' && !isspace((unsigned char)*p)) {
        return false;
    }

    if (*p == '.') {
        if (!have_seconds) {
            return false;
        }
        if (!bx_time_parse_fractional_nanoseconds(&p, &nsec)) {
            return false;
        }
    }

    while (isspace((unsigned char)*p)) {
        p++;
    }
    if (*p != '\0') {
        return false;
    }

    return bx_time_build_local_timestamp(year, month, day, hour, minute, second, nsec, timestamp_out);
}

static bool bx_touch_parse_epoch_literal(const char* value, struct timespec* timestamp_out) {
    struct bx_time_epoch_parse_options options = {
        .allow_trailing_space = false,
        .normalize_negative_fraction = false,
    };
    return bx_time_parse_epoch_literal(value, &options, timestamp_out);
}

static bool bx_touch_parse_date_argument(const char* value, struct timespec* timestamp_out) {
    if (value != NULL && value[0] == '@') {
        return bx_touch_parse_epoch_literal(value, timestamp_out);
    }
    return bx_touch_parse_date_literal(value, timestamp_out);
}

static bool bx_touch_parse_stamp_argument(const char* value, struct timespec* timestamp_out) {
    if (value == NULL || value[0] == '\0') {
        return false;
    }

    const char* dot = strchr(value, '.');
    if (dot != NULL && strchr(dot + 1, '.') != NULL) {
        return false;
    }

    size_t main_len = (dot != NULL) ? (size_t)(dot - value) : strlen(value);
    if (main_len != 8 && main_len != 10 && main_len != 12) {
        return false;
    }

    int second = 0;
    if (dot != NULL) {
        const char* seconds_text = dot + 1;
        if (strlen(seconds_text) != 2 || !bx_time_parse_fixed_width_int(seconds_text, 0, 2, &second)) {
            return false;
        }
    }

    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;

    if (main_len == 12) {
        if (!bx_time_parse_fixed_width_int(value, 0, 4, &year) || !bx_time_parse_fixed_width_int(value, 4, 2, &month) || !bx_time_parse_fixed_width_int(value, 6, 2, &day) ||
            !bx_time_parse_fixed_width_int(value, 8, 2, &hour) || !bx_time_parse_fixed_width_int(value, 10, 2, &minute)) {
            return false;
        }
    }
    else if (main_len == 10) {
        int yy = 0;
        if (!bx_time_parse_fixed_width_int(value, 0, 2, &yy) || !bx_time_parse_fixed_width_int(value, 2, 2, &month) || !bx_time_parse_fixed_width_int(value, 4, 2, &day) ||
            !bx_time_parse_fixed_width_int(value, 6, 2, &hour) || !bx_time_parse_fixed_width_int(value, 8, 2, &minute)) {
            return false;
        }
        year = (yy >= 69) ? (1900 + yy) : (2000 + yy);
    }
    else {
        if (!bx_time_current_local_year(&year) || !bx_time_parse_fixed_width_int(value, 0, 2, &month) || !bx_time_parse_fixed_width_int(value, 2, 2, &day) ||
            !bx_time_parse_fixed_width_int(value, 4, 2, &hour) || !bx_time_parse_fixed_width_int(value, 6, 2, &minute)) {
            return false;
        }
    }

    return bx_time_build_local_timestamp(year, month, day, hour, minute, second, 0, timestamp_out);
}

static bool bx_touch_parse_options(int argc, char** argv, struct bx_touch_options* options, int* first_operand, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"date", required_argument, NULL, 'd'},
        {"no-create", no_argument, NULL, 'c'},
        {"no-dereference", no_argument, NULL, 'h'},
        {"reference", required_argument, NULL, 'r'},
        {"help", no_argument, NULL, 1},
        {"version", no_argument, NULL, 2},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "touch");
    diag->progname = options->progname;

    bx_args_getopt_reset();

    while (true) {
        int option_index = 0;
        int c = bx_args_getopt_long(argc, argv, "+:acd:hmr:t:", long_options, &option_index);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 'a':
                options->update_atime = true;
                break;
            case 'c':
                options->no_create = true;
                break;
            case 'd': {
                if (!bx_touch_time_source_is_compatible(options->time_source, BX_TOUCH_TIME_SOURCE_DATE)) {
                    bx_diag(diag, "cannot specify times from more than one source");
                    return false;
                }
                struct timespec parsed_date;
                if (!bx_touch_parse_date_argument(optarg, &parsed_date)) {
                    bx_diag(diag, "invalid date format '%s'", optarg);
                    return false;
                }
                bx_touch_set_explicit_times(options, &parsed_date, &parsed_date);
                options->time_source = BX_TOUCH_TIME_SOURCE_DATE;
                break;
            }
            case 'h':
                options->no_dereference = true;
                break;
            case 'm':
                options->update_mtime = true;
                break;
            case 'r': {
                if (!bx_touch_time_source_is_compatible(options->time_source, BX_TOUCH_TIME_SOURCE_REFERENCE)) {
                    bx_diag(diag, "cannot specify times from more than one source");
                    return false;
                }
                struct stat ref_st;
                if (stat(optarg, &ref_st) != 0) {
                    bx_perror_path(diag, optarg);
                    return false;
                }
                bx_touch_set_explicit_times(options, &ref_st.st_atim, &ref_st.st_mtim);
                options->time_source = BX_TOUCH_TIME_SOURCE_REFERENCE;
                break;
            }
            case 't': {
                if (!bx_touch_time_source_is_compatible(options->time_source, BX_TOUCH_TIME_SOURCE_TIMESTAMP)) {
                    bx_diag(diag, "cannot specify times from more than one source");
                    return false;
                }
                struct timespec parsed_stamp;
                if (!bx_touch_parse_stamp_argument(optarg, &parsed_stamp)) {
                    bx_diag(diag, "invalid date format '%s'", optarg);
                    return false;
                }
                bx_touch_set_explicit_times(options, &parsed_stamp, &parsed_stamp);
                options->time_source = BX_TOUCH_TIME_SOURCE_TIMESTAMP;
                break;
            }
            case 1:
                options->show_help = true;
                return true;
            case 2:
                options->show_version = true;
                return true;
            case ':':
                if (optopt != 0) {
                    const char* arg = (optind > 0 && optind <= argc) ? argv[optind - 1] : NULL;
                    if (arg != NULL && strncmp(arg, "--", 2) == 0) {
                        bx_diag(diag, "option '%s' requires an argument", arg);
                    }
                    else {
                        bx_diag(diag, "option requires an argument -- '%c'", optopt);
                    }
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
                bx_diag(diag, "internal option parsing error");
                return false;
        }
    }

    if (!options->update_atime && !options->update_mtime) {
        options->update_atime = true;
        options->update_mtime = true;
    }

    *first_operand = optind;
    return true;
}

static const struct timespec* bx_touch_requested_times(const struct bx_touch_options* options, struct timespec times[2]) {
    if (options->time_source == BX_TOUCH_TIME_SOURCE_NOW && options->update_atime && options->update_mtime) {
        return NULL;
    }

    if (options->time_source == BX_TOUCH_TIME_SOURCE_NOW) {
        times[0].tv_sec = 0;
        times[0].tv_nsec = options->update_atime ? UTIME_NOW : UTIME_OMIT;
        times[1].tv_sec = 0;
        times[1].tv_nsec = options->update_mtime ? UTIME_NOW : UTIME_OMIT;
        return times;
    }

    times[0] = options->explicit_atime;
    times[1] = options->explicit_mtime;
    if (!options->update_atime) {
        times[0].tv_sec = 0;
        times[0].tv_nsec = UTIME_OMIT;
    }
    if (!options->update_mtime) {
        times[1].tv_sec = 0;
        times[1].tv_nsec = UTIME_OMIT;
    }
    return times;
}

static void bx_touch_path(const char* path, const struct bx_touch_options* options, struct bx_diag_ctx* diag) {
    struct timespec times[2];
    const struct timespec* requested_times = bx_touch_requested_times(options, times);
    int utimensat_flags = options->no_dereference ? AT_SYMLINK_NOFOLLOW : 0;

    if (utimensat(AT_FDCWD, path, requested_times, utimensat_flags) == 0) {
        return;
    }

    if (errno != ENOENT) {
        bx_perror_path(diag, path);
        return;
    }

    if (options->no_create) {
        return;
    }

    if (options->no_dereference) {
        bx_perror_path(diag, path);
        return;
    }

    int fd = bx_fd_open_cloexec(path, O_WRONLY | O_CREAT, 0666u);
    if (fd < 0) {
        bx_perror_path(diag, path);
        return;
    }

    if (close(fd) != 0) {
        bx_perror_path(diag, path);
        return;
    }

    if (utimensat(AT_FDCWD, path, requested_times, utimensat_flags) != 0) {
        bx_perror_path(diag, path);
    }
}

int bx_touch_main(int argc, char** argv) {
    struct bx_touch_options options;
    struct bx_diag_ctx diag = {
        .progname = "touch",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };
    int first_operand = 0;

    if (!bx_touch_parse_options(argc, argv, &options, &first_operand, &diag)) {
        return diag.exit_status != 0 ? diag.exit_status : 1;
    }

    if (options.show_help) {
        bx_touch_print_help(stdout, options.progname);
        return 0;
    }

    if (options.show_version) {
        bx_cli_print_version(options.progname);
        return 0;
    }

    if (first_operand >= argc) {
        bx_diag(&diag, "missing file operand");
        return diag.exit_status;
    }

    for (int i = first_operand; i < argc; i++) {
        bx_touch_path(argv[i], &options, &diag);
    }

    return diag.exit_status;
}
