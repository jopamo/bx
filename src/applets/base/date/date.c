#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>

#include "applets.h"
#include "bx/diag.h"
#include "bx/libbx.h"
#include "lib/time_parse.h"

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
    bool debug;
    bool show_help;
    bool show_version;
};

static const char* bx_date_progname(const char* argv0) {
    if (argv0 == NULL || argv0[0] == '\0') {
        return "date";
    }

    const char* base = strrchr(argv0, '/');
    if (base != NULL && base[1] != '\0') {
        return base + 1;
    }

    return argv0;
}

static void bx_date_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... [+FORMAT]\n", progname);
    fprintf(stream, "  or:  %s [OPTION]... MMDDhhmm[[CC]YY][.ss]\n", progname);
    fputs("Display date and time in the given FORMAT.\n", stream);
    fputs("With -s, or with MMDDhhmm[[CC]YY][.ss], set the date and time first.\n", stream);
    fputs("\n", stream);
    fputs("Mandatory arguments to long options are mandatory for short options too.\n", stream);
    fputs("  -d, --date=STRING\n", stream);
    fputs("         display time described by STRING, not 'now'\n", stream);
    fputs("      --debug\n", stream);
    fputs("         annotate the parsed date,\n", stream);
    fputs("         and warn about questionable usage to standard error\n", stream);
    fputs("  -f, --file=DATEFILE\n", stream);
    fputs("         like --date; once for each line of DATEFILE;\n", stream);
    fputs("         if DATEFILE is -, read names from standard input\n", stream);
    fputs("  -I[FMT], --iso-8601[=FMT]\n", stream);
    fputs("         output date/time in ISO 8601 format.\n", stream);
    fputs("         FMT='date' (default), 'hours', 'minutes', 'seconds', or 'ns'\n", stream);
    fputs("         for date and time to the indicated precision.\n", stream);
    fputs("         Example: 2006-08-14T02:34:56-06:00\n", stream);
    fputs("      --resolution\n", stream);
    fputs("         output the available resolution of timestamps.\n", stream);
    fputs("         Example: 0.000000001\n", stream);
    fputs("  -R, --rfc-email\n", stream);
    fputs("         output date and time in RFC 5322 format.\n", stream);
    fputs("         Example: Mon, 14 Aug 2006 02:34:56 +0000\n", stream);
    fputs("      --rfc-3339=FMT\n", stream);
    fputs("         output date/time in RFC 3339 format.\n", stream);
    fputs("         FMT='date', 'seconds', or 'ns'\n", stream);
    fputs("         for date and time to the indicated precision.\n", stream);
    fputs("         Example: 2006-08-14 02:34:56-06:00\n", stream);
    fputs("  -r, --reference=FILE\n", stream);
    fputs("         display the last modification time of FILE\n", stream);
    fputs("  -s, --set=STRING\n", stream);
    fputs("         set time described by STRING\n", stream);
    fputs("  -u, --utc, --universal\n", stream);
    fputs("         print or set Coordinated Universal Time (UTC)\n", stream);
    fputs("      --help\n", stream);
    fputs("         display this help and exit\n", stream);
    fputs("      --version\n", stream);
    fputs("         output version information and exit\n", stream);
    fputs("\n", stream);
    fputs("All options that specify the date to display are mutually exclusive.\n", stream);
    fputs("I.e.: --date, --file, --reference, --resolution.\n", stream);
    fputs("\n", stream);
    fputs("FORMAT controls the output.  Interpreted sequences are:\n", stream);
    fputs("\n", stream);
    fputs("  %%   a literal %\n", stream);
    fputs("  %a   locale's abbreviated weekday name (e.g., Sun)\n", stream);
    fputs("  %A   locale's full weekday name (e.g., Sunday)\n", stream);
    fputs("  %b   locale's abbreviated month name (e.g., Jan)\n", stream);
    fputs("  %B   locale's full month name (e.g., January)\n", stream);
    fputs("  %c   locale's date and time (e.g., Thu Mar  3 23:05:25 2005)\n", stream);
    fputs("  %C   century; like %Y, except omit last two digits (e.g., 20)\n", stream);
    fputs("  %d   day of month (e.g., 01)\n", stream);
    fputs("  %D   date (ambiguous); same as %m/%d/%y\n", stream);
    fputs("  %e   day of month, space padded; same as %_d\n", stream);
    fputs("  %F   full date; like %+4Y-%m-%d\n", stream);
    fputs("  %g   last two digits of year of ISO week number (ambiguous; 00-99); see %G\n", stream);
    fputs("  %G   year of ISO week number; normally useful only with %V\n", stream);
    fputs("  %h   same as %b\n", stream);
    fputs("  %H   hour (00..23)\n", stream);
    fputs("  %I   hour (01..12)\n", stream);
    fputs("  %j   day of year (001..366)\n", stream);
    fputs("  %k   hour, space padded ( 0..23); same as %_H\n", stream);
    fputs("  %l   hour, space padded ( 1..12); same as %_I\n", stream);
    fputs("  %m   month (01..12)\n", stream);
    fputs("  %M   minute (00..59)\n", stream);
    fputs("  %n   a newline\n", stream);
    fputs("  %N   nanoseconds (000000000..999999999)\n", stream);
    fputs("  %p   locale's equivalent of either AM or PM; blank if not known\n", stream);
    fputs("  %P   like %p, but lower case\n", stream);
    fputs("  %q   quarter of year (1..4)\n", stream);
    fputs("  %r   locale's 12-hour clock time (e.g., 11:11:04 PM)\n", stream);
    fputs("  %R   24-hour hour and minute; same as %H:%M\n", stream);
    fputs("  %s   seconds since the Epoch (1970-01-01 00:00 UTC)\n", stream);
    fputs("  %S   second (00..60)\n", stream);
    fputs("  %t   a tab\n", stream);
    fputs("  %T   time; same as %H:%M:%S\n", stream);
    fputs("  %u   day of week (1..7); 1 is Monday\n", stream);
    fputs("  %U   week number of year, with Sunday as first day of week (00..53)\n", stream);
    fputs("  %V   ISO week number, with Monday as first day of week (01..53)\n", stream);
    fputs("  %w   day of week (0..6); 0 is Sunday\n", stream);
    fputs("  %W   week number of year, with Monday as first day of week (00..53)\n", stream);
    fputs("  %x   locale's date (can be ambiguous; e.g., 12/31/99)\n", stream);
    fputs("  %X   locale's time representation (e.g., 23:13:48)\n", stream);
    fputs("  %y   last two digits of year (ambiguous; 00..99)\n", stream);
    fputs("  %Y   year\n", stream);
    fputs("  %z   +hhmm numeric time zone (e.g., -0400)\n", stream);
    fputs("  %:z  +hh:mm numeric time zone (e.g., -04:00)\n", stream);
    fputs("  %::z  +hh:mm:ss numeric time zone (e.g., -04:00:00)\n", stream);
    fputs("  %:::z  numeric time zone with : to necessary precision (e.g., -04, +05:30)\n", stream);
    fputs("  %Z   alphabetic time zone abbreviation (e.g., EDT)\n", stream);
    fputs("\n", stream);
    fputs("By default, date pads numeric fields with zeroes.\n", stream);
    fputs("The following optional flags may follow '%':\n", stream);
    fputs("\n", stream);
    fputs("  -  (hyphen) do not pad the field\n", stream);
    fputs("  _  (underscore) pad with spaces\n", stream);
    fputs("  0  (zero) pad with zeros\n", stream);
    fputs("  +  pad with zeros, and put '+' before future years with >4 digits\n", stream);
    fputs("  ^  use upper case if possible\n", stream);
    fputs("  #  use opposite case if possible\n", stream);
    fputs("\n", stream);
    fputs("After any flags comes an optional field width, as a decimal number;\n", stream);
    fputs("then an optional modifier, which is either\n", stream);
    fputs("E to use the locale's alternate representations if available, or\n", stream);
    fputs("O to use the locale's alternate numeric symbols if available.\n", stream);
    fputs("\n", stream);
    fputs("Examples:\n", stream);
    fputs("Convert seconds since the Epoch (1970-01-01 UTC) to a date\n", stream);
    fputs("  $ date --date='@2147483647'\n", stream);
    fputs("\n", stream);
    fputs("Show the time on the west coast of the US (use tzselect(1) to find TZ)\n", stream);
    fputs("  $ TZ='America/Los_Angeles' date\n", stream);
    fputs("\n", stream);
    fputs("Show the local time for 9AM next Friday on the west coast of the US\n", stream);
    fputs("  $ date --date='TZ=\"America/Los_Angeles\" 09:00 next Fri'\n", stream);
    fputs("\n", stream);

}

static void bx_date_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

static bool bx_date_normalize_precision(const char* arg,
                                        const char* const* allowed,
                                        size_t allowed_count,
                                        const char* option_name,
                                        const char** normalized_out,
                                        struct bx_diag_ctx* diag) {
    size_t arg_len = strlen(arg);
    size_t match_count = 0;
    const char* match = NULL;

    for (size_t i = 0; i < allowed_count; i++) {
        if (strncmp(allowed[i], arg, arg_len) == 0) {
            match = allowed[i];
            match_count++;
        }
    }

    if (match_count == 1) {
        *normalized_out = match;
        return true;
    }

    if (match_count > 1) {
        bx_diag(diag, "ambiguous argument '%s' for '%s'", arg, option_name);
    }
    else {
        bx_diag(diag, "invalid argument '%s' for '%s'", arg, option_name);
    }

    return false;
}

static bool bx_date_parse_options(int argc, char** argv, struct bx_date_options* options, int* first_operand, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"date", required_argument, NULL, 'd'},
        {"debug", no_argument, NULL, 5},
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
    options->progname = bx_date_progname((argc > 0) ? argv[0] : NULL);
    diag->progname = options->progname;

    opterr = 0;
    optind = 1;

    while (true) {
        int option_index = 0;
        int c = getopt_long(argc, argv, "d:f:I::Rr:s:u", long_options, &option_index);
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
                options->rfc_email = false;
                options->rfc_3339 = false;
                break;
            case 'R':
                options->rfc_email = true;
                options->iso_8601_fmt = NULL;
                options->rfc_3339 = false;
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
                options->iso_8601_fmt = NULL;
                options->rfc_email = false;
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
            case 5:
                options->debug = true;
                break;
            case '?':
                if (optopt != 0) {
                    bx_diag(diag, "invalid option -- '%c'", optopt);
                }
                else if (optind > 0 && optind <= argc && argv[optind - 1] != NULL) {
                    bx_diag(diag, "unrecognized option '%s'", argv[optind - 1]);
                }
                else {
                    bx_diag(diag, "invalid option");
                }
                return false;
            default:
                return false;
        }
    }

    if (options->iso_8601_fmt != NULL) {
        static const char* const iso_8601_precisions[] = {"hours", "minutes", "date", "seconds", "ns"};
        const char* normalized = NULL;
        if (!bx_date_normalize_precision(options->iso_8601_fmt, iso_8601_precisions, sizeof(iso_8601_precisions) / sizeof(iso_8601_precisions[0]), "--iso-8601", &normalized, diag)) {
            return false;
        }
        options->iso_8601_fmt = normalized;
    }

    if (options->rfc_3339) {
        static const char* const rfc_3339_precisions[] = {"date", "seconds", "ns"};
        const char* normalized = NULL;
        if (!bx_date_normalize_precision(options->rfc_3339_fmt, rfc_3339_precisions, sizeof(rfc_3339_precisions) / sizeof(rfc_3339_precisions[0]), "--rfc-3339", &normalized, diag)) {
            return false;
        }
        options->rfc_3339_fmt = normalized;
    }

    *first_operand = optind;
    return true;
}

static bool bx_date_validate_option_combinations(const struct bx_date_options* options, struct bx_diag_ctx* diag) {
    int display_source_count = 0;
    display_source_count += (options->date_str != NULL) ? 1 : 0;
    display_source_count += (options->file != NULL) ? 1 : 0;
    display_source_count += (options->reference_file != NULL) ? 1 : 0;
    display_source_count += options->resolution ? 1 : 0;

    if (display_source_count > 1) {
        bx_diag(diag, "options to specify dates for printing are mutually exclusive");
        return false;
    }

    if (options->debug && options->date_str == NULL) {
        bx_diag(diag, "option '--debug' requires option '--date'");
        return false;
    }

    if (options->set_str != NULL && display_source_count > 0) {
        bx_diag(diag, "option '--set' cannot be combined with --date, --file, --reference, or --resolution");
        return false;
    }

    return true;
}

static bool bx_date_parse_epoch_literal(const char* text, struct timespec* ts) {
    struct bx_time_epoch_parse_options options = {
        .allow_trailing_space = true,
        .normalize_negative_fraction = true,
    };
    return bx_time_parse_epoch_literal(text, &options, ts);
}

struct bx_date_parse_pattern {
    const char* format;
    bool has_date;
    bool has_time;
    bool has_seconds;
};

static bool bx_date_parse_using_pattern(const char* text, const struct bx_date_parse_pattern* pattern, bool utc, struct timespec* ts) {
    const char* input = text;
    while (isspace((unsigned char)*input)) {
        input++;
    }

    if (*input == '\0') {
        return false;
    }

    time_t now = time(NULL);
    struct tm now_tm;
    if ((utc ? gmtime_r(&now, &now_tm) : localtime_r(&now, &now_tm)) == NULL) {
        return false;
    }

    struct tm tm_value = now_tm;
    if (pattern->has_date && !pattern->has_time) {
        tm_value.tm_hour = 0;
        tm_value.tm_min = 0;
        tm_value.tm_sec = 0;
    }
    if (pattern->has_time && !pattern->has_seconds) {
        tm_value.tm_sec = 0;
    }

    tm_value.tm_isdst = -1;
    char* end = strptime(input, pattern->format, &tm_value);
    if (end == NULL) {
        return false;
    }

    while (isspace((unsigned char)*end)) {
        end++;
    }
    if (*end != '\0') {
        return false;
    }

    time_t seconds = utc ? timegm(&tm_value) : mktime(&tm_value);
    if (seconds == (time_t)-1) {
        return false;
    }

    ts->tv_sec = seconds;
    ts->tv_nsec = 0;
    return true;
}

static void bx_date_debug_annotation(const struct bx_date_options* options, const char* text, const struct timespec* ts) {
    if (!options->debug) {
        return;
    }

    struct tm tm_value;
    if ((options->utc ? gmtime_r(&ts->tv_sec, &tm_value) : localtime_r(&ts->tv_sec, &tm_value)) == NULL) {
        fprintf(stderr, "%s: debug: parsed date part: '%s'\n", options->progname, text);
        fprintf(stderr, "%s: debug: final: %lld.%09ld\n", options->progname, (long long)ts->tv_sec, ts->tv_nsec);
        return;
    }

    char timestamp_buf[128];
    if (strftime(timestamp_buf, sizeof(timestamp_buf), "%Y-%m-%d %H:%M:%S %z", &tm_value) == 0) {
        fprintf(stderr, "%s: debug: parsed date part: '%s'\n", options->progname, text);
        fprintf(stderr, "%s: debug: final: %lld.%09ld\n", options->progname, (long long)ts->tv_sec, ts->tv_nsec);
        return;
    }

    fprintf(stderr, "%s: debug: parsed date part: '%s'\n", options->progname, text);
    fprintf(stderr, "%s: debug: final: %s\n", options->progname, timestamp_buf);
}

static bool parse_date_string(const char* str, struct timespec* ts, const struct bx_date_options* options, struct bx_diag_ctx* diag) {
    static const struct bx_date_parse_pattern patterns[] = {
        {"%Y-%m-%d %H:%M:%S", true, true, true},
        {"%Y-%m-%dT%H:%M:%S", true, true, true},
        {"%Y-%m-%d %H:%M", true, true, false},
        {"%Y-%m-%dT%H:%M", true, true, false},
        {"%Y-%m-%d", true, false, false},
        {"%H:%M:%S", false, true, true},
        {"%H:%M", false, true, false},
    };

    const char* text = str;
    while (isspace((unsigned char)*text)) {
        text++;
    }

    if (*text == '\0') {
        bx_diag(diag, "invalid date '%s'", str);
        return false;
    }

    const char* text_end = text + strlen(text);
    while (text_end > text && isspace((unsigned char)text_end[-1])) {
        text_end--;
    }

    if ((size_t)(text_end - text) == 3 && strncmp(text, "now", 3) == 0) {
        if (clock_gettime(CLOCK_REALTIME, ts) != 0) {
            bx_diag(diag, "cannot get current time: %s", strerror(errno));
            return false;
        }
        bx_date_debug_annotation(options, str, ts);
        return true;
    }

    if (bx_date_parse_epoch_literal(text, ts)) {
        bx_date_debug_annotation(options, str, ts);
        return true;
    }

    for (size_t i = 0; i < sizeof(patterns) / sizeof(patterns[0]); i++) {
        if (bx_date_parse_using_pattern(text, &patterns[i], options->utc, ts)) {
            bx_date_debug_annotation(options, str, ts);
            return true;
        }
    }

    bx_diag(diag, "invalid date '%s'", str);
    return false;
}

static bool parse_legacy_set_time(const char* str, struct timespec* ts, bool utc) {
    size_t len = strlen(str);
    const char* dot = strchr(str, '.');
    size_t main_len = dot ? (size_t)(dot - str) : len;

    if (main_len != 8 && main_len != 10 && main_len != 12)
        return false;

    struct tm tm;
    time_t now = time(NULL);
    if ((utc ? gmtime_r(&now, &tm) : localtime_r(&now, &tm)) == NULL) {
        return false;
    }

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

    if (dot) {
        if (strlen(dot + 1) != 2 || sscanf(dot + 1, "%2d", &sec) != 1) {
            return false;
        }
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

static bool bx_date_append_string(char* out, size_t out_size, size_t* offset, const char* text) {
    size_t text_len = strlen(text);
    if (*offset + text_len + 1 > out_size) {
        return false;
    }
    memcpy(out + *offset, text, text_len);
    *offset += text_len;
    out[*offset] = '\0';
    return true;
}

static bool bx_date_format_custom(const char* format, const struct tm* tm_value, time_t epoch_seconds, long nanoseconds, char* out, size_t out_size, struct bx_diag_ctx* diag) {
    size_t offset = 0;
    out[0] = '\0';

    for (size_t i = 0; format[i] != '\0'; i++) {
        if (format[i] != '%') {
            char literal[2] = {format[i], '\0'};
            if (!bx_date_append_string(out, out_size, &offset, literal)) {
                bx_diag(diag, "format error");
                return false;
            }
            continue;
        }

        size_t spec_start = i;
        i++;
        if (format[i] == '\0') {
            bx_diag(diag, "format error");
            return false;
        }

        if (format[i] == '%') {
            if (!bx_date_append_string(out, out_size, &offset, "%")) {
                bx_diag(diag, "format error");
                return false;
            }
            continue;
        }

        size_t spec_end = i;
        while (strchr("-_0+^#", format[spec_end]) != NULL) {
            spec_end++;
        }
        size_t colon_count = 0;
        while (format[spec_end] == ':') {
            spec_end++;
            colon_count++;
        }
        while (isdigit((unsigned char)format[spec_end])) {
            spec_end++;
        }
        if (format[spec_end] == 'E' || format[spec_end] == 'O') {
            spec_end++;
        }
        if (format[spec_end] == '\0') {
            bx_diag(diag, "format error");
            return false;
        }

        char conversion = format[spec_end];
        if (conversion == 'z' && colon_count > 0) {
            char tz_basic[16];
            if (strftime(tz_basic, sizeof(tz_basic), "%z", tm_value) == 0 || strlen(tz_basic) != 5) {
                bx_diag(diag, "format error");
                return false;
            }

            char tz_buf[24];
            if (colon_count == 1) {
                snprintf(tz_buf, sizeof(tz_buf), "%c%c%c:%c%c", tz_basic[0], tz_basic[1], tz_basic[2], tz_basic[3], tz_basic[4]);
            }
            else if (colon_count == 2) {
                snprintf(tz_buf, sizeof(tz_buf), "%c%c%c:%c%c:00", tz_basic[0], tz_basic[1], tz_basic[2], tz_basic[3], tz_basic[4]);
            }
            else {
                if (tz_basic[3] == '0' && tz_basic[4] == '0') {
                    snprintf(tz_buf, sizeof(tz_buf), "%c%c%c", tz_basic[0], tz_basic[1], tz_basic[2]);
                }
                else {
                    snprintf(tz_buf, sizeof(tz_buf), "%c%c%c:%c%c", tz_basic[0], tz_basic[1], tz_basic[2], tz_basic[3], tz_basic[4]);
                }
            }

            if (!bx_date_append_string(out, out_size, &offset, tz_buf)) {
                bx_diag(diag, "format error");
                return false;
            }
            i = spec_end;
            continue;
        }

        if (conversion == 's') {
            char seconds_buf[64];
            snprintf(seconds_buf, sizeof(seconds_buf), "%lld", (long long)epoch_seconds);
            if (!bx_date_append_string(out, out_size, &offset, seconds_buf)) {
                bx_diag(diag, "format error");
                return false;
            }
            i = spec_end;
            continue;
        }

        if (conversion == 'q') {
            char quarter_buf[2];
            quarter_buf[0] = (char)('0' + (tm_value->tm_mon / 3) + 1);
            quarter_buf[1] = '\0';
            if (!bx_date_append_string(out, out_size, &offset, quarter_buf)) {
                bx_diag(diag, "format error");
                return false;
            }
            i = spec_end;
            continue;
        }

        if (conversion == 'k' || conversion == 'l') {
            int hour = tm_value->tm_hour;
            if (conversion == 'l') {
                hour %= 12;
                if (hour == 0) {
                    hour = 12;
                }
            }

            char hour_buf[8];
            snprintf(hour_buf, sizeof(hour_buf), "%2d", hour);
            if (!bx_date_append_string(out, out_size, &offset, hour_buf)) {
                bx_diag(diag, "format error");
                return false;
            }
            i = spec_end;
            continue;
        }

        if (conversion == 'P') {
            char ampm_buf[32];
            if (strftime(ampm_buf, sizeof(ampm_buf), "%p", tm_value) == 0) {
                ampm_buf[0] = '\0';
            }
            for (size_t idx = 0; ampm_buf[idx] != '\0'; idx++) {
                ampm_buf[idx] = (char)tolower((unsigned char)ampm_buf[idx]);
            }

            if (!bx_date_append_string(out, out_size, &offset, ampm_buf)) {
                bx_diag(diag, "format error");
                return false;
            }
            i = spec_end;
            continue;
        }

        if (conversion == 'N') {
            char nsec_buf[16];
            snprintf(nsec_buf, sizeof(nsec_buf), "%09ld", nanoseconds);
            if (!bx_date_append_string(out, out_size, &offset, nsec_buf)) {
                bx_diag(diag, "format error");
                return false;
            }
            i = spec_end;
            continue;
        }

        char spec[64];
        size_t spec_len = (spec_end - spec_start) + 1;
        if (spec_len >= sizeof(spec)) {
            bx_diag(diag, "format error");
            return false;
        }
        memcpy(spec, format + spec_start, spec_len);
        spec[spec_len] = '\0';

        char piece[256];
        size_t produced = strftime(piece, sizeof(piece), spec, tm_value);
        if (produced == 0 && spec[0] != '\0') {
            bx_diag(diag, "format error");
            return false;
        }
        if (!bx_date_append_string(out, out_size, &offset, piece)) {
            bx_diag(diag, "format error");
            return false;
        }

        i = spec_end;
    }

    return true;
}

static bool format_and_print(const struct timespec* ts, const struct bx_date_options* options, const char* format, struct bx_diag_ctx* diag) {
    struct tm tm;
    if ((options->utc ? gmtime_r(&ts->tv_sec, &tm) : localtime_r(&ts->tv_sec, &tm)) == NULL) {
        bx_diag(diag, "cannot convert time");
        return false;
    }

    if (options->rfc_3339) {
        char buf[128];
        char tz_buf[32];
        strftime(tz_buf, sizeof(tz_buf), "%z", &tm);
        if (strlen(tz_buf) == 5) {
            memmove(tz_buf + 4, tz_buf + 3, 3);
            tz_buf[3] = ':';
        }

        if (strcmp(options->rfc_3339_fmt, "date") == 0) {
            strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
            printf("%s\n", buf);
        }
        else if (strcmp(options->rfc_3339_fmt, "seconds") == 0) {
            strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
            printf("%s%s\n", buf, tz_buf);
        }
        else if (strcmp(options->rfc_3339_fmt, "ns") == 0) {
            strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
            printf("%s.%09ld%s\n", buf, ts->tv_nsec, tz_buf);
        }
        else {
            bx_diag(diag, "invalid RFC 3339 precision: '%s'", options->rfc_3339_fmt);
            return false;
        }
        return true;
    }

    if (options->iso_8601_fmt) {
        char buf[128];
        char tz_buf[32];
        strftime(tz_buf, sizeof(tz_buf), "%z", &tm);
        if (strlen(tz_buf) == 5) {
            memmove(tz_buf + 4, tz_buf + 3, 3);
            tz_buf[3] = ':';
        }

        if (strcmp(options->iso_8601_fmt, "date") == 0) {
            strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
            printf("%s\n", buf);
        }
        else if (strcmp(options->iso_8601_fmt, "hours") == 0) {
            strftime(buf, sizeof(buf), "%Y-%m-%dT%H", &tm);
            printf("%s%s\n", buf, tz_buf);
        }
        else if (strcmp(options->iso_8601_fmt, "minutes") == 0) {
            strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M", &tm);
            printf("%s%s\n", buf, tz_buf);
        }
        else if (strcmp(options->iso_8601_fmt, "seconds") == 0) {
            strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
            printf("%s%s\n", buf, tz_buf);
        }
        else if (strcmp(options->iso_8601_fmt, "ns") == 0) {
            strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
            printf("%s,%09ld%s\n", buf, ts->tv_nsec, tz_buf);
        }
        else {
            bx_diag(diag, "invalid ISO 8601 precision: '%s'", options->iso_8601_fmt);
            return false;
        }
        return true;
    }

    char buf[4096];
    if (!bx_date_format_custom(format, &tm, ts->tv_sec, ts->tv_nsec, buf, sizeof(buf), diag)) {
        return false;
    }
    printf("%s\n", buf);
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
    if (!bx_date_validate_option_combinations(&options, &diag)) {
        return 1;
    }

    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        bx_diag(&diag, "cannot get current time: %s", strerror(errno));
        return 1;
    }

    const char* set_operand = NULL;
    const char* format = "%a %b %e %H:%M:%S %Z %Y";
    int num_operands = argc - first_operand;
    bool format_from_operand = false;

    if (num_operands >= 1 && argv[first_operand][0] == '+') {
        format = argv[first_operand] + 1;
        format_from_operand = true;
        first_operand++;
        num_operands--;
    }

    if (num_operands >= 1) {
        set_operand = argv[first_operand];
        first_operand++;
        num_operands--;
    }

    if (num_operands > 0) {
        bx_diag(&diag, "extra operand '%s'", argv[first_operand]);
        return 1;
    }

    if (options.rfc_email && !format_from_operand) {
        format = "%a, %d %b %Y %H:%M:%S %z";
    }

    if (options.resolution && set_operand != NULL) {
        bx_diag(&diag, "extra operand '%s'", set_operand);
        return 1;
    }

    if ((options.date_str != NULL || options.file != NULL || options.reference_file != NULL || options.resolution) && set_operand != NULL) {
        bx_diag(&diag, "extra operand '%s'", set_operand);
        return 1;
    }

    if (options.set_str != NULL && set_operand != NULL) {
        bx_diag(&diag, "cannot specify date to set with both --set and an operand");
        return 1;
    }

    if (options.file) {
        FILE* f = strcmp(options.file, "-") == 0 ? stdin : fopen(options.file, "r");
        if (!f) {
            bx_diag(&diag, "%s: %s", options.file, strerror(errno));
            return 1;
        }
        char* line = NULL;
        size_t len = 0;
        while (getline(&line, &len, f) != -1) {
            char* nl = strchr(line, '\n');
            if (nl)
                *nl = '\0';
            if (parse_date_string(line, &ts, &options, &diag)) {
                format_and_print(&ts, &options, format, &diag);
            }
        }
        free(line);
        if (f != stdin)
            fclose(f);
        return diag.exit_status;
    }

    if (options.reference_file) {
        struct stat st;
        if (stat(options.reference_file, &st) != 0) {
            bx_diag(&diag, "%s: %s", options.reference_file, strerror(errno));
            return 1;
        }
        ts.tv_sec = st.st_mtime;
        ts.tv_nsec = st.st_mtim.tv_nsec;
    }
    else if (options.date_str) {
        if (!parse_date_string(options.date_str, &ts, &options, &diag))
            return 1;
    }
    else if (options.set_str) {
        if (!parse_date_string(options.set_str, &ts, &options, &diag))
            return 1;
        if (clock_settime(CLOCK_REALTIME, &ts) != 0) {
            bx_diag(&diag, "cannot set date: %s", strerror(errno));
            return 1;
        }
    }
    else if (set_operand != NULL) {
        if (!parse_legacy_set_time(set_operand, &ts, options.utc)) {
            bx_diag(&diag, "invalid date '%s'", set_operand);
            return 1;
        }
        if (clock_settime(CLOCK_REALTIME, &ts) != 0) {
            bx_diag(&diag, "cannot set date: %s", strerror(errno));
        }
    }

    if (options.resolution) {
        struct timespec res;
        if (clock_getres(CLOCK_REALTIME, &res) != 0) {
            bx_diag(&diag, "cannot get clock resolution: %s", strerror(errno));
            return 1;
        }
        printf("%ld.%09ld\n", (long)res.tv_sec, res.tv_nsec);
        return 0;
    }

    if (!format_and_print(&ts, &options, format, &diag)) {
        return 1;
    }

    return diag.exit_status;
}
