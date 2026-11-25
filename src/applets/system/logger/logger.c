#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"
#include "lib/cli_common.h"
#include "lib/fd_ops.h"
#include "lib/option_scan.h"
#include "lib/size_parse.h"

#define BX_LOGGER_MAX_MESSAGE 8192u
#define BX_LOGGER_MAX_TAG 64u
#define BX_LOGGER_DEFAULT_SOCKET "/dev/log"
#define BX_LOGGER_FALLBACK_SOCKET "/run/systemd/journal/dev-log"

struct bx_logger_options {
    const char* socket_path;
    char tag[BX_LOGGER_MAX_TAG];
    int priority;
    bool socket_explicit;
    bool show_help;
    bool show_version;
};

struct bx_logger_name_value {
    const char* name;
    int value;
};

static const struct bx_logger_name_value bx_logger_facilities[] = {
    {"kern", 0}, {"user", 1}, {"mail", 2}, {"daemon", 3}, {"auth", 4},
    {"syslog", 5}, {"lpr", 6}, {"news", 7}, {"uucp", 8}, {"cron", 9},
    {"authpriv", 10}, {"ftp", 11}, {"local0", 16}, {"local1", 17},
    {"local2", 18}, {"local3", 19}, {"local4", 20}, {"local5", 21},
    {"local6", 22}, {"local7", 23},
};

static const struct bx_logger_name_value bx_logger_levels[] = {
    {"emerg", 0}, {"alert", 1}, {"crit", 2}, {"err", 3}, {"error", 3},
    {"warning", 4}, {"warn", 4}, {"notice", 5}, {"info", 6}, {"debug", 7},
};

static void bx_logger_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... [MESSAGE]\n", progname);
    fprintf(stream, "Write MESSAGE, or standard input lines, to the local syslog socket.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -p, --priority PRIORITY  facility.level or numeric priority (default user.notice)\n");
    fprintf(stream, "  -t, --tag TAG            prefix every message with TAG\n");
    fprintf(stream, "  -u, --socket PATH        use PATH instead of /dev/log\n");
    fprintf(stream, "      --help               display this help and exit\n");
    fprintf(stream, "      --version            output version information and exit\n");
}

static bool bx_logger_lookup(const struct bx_logger_name_value* table, size_t count,
                             const char* name, size_t name_len, int* value_out) {
    for (size_t i = 0; i < count; i++) {
        if (strlen(table[i].name) == name_len &&
            strncasecmp(table[i].name, name, name_len) == 0) {
            *value_out = table[i].value;
            return true;
        }
    }
    return false;
}

static bool bx_logger_parse_priority(const char* text, int* priority_out) {
    if (text == NULL || text[0] == '\0') {
        return false;
    }

    bool numeric = true;
    for (const char* cursor = text; *cursor != '\0'; cursor++) {
        if (!isdigit((unsigned char)*cursor)) {
            numeric = false;
            break;
        }
    }
    if (numeric) {
        uintmax_t value;
        if (!bx_size_parse_uint(text, &value) || value > 191u) {
            return false;
        }
        *priority_out = (int)value;
        return true;
    }

    const char* dot = strchr(text, '.');
    int facility = 1;
    int level = 5;
    if (dot == NULL) {
        if (bx_logger_lookup(bx_logger_facilities,
                             sizeof(bx_logger_facilities) / sizeof(bx_logger_facilities[0]),
                             text, strlen(text), &facility)) {
            *priority_out = facility * 8 + level;
            return true;
        }
        if (bx_logger_lookup(bx_logger_levels,
                             sizeof(bx_logger_levels) / sizeof(bx_logger_levels[0]),
                             text, strlen(text), &level)) {
            *priority_out = facility * 8 + level;
            return true;
        }
        return false;
    }

    if (dot == text || dot[1] == '\0' || strchr(dot + 1, '.') != NULL) {
        return false;
    }
    if (!bx_logger_lookup(bx_logger_facilities,
                          sizeof(bx_logger_facilities) / sizeof(bx_logger_facilities[0]),
                          text, (size_t)(dot - text), &facility)) {
        return false;
    }
    if (!bx_logger_lookup(bx_logger_levels,
                          sizeof(bx_logger_levels) / sizeof(bx_logger_levels[0]),
                          dot + 1, strlen(dot + 1), &level)) {
        return false;
    }
    *priority_out = facility * 8 + level;
    return true;
}

static void bx_logger_set_tag(struct bx_logger_options* options, const char* tag) {
    size_t length = strlen(tag);
    if (length >= sizeof(options->tag)) {
        length = sizeof(options->tag) - 1u;
    }
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)tag[i];
        options->tag[i] = (c <= 0x20u || c == 0x7fu || c == ':') ? '_' : (char)c;
    }
    options->tag[length] = '\0';
    if (length == 0u) {
        snprintf(options->tag, sizeof(options->tag), "logger");
    }
}

static bool bx_logger_parse_options(int argc, char** argv, struct bx_logger_options* options,
                                    size_t* first_message, struct bx_diag_ctx* diag) {
    enum {
        OPT_HELP = 1000,
        OPT_VERSION,
    };
    static const struct bx_option_spec specs[] = {
        {'p', 'p', "priority", BX_OPTION_REQUIRED_VALUE},
        {'t', 't', "tag", BX_OPTION_REQUIRED_VALUE},
        {'u', 'u', "socket", BX_OPTION_REQUIRED_VALUE},
        {OPT_HELP, 0, "help", BX_OPTION_NO_VALUE},
        {OPT_VERSION, 0, "version", BX_OPTION_NO_VALUE},
    };

    memset(options, 0, sizeof(*options));
    options->socket_path = BX_LOGGER_DEFAULT_SOCKET;
    options->priority = 1 * 8 + 5;
    bx_logger_set_tag(options, diag->progname);

    struct bx_option_cursor cursor;
    bx_option_cursor_init(&cursor, argv, argc > 0 ? 1u : 0u, (size_t)argc,
                          BX_OPTION_PREFIX | BX_OPTION_ABBREVIATE, specs, sizeof(specs) / sizeof(specs[0]));
    struct bx_option_result option;
    while ((option = bx_option_next(&cursor)).event != BX_OPTION_END) {
        if (option.event != BX_OPTION_MATCH) {
            if (option.event == BX_OPTION_MISSING_VALUE && option.short_name)
                bx_diag(diag, "option requires an argument -- '%c'", option.short_name);
            else if (option.event == BX_OPTION_MISSING_VALUE)
                bx_diag(diag, "option '%s' requires an argument", option.token);
            else if (option.short_name)
                bx_diag(diag, "invalid option -- '%c'", option.short_name);
            else
                bx_diag(diag, "unrecognized option '%s'", option.token);
            return false;
        }
        switch (option.spec->id) {
            case 'p':
                if (!bx_logger_parse_priority(option.value, &options->priority)) {
                    bx_diag(diag, "invalid priority '%s'", option.value);
                    return false;
                }
                break;
            case 't':
                bx_logger_set_tag(options, option.value);
                break;
            case 'u':
                if (option.value[0] == '\0') {
                    bx_diag(diag, "empty socket path");
                    return false;
                }
                options->socket_path = option.value;
                options->socket_explicit = true;
                break;
            case OPT_HELP:
                options->show_help = true;
                return true;
            case OPT_VERSION:
                options->show_version = true;
                return true;
        }
    }

    *first_message = option.index;
    return true;
}

static int bx_logger_open_socket(const char* path) {
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(address.sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(address.sun_path, path, strlen(path) + 1u);

    int fd = bx_fd_socket_cloexec(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }
    if (connect(fd, (const struct sockaddr*)&address, sizeof(address)) != 0) {
        int saved_errno = errno;
        bx_fd_cleanup(&fd);
        errno = saved_errno;
        return -1;
    }
    return fd;
}

static int bx_logger_connect(struct bx_logger_options* options, const char** path_out,
                             struct bx_diag_ctx* diag) {
    const char* path = options->socket_path;
    int fd = bx_logger_open_socket(path);
    if (fd >= 0) {
        *path_out = path;
        return fd;
    }
    if (!options->socket_explicit && errno == ENOENT) {
        path = BX_LOGGER_FALLBACK_SOCKET;
        fd = bx_logger_open_socket(path);
        if (fd >= 0) {
            *path_out = path;
            return fd;
        }
    }

    bx_diag(diag, "cannot connect to %s: %s", path, strerror(errno));
    return -1;
}

static bool bx_logger_send(int fd, const char* path, int priority, const char* tag,
                           const char* message, struct bx_diag_ctx* diag) {
    char timestamp[32];
    time_t now = time(NULL);
    struct tm local;
    if (localtime_r(&now, &local) == NULL ||
        strftime(timestamp, sizeof(timestamp), "%b %e %H:%M:%S", &local) == 0u) {
        snprintf(timestamp, sizeof(timestamp), "Jan  1 00:00:00");
    }

    char packet[BX_LOGGER_MAX_MESSAGE + 128u];
    int length = snprintf(packet, sizeof(packet), "<%d>%s %s: %s\n", priority, timestamp,
                          tag, message);
    if (length < 0) {
        bx_diag(diag, "cannot format syslog message");
        return false;
    }
    if ((size_t)length >= sizeof(packet)) {
        length = (int)sizeof(packet) - 1;
    }

    ssize_t sent = send(fd, packet, (size_t)length, MSG_NOSIGNAL);
    if (sent < 0) {
        bx_diag(diag, "cannot send to %s: %s", path, strerror(errno));
        return false;
    }
    if ((size_t)sent != (size_t)length) {
        bx_diag(diag, "short send to %s", path);
        return false;
    }
    return true;
}

static bool bx_logger_append(char* buffer, size_t* length, const char* text) {
    if (*length > 0u && *length + 1u < BX_LOGGER_MAX_MESSAGE) {
        buffer[(*length)++] = ' ';
    }
    size_t room = BX_LOGGER_MAX_MESSAGE - *length;
    size_t text_length = strnlen(text, room);
    if (text_length == room)
        return false;
    memcpy(buffer + *length, text, text_length);
    *length += text_length;
    buffer[*length] = '\0';
    return true;
}

static bool bx_logger_read_line(FILE* stream, char* buffer, size_t buffer_size, bool* eof_out) {
    if (fgets(buffer, (int)buffer_size, stream) == NULL) {
        *eof_out = true;
        return false;
    }
    *eof_out = false;
    size_t length = strlen(buffer);
    if (length > 0u && buffer[length - 1u] == '\n') {
        buffer[--length] = '\0';
    }
    else if (!feof(stream)) {
        int c;
        while ((c = fgetc(stream)) != EOF && c != '\n') {
        }
    }
    return true;
}

int bx_logger_main(int argc, char** argv) {
    struct bx_logger_options options;
    struct bx_diag_ctx diag = {
        .progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "logger"),
    };
    size_t first_message = (size_t)argc;

    if (!bx_logger_parse_options(argc, argv, &options, &first_message, &diag)) {
        bx_cli_print_try_help(diag.progname);
        return diag.exit_status;
    }
    if (options.show_help) {
        bx_logger_print_help(stdout, diag.progname);
        return 0;
    }
    if (options.show_version) {
        bx_cli_print_version(diag.progname);
        return 0;
    }

    bool has_message = first_message < (size_t)argc;
    char message[BX_LOGGER_MAX_MESSAGE];
    if (has_message) {
        size_t length = 0;
        message[0] = '\0';
        for (size_t i = first_message; i < (size_t)argc; i++) {
            if (!bx_logger_append(message, &length, argv[i])) {
                bx_diag(&diag, "message is too long");
                return diag.exit_status;
            }
        }
    }

    const char* path = NULL;
    int fd = bx_logger_connect(&options, &path, &diag);
    if (fd < 0) {
        return (diag.exit_status != 0) ? diag.exit_status : 1;
    }

    bool ok = true;
    if (has_message) {
        ok = bx_logger_send(fd, path, options.priority, options.tag, message, &diag);
    }
    else {
        char line[BX_LOGGER_MAX_MESSAGE];
        bool eof = false;
        while (bx_logger_read_line(stdin, line, sizeof(line), &eof)) {
            if (!bx_logger_send(fd, path, options.priority, options.tag, line, &diag)) {
                ok = false;
                break;
            }
        }
        if (ferror(stdin)) {
            bx_diag(&diag, "cannot read standard input: %s", strerror(errno));
            ok = false;
        }
        (void)eof;
    }

    int saved_errno = errno;
    bx_fd_cleanup(&fd);
    errno = saved_errno;
    if (!ok && diag.exit_status == 0) {
        diag.exit_status = 1;
    }
    return ok ? 0 : (diag.exit_status != 0 ? diag.exit_status : 1);
}
