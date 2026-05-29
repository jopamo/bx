#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/klog.h>
#include <sys/sysinfo.h>
#include <time.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"
#include "bx/libbx.h"
#include "lib/cli_common.h"
#include "lib/size_parse.h"
#include "lib/args_common.h"

enum bx_dmesg_action {
    BX_DMESG_ACTION_READ = 0,
    BX_DMESG_ACTION_READ_CLEAR,
    BX_DMESG_ACTION_CLEAR,
    BX_DMESG_ACTION_CONSOLE_OFF,
    BX_DMESG_ACTION_CONSOLE_ON,
    BX_DMESG_ACTION_CONSOLE_LEVEL,
};

enum bx_dmesg_time_format {
    BX_DMESG_TIME_RAW = 0,
    BX_DMESG_TIME_NOTIME,
    BX_DMESG_TIME_CTIME,
};

struct bx_dmesg_options {
    const char* progname;
    bool show_help;
    bool show_version;
    bool raw;
    bool decode;
    bool force_syslog;
    enum bx_dmesg_action action;
    enum bx_dmesg_time_format time_format;
    size_t buffer_size;
    int console_level;
};

struct bx_dmesg_name {
    const char* name;
    const char* help;
};

static const struct bx_dmesg_name bx_dmesg_level_names[] = {
    {"emerg", "system is unusable"},
    {"alert", "action must be taken immediately"},
    {"crit", "critical conditions"},
    {"err", "error conditions"},
    {"warn", "warning conditions"},
    {"notice", "normal but significant condition"},
    {"info", "informational"},
    {"debug", "debug-level messages"},
};

static const struct bx_dmesg_name bx_dmesg_facility_names[] = {
    {"kern", "kernel messages"},
    {"user", "random user-level messages"},
    {"mail", "mail system"},
    {"daemon", "system daemons"},
    {"auth", "security/authorization messages"},
    {"syslog", "messages generated internally by syslogd"},
    {"lpr", "line printer subsystem"},
    {"news", "network news subsystem"},
    {"uucp", "UUCP subsystem"},
    {"cron", "clock daemon"},
    {"authpriv", "security/authorization messages (private)"},
    {"ftp", "FTP daemon"},
    {"res0", "reserved 0"},
    {"res1", "reserved 1"},
    {"res2", "reserved 2"},
    {"res3", "reserved 3"},
    {"local0", "local use 0"},
    {"local1", "local use 1"},
    {"local2", "local use 2"},
    {"local3", "local use 3"},
    {"local4", "local use 4"},
    {"local5", "local use 5"},
    {"local6", "local use 6"},
    {"local7", "local use 7"},
};

static void bx_dmesg_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage:\n");
    fprintf(stream, " %s [options]\n", progname);
    fprintf(stream, "\n");
    fprintf(stream, "Display or control the kernel ring buffer.\n");
    fprintf(stream, "\n");
    fprintf(stream, "Options:\n");
    fprintf(stream, " -C, --clear                 clear the kernel ring buffer\n");
    fprintf(stream, " -c, --read-clear            read and clear all messages\n");
    fprintf(stream, " -D, --console-off           disable printing messages to console\n");
    fprintf(stream, " -E, --console-on            enable printing messages to console\n");
    fprintf(stream, " -n, --console-level <level> set console log level\n");
    fprintf(stream, " -r, --raw                   print the raw message buffer\n");
    fprintf(stream, " -S, --syslog                use syslog(2) style kernel log access\n");
    fprintf(stream, " -s, --buffer-size <size>    buffer size to query the kernel ring buffer\n");
    fprintf(stream, " -t, --notime                do not show timestamps with messages\n");
    fprintf(stream, " -T, --ctime                 show human-readable timestamps\n");
    fprintf(stream, " -x, --decode                decode facility and level\n");
    fprintf(stream, "     --time-format <format>  one of: raw, notime, ctime\n");
    fprintf(stream, "\n");
    fprintf(stream, " -h, --help                  display this help\n");
    fprintf(stream, " -V, --version               display version\n");
}

static bool bx_dmesg_parse_size(const char* text, size_t* size_out) {
    uintmax_t value = 0;
    if (!bx_size_parse_uint(text, &value) || value == 0 ||
        value > (uintmax_t)INT_MAX) {
        return false;
    }

    *size_out = (size_t)value;
    return true;
}

static bool bx_dmesg_parse_console_level(const char* text, int* level_out) {
    if (text == NULL || text[0] == '\0') {
        return false;
    }

    if (isdigit((unsigned char)text[0])) {
        uintmax_t value = 0;
        if (!bx_size_parse_uint(text, &value) || value > 8u) {
            return false;
        }
        *level_out = (int)value;
        return true;
    }

    for (size_t i = 0; i < sizeof(bx_dmesg_level_names) / sizeof(bx_dmesg_level_names[0]); i++) {
        if (strcmp(text, bx_dmesg_level_names[i].name) == 0) {
            *level_out = (int)i;
            return true;
        }
    }

    return false;
}

static bool bx_dmesg_parse_time_format(const char* text, enum bx_dmesg_time_format* format_out) {
    if (strcmp(text, "raw") == 0) {
        *format_out = BX_DMESG_TIME_RAW;
        return true;
    }
    if (strcmp(text, "notime") == 0) {
        *format_out = BX_DMESG_TIME_NOTIME;
        return true;
    }
    if (strcmp(text, "ctime") == 0) {
        *format_out = BX_DMESG_TIME_CTIME;
        return true;
    }
    return false;
}

static bool bx_dmesg_parse_priority_prefix(const char** cursor, int* priority_out) {
    if (cursor == NULL || *cursor == NULL || priority_out == NULL) {
        return false;
    }

    const char* p = *cursor;
    if (p[0] != '<') {
        return false;
    }

    errno = 0;
    char* end = NULL;
    intmax_t priority = strtoimax(p + 1, &end, 10);
    if (errno != 0 || end == p + 1 || end == NULL || *end != '>' || priority < 0 || priority > INT_MAX) {
        return false;
    }

    *priority_out = (int)priority;
    *cursor = end + 1;
    return true;
}

static bool bx_dmesg_parse_options(int argc, char** argv, struct bx_dmesg_options* options, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"clear", no_argument, NULL, 'C'},
        {"read-clear", no_argument, NULL, 'c'},
        {"console-off", no_argument, NULL, 'D'},
        {"console-on", no_argument, NULL, 'E'},
        {"console-level", required_argument, NULL, 'n'},
        {"raw", no_argument, NULL, 'r'},
        {"syslog", no_argument, NULL, 'S'},
        {"buffer-size", required_argument, NULL, 's'},
        {"notime", no_argument, NULL, 't'},
        {"ctime", no_argument, NULL, 'T'},
        {"decode", no_argument, NULL, 'x'},
        {"time-format", required_argument, NULL, 1000},
        {"help", no_argument, NULL, 'h'},
        {"version", no_argument, NULL, 'V'},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "dmesg");
    options->action = BX_DMESG_ACTION_READ;
    options->time_format = BX_DMESG_TIME_RAW;
    diag->progname = options->progname;

    bx_args_getopt_reset();

    while (true) {
        int c = bx_args_getopt_long(argc, argv, "+CcDEn:rSs:TtxhV", long_options, NULL);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 'C':
                options->action = BX_DMESG_ACTION_CLEAR;
                break;
            case 'c':
                options->action = BX_DMESG_ACTION_READ_CLEAR;
                break;
            case 'D':
                options->action = BX_DMESG_ACTION_CONSOLE_OFF;
                break;
            case 'E':
                options->action = BX_DMESG_ACTION_CONSOLE_ON;
                break;
            case 'n':
                if (!bx_dmesg_parse_console_level(optarg, &options->console_level)) {
                    bx_diag(diag, "invalid console level '%s'", optarg);
                    return false;
                }
                options->action = BX_DMESG_ACTION_CONSOLE_LEVEL;
                break;
            case 'r':
                options->raw = true;
                break;
            case 'S':
                options->force_syslog = true;
                break;
            case 's':
                if (!bx_dmesg_parse_size(optarg, &options->buffer_size)) {
                    bx_diag(diag, "invalid buffer size '%s'", optarg);
                    return false;
                }
                break;
            case 't':
                options->time_format = BX_DMESG_TIME_NOTIME;
                break;
            case 'T':
                options->time_format = BX_DMESG_TIME_CTIME;
                break;
            case 'x':
                options->decode = true;
                break;
            case 1000:
                if (!bx_dmesg_parse_time_format(optarg, &options->time_format)) {
                    bx_diag(diag, "unsupported time format '%s'", optarg);
                    return false;
                }
                break;
            case 'h':
                options->show_help = true;
                return true;
            case 'V':
                options->show_version = true;
                return true;
            case '?':
                bx_cli_diag_unrecognized_option(diag, optopt, optind, argc, argv);
                return false;
            case ':':
                bx_cli_diag_option_requires_arg(diag, optopt, optind, argc, argv);
                return false;
            default:
                return false;
        }
    }

    if (optind < argc) {
        bx_cli_diag_extra_operand(diag, argv[optind]);
        return false;
    }

    return true;
}

static const char* bx_dmesg_level_name(unsigned level) {
    return level < sizeof(bx_dmesg_level_names) / sizeof(bx_dmesg_level_names[0]) ? bx_dmesg_level_names[level].name : "unknown";
}

static const char* bx_dmesg_facility_name(unsigned facility) {
    return facility < sizeof(bx_dmesg_facility_names) / sizeof(bx_dmesg_facility_names[0]) ? bx_dmesg_facility_names[facility].name : "unknown";
}

static bool bx_dmesg_format_ctime(double seconds_since_boot, char* out, size_t out_size) {
    struct sysinfo info;
    if (sysinfo(&info) != 0) {
        return false;
    }

    time_t now = time(NULL);
    time_t stamp = now - info.uptime + (time_t)seconds_since_boot;
    struct tm tm;
    if (localtime_r(&stamp, &tm) == NULL) {
        return false;
    }

    return strftime(out, out_size, "%a %b %e %H:%M:%S %Y", &tm) != 0;
}

static void bx_dmesg_print_line(const struct bx_dmesg_options* options, const char* line) {
    const char* p = line;
    int priority = -1;

    (void)bx_dmesg_parse_priority_prefix(&p, &priority);

    while (*p == ' ') {
        p++;
    }

    const char* message = p;
    double stamp = 0.0;
    bool has_stamp = false;
    if (*message == '[') {
        char* end = NULL;
        stamp = strtod(message + 1, &end);
        if (end != message + 1 && end != NULL && *end == ']') {
            has_stamp = true;
            message = end + 1;
            if (*message == ' ') {
                message++;
            }
        }
    }

    if (options->raw) {
        fputs(line, stdout);
        fputc('\n', stdout);
        return;
    }

    if (options->decode && priority >= 0) {
        unsigned facility = (unsigned)priority >> 3;
        unsigned level = (unsigned)priority & 7u;
        printf("%s.%s: ", bx_dmesg_facility_name(facility), bx_dmesg_level_name(level));
    }

    if (options->time_format == BX_DMESG_TIME_RAW && has_stamp) {
        const char* after_pri = p;
        fputs(after_pri, stdout);
        fputc('\n', stdout);
        return;
    }

    if (options->time_format == BX_DMESG_TIME_CTIME && has_stamp) {
        char ctime_buf[128];
        if (bx_dmesg_format_ctime(stamp, ctime_buf, sizeof(ctime_buf))) {
            printf("[%s] ", ctime_buf);
        }
    }

    fputs(message, stdout);
    fputc('\n', stdout);
}

static bool bx_dmesg_print_buffer(const struct bx_dmesg_options* options, struct bx_diag_ctx* diag) {
    int action = options->action == BX_DMESG_ACTION_READ_CLEAR ? 4 : 3;
    int size = options->buffer_size != 0 ? (int)options->buffer_size : klogctl(10, NULL, 0);
    if (size < 0) {
        bx_diag(diag, "failed to query kernel log buffer size: %s", strerror(errno));
        return false;
    }
    if (size == 0) {
        return true;
    }

    char* buffer = xmalloc((size_t)size + 1u);
    int read_size = klogctl(action, buffer, size);
    if (read_size < 0) {
        bx_diag(diag, "failed to read kernel log buffer: %s", strerror(errno));
        free(buffer);
        return false;
    }
    buffer[read_size] = '\0';

    char* saveptr = NULL;
    for (char* line = strtok_r(buffer, "\n", &saveptr); line != NULL; line = strtok_r(NULL, "\n", &saveptr)) {
        bx_dmesg_print_line(options, line);
    }

    free(buffer);
    return true;
}

static bool bx_dmesg_run_control_action(const struct bx_dmesg_options* options, struct bx_diag_ctx* diag) {
    int rc = 0;
    switch (options->action) {
        case BX_DMESG_ACTION_CLEAR:
            rc = klogctl(5, NULL, 0);
            break;
        case BX_DMESG_ACTION_CONSOLE_OFF:
            rc = klogctl(6, NULL, 0);
            break;
        case BX_DMESG_ACTION_CONSOLE_ON:
            rc = klogctl(7, NULL, 0);
            break;
        case BX_DMESG_ACTION_CONSOLE_LEVEL:
            rc = klogctl(8, NULL, options->console_level);
            break;
        case BX_DMESG_ACTION_READ:
        case BX_DMESG_ACTION_READ_CLEAR:
            return bx_dmesg_print_buffer(options, diag);
    }

    if (rc < 0) {
        bx_diag(diag, "dmesg control operation failed: %s", strerror(errno));
        return false;
    }
    return true;
}

int bx_dmesg_main(int argc, char** argv) {
    struct bx_dmesg_options options;
    struct bx_diag_ctx diag = {
        .progname = "dmesg",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };

    if (!bx_dmesg_parse_options(argc, argv, &options, &diag)) {
        bx_cli_print_try_help(options.progname != NULL ? options.progname : "dmesg");
        return 1;
    }

    if (options.show_help) {
        bx_dmesg_print_help(stdout, options.progname);
        return 0;
    }
    if (options.show_version) {
        bx_cli_print_version(options.progname);
        return 0;
    }

    if (!bx_dmesg_run_control_action(&options, &diag)) {
        return 1;
    }
    return 0;
}
