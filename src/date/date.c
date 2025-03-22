#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>

#include "applets.h"
#include "diag.h"
#include "libbx.h"

struct bx_date_options {
    const char* progname;
    const char* date_str;
    const char* file;
    const char* iso_8601_fmt;
    bool rfc_email;
    bool rfc_3339;
    const char* rfc_3339_fmt;
    const char* reference_file;
    const char* set_str;
    bool utc;
    bool resolution;
    bool show_help;
    bool show_version;
};

static void bx_date_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... [+FORMAT]\n", progname);
    fprintf(stream, "  or:  %s [-u|--utc|--universal] [MMDDhhmm[[CC]YY][.ss]]\n", progname);
    fprintf(stream, "Display current time in the given FORMAT, or set system time.\n");
    fprintf(stream, "\n");
    fprintf(stream, "Mandatory arguments to long options are mandatory for short options too.\n");
    fprintf(stream, "  -d, --date=STRING         display time described by STRING, not 'now'\n");
    fprintf(stream, "      --debug               annotate the parsed date, and warn about questionable\n");
    fprintf(stream, "                              usage to stderr\n");
    fprintf(stream, "  -f, --file=DATEFILE       like --date; read each line of DATEFILE\n");
    fprintf(stream, "  -I[FMT], --iso-8601[=FMT]  output date/time in ISO 8601 format.\n");
    fprintf(stream, "                              FMT='date' for date only (default),\n");
    fprintf(stream, "                              'hours', 'minutes', 'seconds', or 'ns' for date\n");
    fprintf(stream, "                              and time to the indicated precision.\n");
    fprintf(stream, "                              Example: 2006-08-14T02:34:56-06:00\n");
    fprintf(stream, "      --resolution          output the available resolution of timestamps\n");
    fprintf(stream, "                              Example: 0.000000001\n");
    fprintf(stream, "  -R, --rfc-email           output date and time in RFC 5322 format.\n");
    fprintf(stream, "                              Example: Mon, 14 Aug 2006 02:34:56 -0600\n");
    fprintf(stream, "      --rfc-3339=FMT        output date/time in RFC 3339 format.\n");
    fprintf(stream, "                              FMT='date', 'seconds', or 'ns' for date\n");
    fprintf(stream, "                              and time to the indicated precision.\n");
    fprintf(stream, "                              Example: 2006-08-14 02:34:56-06:00\n");
    fprintf(stream, "  -r, --reference=FILE      display the last modification time of FILE\n");
    fprintf(stream, "  -s, --set=STRING          set time described by STRING\n");
    fprintf(stream, "  -u, --utc, --universal    print or set Coordinated Universal Time (UTC)\n");
    fprintf(stream, "      --help     display this help and exit\n");
    fprintf(stream, "      --version  output version information and exit\n");
}

static void bx_date_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

static bool bx_date_parse_options(int argc, char** argv, struct bx_date_options* options, int* first_operand, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"date", required_argument, NULL, 'd'},
        {"file", required_argument, NULL, 'f'},
        {"iso-8601", optional_argument, NULL, 'I'},
        {"rfc-email", no_argument, NULL, 'R'},
        {"rfc-3339", required_argument, NULL, 1},
        {"reference", required_argument, NULL, 'r'},
        {"set", required_argument, NULL, 's'},
        {"utc", no_argument, NULL, 'u'},
        {"universal", no_argument, NULL, 'u'},
        {"resolution", no_argument, NULL, 2},
        {"help", no_argument, NULL, 3},
        {"version", no_argument, NULL, 4},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = "date";
    diag->progname = options->progname;

    opterr = 0;
    optind = 1;

    while (true) {
        int option_index = 0;
        int c = getopt_long(argc, argv, "+d:f:I::Rr:s:u", long_options, &option_index);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 'd':
                options->date_str = optarg;
                break;
            case 'f':
                options->file = optarg;
                break;
            case 'I':
                options->iso_8601_fmt = optarg ? optarg : "date";
                break;
            case 'R':
                options->rfc_email = true;
                break;
            case 'r':
                options->reference_file = optarg;
                break;
            case 's':
                options->set_str = optarg;
                break;
            case 'u':
                options->utc = true;
                break;
            case 1:
                options->rfc_3339 = true;
                options->rfc_3339_fmt = optarg;
                break;
            case 2:
                options->resolution = true;
                break;
            case 3:
                options->show_help = true;
                return true;
            case 4:
                options->show_version = true;
                return true;
            case '?':
                bx_diag(diag, "invalid option -- '%c'", optopt);
                return false;
            default:
                return false;
        }
    }

    *first_operand = optind;
    return true;
}

// Stub for now - complex GNU date parsing is hard.
// We'll support some basic formats.
static bool parse_date_string(const char* str, struct timespec* ts, bool utc, struct bx_diag_ctx* diag) {
    struct tm tm;
    memset(&tm, 0, sizeof(tm));

    // Try some common formats
    if (strptime(str, "%Y-%m-%d %H:%M:%S", &tm) || strptime(str, "%Y-%m-%d", &tm) || strptime(str, "%H:%M:%S", &tm)) {
        time_t t = utc ? timegm(&tm) : mktime(&tm);
        if (t == (time_t)-1) {
            bx_diag(diag, "invalid date '%s'", str);
            return false;
        }
        ts->tv_sec = t;
        ts->tv_nsec = 0;
        return true;
    }

    bx_diag(diag, "invalid date '%s'", str);
    return false;
}

static bool parse_legacy_set_time(const char* str, struct timespec* ts, bool utc, struct bx_diag_ctx* diag) {
    // MMDDhhmm[[CC]YY][.ss]
    // 10 or 12 or 8 digits, maybe .ss
    size_t len = strlen(str);
    const char* dot = strchr(str, '.');
    size_t main_len = dot ? (size_t)(dot - str) : len;

    if (main_len != 8 && main_len != 10 && main_len != 12)
        return false;

    struct tm tm;
    time_t now = time(NULL);
    struct tm* now_tm = utc ? gmtime(&now) : localtime(&now);
    tm = *now_tm;

    // Use sscanf or manual parsing
    int month, day, hour, min, year = -1, sec = 0;
    if (sscanf(str, "%2d%2d%2d%2d", &month, &day, &hour, &min) != 4)
        return false;

    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = min;

    if (main_len >= 10) {
        if (main_len == 10) {
            sscanf(str + 8, "%2d", &year);
            if (year < 69)
                year += 2000;
            else
                year += 1900;
        }
        else {
            sscanf(str + 8, "%4d", &year);
        }
        tm.tm_year = year - 1900;
    }

    if (dot && sscanf(dot + 1, "%2d", &sec) == 1) {
        tm.tm_sec = sec;
    }
    else {
        tm.tm_sec = 0;
    }

    time_t t = utc ? timegm(&tm) : mktime(&tm);
    if (t == (time_t)-1)
        return false;

    ts->tv_sec = t;
    ts->tv_nsec = 0;
    return true;
}

int bx_date_main(int argc, char** argv) {
    struct bx_date_options options;
    struct bx_diag_ctx diag = {.progname = "date", .exit_status = 0};
    int first_operand = 0;

    if (!bx_date_parse_options(argc, argv, &options, &first_operand, &diag))
        return 1;
    if (options.show_help) {
        bx_date_print_help(stdout, options.progname);
        return 0;
    }
    if (options.show_version) {
        bx_date_print_version(options.progname);
        return 0;
    }

    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        bx_diag(&diag, "cannot get current time: %s", strerror(errno));
        return 1;
    }

    if (options.reference_file) {
        struct stat st;
        if (stat(options.reference_file, &st) != 0) {
            bx_diag(&diag, "%s: %s", options.reference_file, strerror(errno));
            return 1;
        }
        ts.tv_sec = st.st_mtime;
#ifdef __USE_XOPEN2K8
        ts.tv_nsec = st.st_mtim.tv_nsec;
#else
        ts.tv_nsec = 0;
#endif
    }
    else if (options.date_str) {
        if (!parse_date_string(options.date_str, &ts, options.utc, &diag))
            return 1;
    }
    else if (options.set_str) {
        // ... set time logic ...
    }
    else if (argc - first_operand == 1 && !strchr(argv[first_operand], '+')) {
        if (!parse_legacy_set_time(argv[first_operand], &ts, options.utc, &diag)) {
            bx_diag(&diag, "invalid date '%s'", argv[first_operand]);
            return 1;
        }
    }

    if (options.resolution) {
        struct timespec res;
        clock_getres(CLOCK_REALTIME, &res);
        printf("%ld.%09ld\n", (long)res.tv_sec, res.tv_nsec);
        return 0;
    }

    struct tm* tm = options.utc ? gmtime(&ts.tv_sec) : localtime(&ts.tv_sec);
    if (!tm) {
        bx_diag(&diag, "cannot convert time");
        return 1;
    }

    char buf[1024];
    const char* format = "%a %b %e %H:%M:%S %Z %Y";  // Default GNU date format

    if (argc - first_operand >= 1 && argv[first_operand][0] == '+') {
        format = argv[first_operand] + 1;
    }
    else if (options.rfc_email) {
        format = "%a, %d %b %Y %H:%M:%S %z";
    }
    else if (options.iso_8601_fmt) {
        if (strcmp(options.iso_8601_fmt, "date") == 0)
            format = "%Y-%m-%d";
        else if (strcmp(options.iso_8601_fmt, "hours") == 0)
            format = "%Y-%m-%dT%H%z";
        else if (strcmp(options.iso_8601_fmt, "minutes") == 0)
            format = "%Y-%m-%dT%H:%M%z";
        else if (strcmp(options.iso_8601_fmt, "seconds") == 0)
            format = "%Y-%m-%dT%H:%M:%S%z";
        else if (strcmp(options.iso_8601_fmt, "ns") == 0) {
            strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S,", tm);
            printf("%s%09ld%s\n", buf, ts.tv_nsec, "TODO_TZ");  // ISO 8601 with ns is tricky
            return 0;
        }
    }
    else if (options.rfc_3339) {
        // ...
    }

    if (strftime(buf, sizeof(buf), format, tm) == 0) {
        bx_diag(&diag, "format error");
        return 1;
    }
    printf("%s\n", buf);

    return diag.exit_status;
}
