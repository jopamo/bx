#define _GNU_SOURCE

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"
#include "lib/cli_common.h"

#ifndef SYS_pidfd_open
#if defined(__x86_64__)
#define SYS_pidfd_open 434
#elif defined(__aarch64__)
#define SYS_pidfd_open 434
#endif
#endif

#ifndef SYS_pidfd_send_signal
#if defined(__x86_64__)
#define SYS_pidfd_send_signal 424
#elif defined(__aarch64__)
#define SYS_pidfd_send_signal 424
#endif
#endif

enum bx_kill_action {
    BX_KILL_ACTION_SEND = 0,
    BX_KILL_ACTION_LIST,
    BX_KILL_ACTION_TABLE,
    BX_KILL_ACTION_PID,
    BX_KILL_ACTION_SHOW_PROCESS_STATE,
};

struct bx_kill_timeout_step {
    int milliseconds;
    int signal_number;
};

struct bx_kill_options {
    const char* progname;
    bool show_help;
    bool show_version;
    enum bx_kill_action action;
    bool all_uids;
    bool verbose;
    bool require_handler;
    bool queue_set;
    int queue_value;
    bool signal_explicit;
    int signal_number;
    const char* list_query;
    const char* show_state_pid;
    struct bx_kill_timeout_step* timeouts;
    size_t timeout_count;
    size_t timeout_capacity;
    int first_operand_index;
};

struct bx_kill_signal_alias {
    const char* name;
    int value;
    bool primary;
};

struct bx_kill_uint_list {
    uintmax_t* items;
    size_t len;
    size_t cap;
};

struct bx_kill_outcome {
    size_t successes;
    size_t failures;
};

static const struct bx_kill_signal_alias bx_kill_signal_aliases[] = {
    {"HUP", SIGHUP, true},
    {"INT", SIGINT, true},
    {"QUIT", SIGQUIT, true},
    {"ILL", SIGILL, true},
#ifdef SIGTRAP
    {"TRAP", SIGTRAP, true},
#endif
    {"ABRT", SIGABRT, true},
#ifdef SIGIOT
    {"IOT", SIGIOT, false},
#endif
#ifdef SIGBUS
    {"BUS", SIGBUS, true},
#endif
    {"FPE", SIGFPE, true},
    {"KILL", SIGKILL, true},
    {"USR1", SIGUSR1, true},
    {"SEGV", SIGSEGV, true},
    {"USR2", SIGUSR2, true},
    {"PIPE", SIGPIPE, true},
    {"ALRM", SIGALRM, true},
    {"TERM", SIGTERM, true},
#ifdef SIGSTKFLT
    {"STKFLT", SIGSTKFLT, true},
#endif
    {"CHLD", SIGCHLD, true},
    {"CONT", SIGCONT, true},
    {"STOP", SIGSTOP, true},
    {"TSTP", SIGTSTP, true},
    {"TTIN", SIGTTIN, true},
    {"TTOU", SIGTTOU, true},
#ifdef SIGURG
    {"URG", SIGURG, true},
#endif
#ifdef SIGXCPU
    {"XCPU", SIGXCPU, true},
#endif
#ifdef SIGXFSZ
    {"XFSZ", SIGXFSZ, true},
#endif
#ifdef SIGVTALRM
    {"VTALRM", SIGVTALRM, true},
#endif
#ifdef SIGPROF
    {"PROF", SIGPROF, true},
#endif
#ifdef SIGWINCH
    {"WINCH", SIGWINCH, true},
#endif
#ifdef SIGIO
    {"IO", SIGIO, true},
#endif
#ifdef SIGPOLL
    {"POLL", SIGPOLL, false},
#endif
#ifdef SIGPWR
    {"PWR", SIGPWR, true},
#endif
#ifdef SIGSYS
    {"UNUSED", SIGSYS, true},
    {"SYS", SIGSYS, false},
#endif
};

static void bx_kill_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [options] pid|name...\n", progname);
    fprintf(stream, "Send signals to processes or process groups.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -s, --signal SIGNAL          send SIGNAL instead of TERM\n");
    fprintf(stream, "  -l, --list [ARG]             list signals or decode number/mask\n");
    fprintf(stream, "  -L, --table                  list signals with numbers\n");
    fprintf(stream, "  -p, --pid                    print matching PIDs without signaling\n");
    fprintf(stream, "  -a, --all                    match names across all UIDs\n");
    fprintf(stream, "  -r, --require-handler        skip targets with no userspace handler\n");
    fprintf(stream, "  -q, --queue VALUE            use sigqueue(3) with VALUE\n");
    fprintf(stream, "      --timeout MS SIGNAL      send follow-up SIGNAL after MS delay\n");
    fprintf(stream, "  -d, --show-process-state PID decode signal masks from /proc/PID/status\n");
    fprintf(stream, "      --verbose                print each target before signaling\n");
    fprintf(stream, "  -h, --help                   display this help and exit\n");
    fprintf(stream, "  -V, --version                output version information and exit\n");
}

static bool bx_kill_uint_list_append(struct bx_kill_uint_list* list, uintmax_t value) {
    if (list->len == list->cap) {
        size_t new_cap = list->cap == 0 ? 8 : list->cap * 2;
        uintmax_t* new_items = realloc(list->items, new_cap * sizeof(*new_items));
        if (new_items == NULL) {
            return false;
        }
        list->items = new_items;
        list->cap = new_cap;
    }

    list->items[list->len++] = value;
    return true;
}

static int bx_kill_compare_uintmax(const void* a, const void* b) {
    uintmax_t aa = *(const uintmax_t*)a;
    uintmax_t bb = *(const uintmax_t*)b;
    if (aa < bb) {
        return -1;
    }
    if (aa > bb) {
        return 1;
    }
    return 0;
}

static bool bx_kill_is_valid_signal_number(int signal_number) {
    if (signal_number == 0) {
        return true;
    }
    if (signal_number < 0) {
        return false;
    }
    if (sigaction(signal_number, NULL, NULL) == 0) {
        return true;
    }
    return errno != EINVAL;
}

static bool bx_kill_parse_unsigned_decimal(const char* text, uintmax_t* value_out) {
    if (text == NULL || text[0] == '\0') {
        return false;
    }

    errno = 0;
    char* end = NULL;
    uintmax_t value = strtoumax(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        return false;
    }

    *value_out = value;
    return true;
}

static bool bx_kill_parse_signed_decimal(const char* text, intmax_t* value_out) {
    if (text == NULL || text[0] == '\0') {
        return false;
    }

    errno = 0;
    char* end = NULL;
    intmax_t value = strtoimax(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        return false;
    }

    *value_out = value;
    return true;
}

static bool bx_kill_parse_hex_mask(const char* text, uint64_t* mask_out) {
    if (text == NULL || strncasecmp(text, "0x", 2) != 0) {
        return false;
    }

    errno = 0;
    char* end = NULL;
    unsigned long long value = strtoull(text, &end, 16);
    if (errno != 0 || end == text || *end != '\0') {
        return false;
    }

    *mask_out = (uint64_t)value;
    return true;
}

static int bx_kill_signal_rtmin(void) {
#ifdef SIGRTMIN
    return SIGRTMIN;
#else
    return -1;
#endif
}

static int bx_kill_signal_rtmax(void) {
#ifdef SIGRTMAX
    return SIGRTMAX;
#else
    return -1;
#endif
}

static bool bx_kill_parse_rt_signal(const char* name, int* signal_out) {
    int rtmin = bx_kill_signal_rtmin();
    int rtmax = bx_kill_signal_rtmax();
    if (rtmin < 0 || rtmax < rtmin) {
        return false;
    }

    if (strcasecmp(name, "RTMIN") == 0) {
        *signal_out = rtmin;
        return true;
    }
    if (strcasecmp(name, "RTMAX") == 0) {
        *signal_out = rtmax;
        return true;
    }

    if (strncasecmp(name, "RTMIN+", 6) == 0) {
        uintmax_t offset = 0;
        if (!bx_kill_parse_unsigned_decimal(name + 6, &offset)) {
            return false;
        }
        if (offset > (uintmax_t)(rtmax - rtmin)) {
            return false;
        }
        *signal_out = rtmin + (int)offset;
        return true;
    }

    if (strncasecmp(name, "RTMAX-", 6) == 0) {
        uintmax_t offset = 0;
        if (!bx_kill_parse_unsigned_decimal(name + 6, &offset)) {
            return false;
        }
        if (offset > (uintmax_t)(rtmax - rtmin)) {
            return false;
        }
        *signal_out = rtmax - (int)offset;
        return true;
    }

    if (strncasecmp(name, "RT", 2) == 0 && isdigit((unsigned char)name[2])) {
        uintmax_t offset = 0;
        if (!bx_kill_parse_unsigned_decimal(name + 2, &offset)) {
            return false;
        }
        if (offset > (uintmax_t)(rtmax - rtmin)) {
            return false;
        }
        *signal_out = rtmin + (int)offset;
        return true;
    }

    return false;
}

static bool bx_kill_parse_signal(const char* text, int* signal_out) {
    if (text == NULL || text[0] == '\0') {
        return false;
    }

    uintmax_t number = 0;
    if (bx_kill_parse_unsigned_decimal(text, &number)) {
        if (number > INT32_MAX) {
            return false;
        }
        if (!bx_kill_is_valid_signal_number((int)number)) {
            return false;
        }
        *signal_out = (int)number;
        return true;
    }

    const char* name = text;
    if (strncasecmp(name, "SIG", 3) == 0) {
        name += 3;
    }

    if (bx_kill_parse_rt_signal(name, signal_out)) {
        return true;
    }

    for (size_t i = 0; i < sizeof(bx_kill_signal_aliases) / sizeof(bx_kill_signal_aliases[0]); i++) {
        if (strcasecmp(name, bx_kill_signal_aliases[i].name) == 0) {
            *signal_out = bx_kill_signal_aliases[i].value;
            return true;
        }
    }

    return false;
}

static const char* bx_kill_primary_signal_name(int signal_number) {
    for (size_t i = 0; i < sizeof(bx_kill_signal_aliases) / sizeof(bx_kill_signal_aliases[0]); i++) {
        if (bx_kill_signal_aliases[i].primary && bx_kill_signal_aliases[i].value == signal_number) {
            return bx_kill_signal_aliases[i].name;
        }
    }
    return NULL;
}

static const char* bx_kill_format_signal_name(int signal_number, char* buf, size_t buf_size) {
    const char* primary = bx_kill_primary_signal_name(signal_number);
    if (primary != NULL) {
        return primary;
    }

    int rtmin = bx_kill_signal_rtmin();
    int rtmax = bx_kill_signal_rtmax();
    if (rtmin >= 0 && rtmax >= rtmin && signal_number >= rtmin && signal_number <= rtmax) {
        snprintf(buf, buf_size, "RT%d", signal_number - rtmin);
        return buf;
    }

    snprintf(buf, buf_size, "%d", signal_number);
    return buf;
}

static bool bx_kill_parse_timeout_ms(const char* text, int* milliseconds_out) {
    uintmax_t value = 0;
    if (!bx_kill_parse_unsigned_decimal(text, &value) || value > INT32_MAX) {
        return false;
    }
    *milliseconds_out = (int)value;
    return true;
}

static bool bx_kill_add_timeout(struct bx_kill_options* options, int milliseconds, int signal_number) {
    if (options->timeout_count == options->timeout_capacity) {
        size_t new_capacity = options->timeout_capacity == 0 ? 4 : options->timeout_capacity * 2;
        struct bx_kill_timeout_step* new_timeouts = realloc(options->timeouts, new_capacity * sizeof(*new_timeouts));
        if (new_timeouts == NULL) {
            return false;
        }
        options->timeouts = new_timeouts;
        options->timeout_capacity = new_capacity;
    }

    options->timeouts[options->timeout_count].milliseconds = milliseconds;
    options->timeouts[options->timeout_count].signal_number = signal_number;
    options->timeout_count++;
    return true;
}

static bool bx_kill_looks_like_negative_pid_operand(const char* text) {
    if (text == NULL || text[0] != '-' || text[1] == '\0') {
        return false;
    }

    for (const char* p = text + 1; *p != '\0'; p++) {
        if (!isdigit((unsigned char)*p)) {
            return false;
        }
    }

    return true;
}

static bool bx_kill_parse_queue_value(const char* text, int* value_out) {
    intmax_t value = 0;
    if (!bx_kill_parse_signed_decimal(text, &value) || value < INT32_MIN || value > INT32_MAX) {
        return false;
    }
    *value_out = (int)value;
    return true;
}

static bool bx_kill_parse_pid_text(const char* text, pid_t* pid_out) {
    intmax_t value = 0;
    if (!bx_kill_parse_signed_decimal(text, &value)) {
        return false;
    }

    if (value < INT32_MIN || value > INT32_MAX) {
        return false;
    }

    *pid_out = (pid_t)value;
    return true;
}

static bool bx_kill_parse_pidfd_operand(const char* text, pid_t* pid_out, uintmax_t* inode_out) {
    const char* colon = strchr(text, ':');
    if (colon == NULL || colon == text || colon[1] == '\0' || strchr(colon + 1, ':') != NULL) {
        return false;
    }

    char pid_buf[64];
    size_t pid_len = (size_t)(colon - text);
    if (pid_len >= sizeof(pid_buf)) {
        return false;
    }
    memcpy(pid_buf, text, pid_len);
    pid_buf[pid_len] = '\0';

    pid_t pid = -1;
    if (!bx_kill_parse_pid_text(pid_buf, &pid) || pid <= 0) {
        return false;
    }

    uintmax_t inode = 0;
    if (!bx_kill_parse_unsigned_decimal(colon + 1, &inode) || inode == 0) {
        return false;
    }

    *pid_out = pid;
    *inode_out = inode;
    return true;
}

static const char* bx_kill_basename(const char* path) {
    if (path == NULL) {
        return "";
    }
    const char* slash = strrchr(path, '/');
    return slash != NULL ? slash + 1 : path;
}

static bool bx_kill_read_proc_text(const char* path, char* buf, size_t buf_size) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }

    ssize_t n = read(fd, buf, buf_size - 1);
    int saved_errno = errno;
    close(fd);
    errno = saved_errno;
    if (n < 0) {
        return false;
    }

    buf[n] = '\0';
    return true;
}

static bool bx_kill_read_status_mask(pid_t pid, const char* key, uint64_t* mask_out) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%ld/status", (long)pid);

    FILE* fp = fopen(path, "r");
    if (fp == NULL) {
        return false;
    }

    char* line = NULL;
    size_t linecap = 0;
    bool found = false;
    while (getline(&line, &linecap, fp) >= 0) {
        if (strncmp(line, key, strlen(key)) == 0) {
            char* value = line + strlen(key);
            while (*value == ' ' || *value == '\t') {
                value++;
            }
            errno = 0;
            char* end = NULL;
            unsigned long long parsed = strtoull(value, &end, 16);
            if (errno == 0 && end != value) {
                *mask_out = (uint64_t)parsed;
                found = true;
            }
            break;
        }
    }

    free(line);
    fclose(fp);
    if (!found) {
        errno = ESRCH;
    }
    return found;
}

static bool bx_kill_read_status_uid(pid_t pid, uid_t* uid_out) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%ld/status", (long)pid);

    FILE* fp = fopen(path, "r");
    if (fp == NULL) {
        return false;
    }

    char* line = NULL;
    size_t linecap = 0;
    bool found = false;
    while (getline(&line, &linecap, fp) >= 0) {
        if (strncmp(line, "Uid:", 4) == 0) {
            char* value = line + 4;
            while (*value == ' ' || *value == '\t') {
                value++;
            }
            errno = 0;
            char* end = NULL;
            unsigned long parsed = strtoul(value, &end, 10);
            if (errno == 0 && end != value) {
                *uid_out = (uid_t)parsed;
                found = true;
            }
            break;
        }
    }

    free(line);
    fclose(fp);
    if (!found) {
        errno = ESRCH;
    }
    return found;
}

static bool bx_kill_name_matches_pid(pid_t pid, const char* name) {
    char path[64];
    char buf[4096];

    snprintf(path, sizeof(path), "/proc/%ld/comm", (long)pid);
    if (bx_kill_read_proc_text(path, buf, sizeof(buf))) {
        size_t len = strlen(buf);
        while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
            buf[--len] = '\0';
        }
        if (strcmp(buf, name) == 0) {
            return true;
        }
    }

    snprintf(path, sizeof(path), "/proc/%ld/cmdline", (long)pid);
    if (bx_kill_read_proc_text(path, buf, sizeof(buf))) {
        if (buf[0] != '\0' && strcmp(bx_kill_basename(buf), name) == 0) {
            return true;
        }
    }

    return false;
}

static bool bx_kill_resolve_name_to_pids(const struct bx_kill_options* options, const char* name, struct bx_kill_uint_list* pids) {
    DIR* dir = opendir("/proc");
    if (dir == NULL) {
        return false;
    }

    uid_t self_uid = getuid();
    struct dirent* entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (!isdigit((unsigned char)entry->d_name[0])) {
            continue;
        }

        uintmax_t pid_value = 0;
        if (!bx_kill_parse_unsigned_decimal(entry->d_name, &pid_value) || pid_value == 0 || pid_value > INT32_MAX) {
            continue;
        }

        pid_t pid = (pid_t)pid_value;
        if (!bx_kill_name_matches_pid(pid, name)) {
            continue;
        }

        if (!options->all_uids) {
            uid_t target_uid = 0;
            if (!bx_kill_read_status_uid(pid, &target_uid)) {
                continue;
            }
            if (target_uid != self_uid) {
                continue;
            }
        }

        if (!bx_kill_uint_list_append(pids, pid_value)) {
            closedir(dir);
            errno = ENOMEM;
            return false;
        }
    }

    closedir(dir);
    if (pids->len > 1) {
        qsort(pids->items, pids->len, sizeof(*pids->items), bx_kill_compare_uintmax);
    }
    return true;
}

static void bx_kill_print_signal_list_names(FILE* stream, uint64_t mask) {
    bool first = true;
    for (int signal_number = 1; signal_number <= 64; signal_number++) {
        if ((mask & (UINT64_C(1) << (signal_number - 1))) == 0) {
            continue;
        }

        char name_buf[32];
        const char* name = bx_kill_format_signal_name(signal_number, name_buf, sizeof(name_buf));
        fprintf(stream, "%s%s", first ? "" : " ", name);
        first = false;
    }
    fputc('\n', stream);
}

static void bx_kill_list_all_signals(FILE* stream) {
    size_t column = 0;
    for (size_t i = 0; i < sizeof(bx_kill_signal_aliases) / sizeof(bx_kill_signal_aliases[0]); i++) {
        const char* name = bx_kill_signal_aliases[i].name;
        size_t len = strlen(name);
        if (column != 0 && column + 1 + len > 72) {
            fputc('\n', stream);
            column = 0;
        }
        if (column != 0) {
            fputc(' ', stream);
            column++;
        }
        fputs(name, stream);
        column += len;
    }

    if (bx_kill_signal_rtmin() >= 0) {
        const char* extras[] = {"RT<N>", "RTMIN+<N>", "RTMAX-<N>"};
        for (size_t i = 0; i < sizeof(extras) / sizeof(extras[0]); i++) {
            size_t len = strlen(extras[i]);
            if (column != 0 && column + 1 + len > 72) {
                fputc('\n', stream);
                column = 0;
            }
            if (column != 0) {
                fputc(' ', stream);
                column++;
            }
            fputs(extras[i], stream);
            column += len;
        }
    }

    fputc('\n', stream);
}

static void bx_kill_print_signal_table(FILE* stream) {
    for (size_t i = 0; i < sizeof(bx_kill_signal_aliases) / sizeof(bx_kill_signal_aliases[0]); i++) {
        fprintf(stream, "%2d %s\n", bx_kill_signal_aliases[i].value, bx_kill_signal_aliases[i].name);
    }

    int rtmin = bx_kill_signal_rtmin();
    int rtmax = bx_kill_signal_rtmax();
    if (rtmin >= 0) {
        fprintf(stream, "%2d RTMIN\n", rtmin);
        fprintf(stream, "%2d RTMAX\n", rtmax);
    }
}

static bool bx_kill_print_list_query(const char* query, struct bx_diag_ctx* diag) {
    if (query == NULL) {
        bx_kill_list_all_signals(stdout);
        return true;
    }

    uint64_t mask = 0;
    if (bx_kill_parse_hex_mask(query, &mask)) {
        if (mask != 0) {
            for (int signal_number = 1; signal_number <= 64; signal_number++) {
                if ((mask & (UINT64_C(1) << (signal_number - 1))) != 0) {
                    char name_buf[32];
                    puts(bx_kill_format_signal_name(signal_number, name_buf, sizeof(name_buf)));
                }
            }
        }
        return true;
    }

    int signal_number = 0;
    if (!bx_kill_parse_signal(query, &signal_number)) {
        bx_diag(diag, "unknown signal '%s'", query);
        return false;
    }

    char name_buf[32];
    puts(bx_kill_format_signal_name(signal_number, name_buf, sizeof(name_buf)));
    return true;
}

static bool bx_kill_print_show_process_state(const char* pid_text, struct bx_diag_ctx* diag) {
    pid_t pid = -1;
    if (!bx_kill_parse_pid_text(pid_text, &pid) || pid <= 0) {
        bx_diag(diag, "invalid PID '%s'", pid_text != NULL ? pid_text : "");
        return false;
    }

    struct {
        const char* label;
        const char* key;
    } fields[] = {
        {"Blocked", "SigBlk:"},
        {"Ignored", "SigIgn:"},
        {"Caught", "SigCgt:"},
    };

    bool ok = true;
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        uint64_t mask = 0;
        if (!bx_kill_read_status_mask(pid, fields[i].key, &mask)) {
            bx_diag(diag, "failed to read /proc/%ld/status: %s", (long)pid, strerror(errno));
            return false;
        }
        if (mask == 0) {
            continue;
        }
        printf("%s: ", fields[i].label);
        bx_kill_print_signal_list_names(stdout, mask);
        ok = true;
    }

    return ok;
}

static int bx_kill_pidfd_open(pid_t pid) {
#ifdef SYS_pidfd_open
    return (int)syscall(SYS_pidfd_open, pid, 0);
#else
    errno = ENOSYS;
    return -1;
#endif
}

static int bx_kill_pidfd_send_signal(int pidfd, int signal_number, siginfo_t* info) {
#ifdef SYS_pidfd_send_signal
    return (int)syscall(SYS_pidfd_send_signal, pidfd, signal_number, info, 0);
#else
    (void)pidfd;
    (void)signal_number;
    (void)info;
    errno = ENOSYS;
    return -1;
#endif
}

static bool bx_kill_target_has_handler(pid_t pid, int signal_number, bool* has_handler_out) {
    if (signal_number <= 0 || signal_number > 64 || signal_number == SIGKILL || signal_number == SIGSTOP) {
        *has_handler_out = false;
        return true;
    }

    uint64_t mask = 0;
    if (!bx_kill_read_status_mask(pid, "SigCgt:", &mask)) {
        return false;
    }

    *has_handler_out = (mask & (UINT64_C(1) << (signal_number - 1))) != 0;
    return true;
}

static void bx_kill_verbose_send(const struct bx_kill_options* options, const char* target_text, int signal_number) {
    if (!options->verbose) {
        return;
    }
    printf("sending signal %d to pid %s\n", signal_number, target_text);
}

static void bx_kill_verbose_skip_no_handler(const struct bx_kill_options* options, const char* target_text, int signal_number) {
    if (!options->verbose) {
        return;
    }
    printf("not signalling pid %s, it has no userspace handler for signal %d\n", target_text, signal_number);
}

static bool bx_kill_send_once(const struct bx_kill_options* options, pid_t pid, int pidfd, int signal_number) {
    if (options->queue_set) {
        union sigval value;
        value.sival_int = options->queue_value;

        if (pidfd >= 0) {
            siginfo_t info;
            memset(&info, 0, sizeof(info));
            info.si_signo = signal_number;
            info.si_code = SI_QUEUE;
            info.si_value = value;
            return bx_kill_pidfd_send_signal(pidfd, signal_number, &info) == 0;
        }

        return sigqueue(pid, signal_number, value) == 0;
    }

    if (pidfd >= 0) {
        return bx_kill_pidfd_send_signal(pidfd, signal_number, NULL) == 0;
    }

    return kill(pid, signal_number) == 0;
}

static int bx_kill_wait_pidfd_for_timeout(int pidfd, int milliseconds) {
    struct pollfd pfd = {
        .fd = pidfd,
        .events = POLLIN,
        .revents = 0,
    };

    struct timespec start;
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        return -1;
    }

    int remaining = milliseconds;
    for (;;) {
        int rc = poll(&pfd, 1, remaining);
        if (rc > 0) {
            return 0;
        }
        if (rc == 0) {
            return 1;
        }
        if (errno != EINTR) {
            return -1;
        }

        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
            return -1;
        }

        int64_t elapsed_ms = (now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000;
        if (elapsed_ms >= milliseconds) {
            return 1;
        }
        remaining = milliseconds - (int)elapsed_ms;
    }
}

static bool bx_kill_prepare_pidfd(pid_t pid, uintmax_t expected_inode, int* pidfd_out, struct bx_diag_ctx* diag) {
    int pidfd = bx_kill_pidfd_open(pid);
    if (pidfd < 0) {
        bx_diag(diag, "failed to open pidfd for %ld: %s", (long)pid, strerror(errno));
        return false;
    }

    if (expected_inode != 0) {
        struct stat st;
        if (fstat(pidfd, &st) != 0) {
            bx_diag(diag, "failed to inspect pidfd for %ld: %s", (long)pid, strerror(errno));
            close(pidfd);
            return false;
        }
        if ((uintmax_t)st.st_ino != expected_inode) {
            bx_diag(diag, "process %ld no longer matches pidfd inode %" PRIuMAX, (long)pid, expected_inode);
            close(pidfd);
            return false;
        }
    }

    *pidfd_out = pidfd;
    return true;
}

static bool bx_kill_send_target(
    const struct bx_kill_options* options,
    pid_t pid,
    const char* target_text,
    uintmax_t expected_pidfd_inode,
    struct bx_diag_ctx* diag
) {
    bool needs_pidfd = expected_pidfd_inode != 0 || options->timeout_count > 0;
    int pidfd = -1;

    if ((options->queue_set || options->timeout_count > 0 || options->require_handler || expected_pidfd_inode != 0) && pid <= 0) {
        bx_diag(diag, "option requires a positive PID target: %s", target_text);
        return false;
    }

    if (needs_pidfd) {
        if (!bx_kill_prepare_pidfd(pid, expected_pidfd_inode, &pidfd, diag)) {
            return false;
        }
    }

    if (options->require_handler) {
        bool has_handler = false;
        if (!bx_kill_target_has_handler(pid, options->signal_number, &has_handler)) {
            bx_diag(diag, "failed to inspect process %s: %s", target_text, strerror(errno));
            if (pidfd >= 0) {
                close(pidfd);
            }
            return false;
        }
        if (!has_handler) {
            bx_kill_verbose_skip_no_handler(options, target_text, options->signal_number);
            if (pidfd >= 0) {
                close(pidfd);
            }
            return false;
        }
    }

    bx_kill_verbose_send(options, target_text, options->signal_number);
    if (!bx_kill_send_once(options, pid, pidfd, options->signal_number)) {
        bx_diag(diag, "sending signal to %s failed: %s", target_text, strerror(errno));
        if (pidfd >= 0) {
            close(pidfd);
        }
        return false;
    }

    for (size_t i = 0; i < options->timeout_count; i++) {
        if (pidfd < 0) {
            bx_diag(diag, "internal error: timeout requires pidfd");
            return false;
        }

        int wait_rc = bx_kill_wait_pidfd_for_timeout(pidfd, options->timeouts[i].milliseconds);
        if (wait_rc < 0) {
            bx_diag(diag, "failed while waiting on pidfd for %s: %s", target_text, strerror(errno));
            close(pidfd);
            return false;
        }
        if (wait_rc == 0) {
            continue;
        }

        bx_kill_verbose_send(options, target_text, options->timeouts[i].signal_number);
        if (!bx_kill_send_once(options, pid, pidfd, options->timeouts[i].signal_number)) {
            if (errno == ESRCH) {
                close(pidfd);
                return true;
            }
            bx_diag(diag, "sending signal to %s failed: %s", target_text, strerror(errno));
            close(pidfd);
            return false;
        }
    }

    if (pidfd >= 0) {
        close(pidfd);
    }
    return true;
}

static bool bx_kill_print_pid_target(pid_t pid) {
    printf("%ld\n", (long)pid);
    return true;
}

static void bx_kill_record_result(struct bx_kill_outcome* outcome, bool success) {
    if (success) {
        outcome->successes++;
    }
    else {
        outcome->failures++;
    }
}

static bool bx_kill_handle_numeric_target(
    const struct bx_kill_options* options,
    const char* operand,
    struct bx_diag_ctx* diag,
    struct bx_kill_outcome* outcome
) {
    pid_t pid = -1;
    if (!bx_kill_parse_pid_text(operand, &pid)) {
        bx_diag(diag, "invalid PID '%s'", operand);
        bx_kill_record_result(outcome, false);
        return false;
    }

    if (options->action == BX_KILL_ACTION_PID) {
        bx_kill_print_pid_target(pid);
        bx_kill_record_result(outcome, true);
        return true;
    }

    bool ok = bx_kill_send_target(options, pid, operand, 0, diag);
    bx_kill_record_result(outcome, ok);
    return ok;
}

static bool bx_kill_handle_pidfd_target(
    const struct bx_kill_options* options,
    const char* operand,
    struct bx_diag_ctx* diag,
    struct bx_kill_outcome* outcome
) {
    pid_t pid = -1;
    uintmax_t inode = 0;
    if (!bx_kill_parse_pidfd_operand(operand, &pid, &inode)) {
        bx_diag(diag, "invalid pid:pidfd operand '%s'", operand);
        bx_kill_record_result(outcome, false);
        return false;
    }

    if (!options->signal_explicit) {
        bx_diag(diag, "pid:pidfd operands require an explicit signal option");
        bx_kill_record_result(outcome, false);
        return false;
    }

    if (options->action == BX_KILL_ACTION_PID) {
        bx_diag(diag, "-p does not accept pid:pidfd operands");
        bx_kill_record_result(outcome, false);
        return false;
    }

    bool ok = bx_kill_send_target(options, pid, operand, inode, diag);
    bx_kill_record_result(outcome, ok);
    return ok;
}

static bool bx_kill_handle_name_target(
    const struct bx_kill_options* options,
    const char* operand,
    struct bx_diag_ctx* diag,
    struct bx_kill_outcome* outcome
) {
    struct bx_kill_uint_list pids = {0};
    bool ok = bx_kill_resolve_name_to_pids(options, operand, &pids);
    if (!ok) {
        bx_diag(diag, "failed to enumerate /proc: %s", strerror(errno));
        bx_kill_record_result(outcome, false);
        free(pids.items);
        return false;
    }

    if (pids.len == 0) {
        bx_diag(diag, "cannot find process \"%s\"", operand);
        bx_kill_record_result(outcome, false);
        free(pids.items);
        return false;
    }

    bool all_ok = true;
    for (size_t i = 0; i < pids.len; i++) {
        char target_text[64];
        snprintf(target_text, sizeof(target_text), "%" PRIuMAX, pids.items[i]);

        bool target_ok = false;
        if (options->action == BX_KILL_ACTION_PID) {
            printf("%s\n", target_text);
            target_ok = true;
        }
        else {
            target_ok = bx_kill_send_target(options, (pid_t)pids.items[i], target_text, 0, diag);
        }
        bx_kill_record_result(outcome, target_ok);
        if (!target_ok) {
            all_ok = false;
        }
    }

    free(pids.items);
    return all_ok;
}

static bool bx_kill_execute_targets(const struct bx_kill_options* options, int argc, char** argv, struct bx_diag_ctx* diag, struct bx_kill_outcome* outcome) {
    if (options->first_operand_index >= argc) {
        bx_diag(diag, options->action == BX_KILL_ACTION_PID ? "not enough arguments" : "not enough arguments");
        return false;
    }

    bool all_ok = true;
    for (int i = options->first_operand_index; i < argc; i++) {
        const char* operand = argv[i];
        bool target_ok = false;

        if (strchr(operand, ':') != NULL) {
            target_ok = bx_kill_handle_pidfd_target(options, operand, diag, outcome);
        }
        else {
            pid_t numeric_pid = -1;
            if (bx_kill_parse_pid_text(operand, &numeric_pid)) {
                target_ok = bx_kill_handle_numeric_target(options, operand, diag, outcome);
            }
            else {
                target_ok = bx_kill_handle_name_target(options, operand, diag, outcome);
            }
        }

        if (!target_ok) {
            all_ok = false;
        }
    }

    return all_ok;
}

static bool bx_kill_parse_options(int argc, char** argv, struct bx_kill_options* options, struct bx_diag_ctx* diag) {
    memset(options, 0, sizeof(*options));
    options->progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "kill");
    options->signal_number = SIGTERM;
    options->action = BX_KILL_ACTION_SEND;
    options->first_operand_index = argc;
    diag->progname = options->progname;

    int i = 1;
    while (i < argc) {
        const char* arg = argv[i];
        if (strcmp(arg, "--") == 0) {
            i++;
            break;
        }
        if (arg[0] != '-' || arg[1] == '\0') {
            break;
        }

        if (strncmp(arg, "--", 2) == 0) {
            if (strcmp(arg, "--help") == 0) {
                options->show_help = true;
                return true;
            }
            if (strcmp(arg, "--version") == 0) {
                options->show_version = true;
                return true;
            }
            if (strcmp(arg, "--all") == 0) {
                options->all_uids = true;
                i++;
                continue;
            }
            if (strcmp(arg, "--pid") == 0) {
                options->action = BX_KILL_ACTION_PID;
                i++;
                continue;
            }
            if (strcmp(arg, "--list") == 0) {
                options->action = BX_KILL_ACTION_LIST;
                i++;
                continue;
            }
            if (strncmp(arg, "--list=", 7) == 0) {
                options->action = BX_KILL_ACTION_LIST;
                options->list_query = arg + 7;
                i++;
                continue;
            }
            if (strcmp(arg, "--table") == 0) {
                options->action = BX_KILL_ACTION_TABLE;
                i++;
                continue;
            }
            if (strcmp(arg, "--require-handler") == 0) {
                options->require_handler = true;
                i++;
                continue;
            }
            if (strcmp(arg, "--verbose") == 0) {
                options->verbose = true;
                i++;
                continue;
            }
            if (strcmp(arg, "--signal") == 0 || strncmp(arg, "--signal=", 9) == 0) {
                const char* value = NULL;
                if (strncmp(arg, "--signal=", 9) == 0) {
                    value = arg + 9;
                }
                else if (i + 1 < argc) {
                    value = argv[++i];
                }
                else {
                    bx_diag(diag, "option requires an argument -- '--signal'");
                    return false;
                }

                if (!bx_kill_parse_signal(value, &options->signal_number)) {
                    bx_diag(diag, "invalid signal '%s'", value);
                    return false;
                }
                options->signal_explicit = true;
                i++;
                continue;
            }
            if (strcmp(arg, "--queue") == 0 || strncmp(arg, "--queue=", 8) == 0) {
                const char* value = NULL;
                if (strncmp(arg, "--queue=", 8) == 0) {
                    value = arg + 8;
                }
                else if (i + 1 < argc) {
                    value = argv[++i];
                }
                else {
                    bx_diag(diag, "option requires an argument -- '--queue'");
                    return false;
                }

                if (!bx_kill_parse_queue_value(value, &options->queue_value)) {
                    bx_diag(diag, "invalid queue value '%s'", value);
                    return false;
                }
                options->queue_set = true;
                i++;
                continue;
            }
            if (strcmp(arg, "--show-process-state") == 0 || strncmp(arg, "--show-process-state=", 21) == 0) {
                const char* value = NULL;
                if (strncmp(arg, "--show-process-state=", 21) == 0) {
                    value = arg + 21;
                }
                else if (i + 1 < argc) {
                    value = argv[++i];
                }
                else {
                    bx_diag(diag, "option requires an argument -- '--show-process-state'");
                    return false;
                }
                options->action = BX_KILL_ACTION_SHOW_PROCESS_STATE;
                options->show_state_pid = value;
                i++;
                continue;
            }
            if (strcmp(arg, "--timeout") == 0 || strncmp(arg, "--timeout=", 10) == 0) {
                const char* ms_text = NULL;
                const char* signal_text = NULL;
                if (strncmp(arg, "--timeout=", 10) == 0) {
                    ms_text = arg + 10;
                }
                else if (i + 1 < argc) {
                    ms_text = argv[++i];
                }
                else {
                    bx_diag(diag, "option requires an argument -- '--timeout'");
                    return false;
                }
                if (i + 1 >= argc) {
                    bx_diag(diag, "--timeout requires a milliseconds argument and a signal");
                    return false;
                }
                signal_text = argv[++i];

                int milliseconds = 0;
                int signal_number = 0;
                if (!bx_kill_parse_timeout_ms(ms_text, &milliseconds)) {
                    bx_diag(diag, "invalid timeout '%s'", ms_text);
                    return false;
                }
                if (!bx_kill_parse_signal(signal_text, &signal_number)) {
                    bx_diag(diag, "invalid signal '%s'", signal_text);
                    return false;
                }
                if (!bx_kill_add_timeout(options, milliseconds, signal_number)) {
                    bx_diag(diag, "out of memory");
                    return false;
                }
                i++;
                continue;
            }

            bx_diag(diag, "unrecognized option '%s'", arg);
            return false;
        }

        if (strcmp(arg, "-h") == 0) {
            options->show_help = true;
            return true;
        }
        if (strcmp(arg, "-V") == 0) {
            options->show_version = true;
            return true;
        }
        if (strcmp(arg, "-a") == 0) {
            options->all_uids = true;
            i++;
            continue;
        }
        if (strcmp(arg, "-p") == 0) {
            options->action = BX_KILL_ACTION_PID;
            i++;
            continue;
        }
        if (strcmp(arg, "-l") == 0) {
            options->action = BX_KILL_ACTION_LIST;
            i++;
            continue;
        }
        if (strcmp(arg, "-L") == 0) {
            options->action = BX_KILL_ACTION_TABLE;
            i++;
            continue;
        }
        if (strcmp(arg, "-r") == 0) {
            options->require_handler = true;
            i++;
            continue;
        }
        if (strcmp(arg, "-s") == 0 || strncmp(arg, "-s", 2) == 0) {
            const char* value = NULL;
            if (strlen(arg) > 2) {
                value = arg + 2;
            }
            else if (i + 1 < argc) {
                value = argv[++i];
            }
            else {
                bx_diag(diag, "option requires an argument -- 's'");
                return false;
            }

            if (!bx_kill_parse_signal(value, &options->signal_number)) {
                bx_diag(diag, "invalid signal '%s'", value);
                return false;
            }
            options->signal_explicit = true;
            i++;
            continue;
        }
        if (strcmp(arg, "-q") == 0 || strncmp(arg, "-q", 2) == 0) {
            const char* value = NULL;
            if (strlen(arg) > 2) {
                value = arg + 2;
            }
            else if (i + 1 < argc) {
                value = argv[++i];
            }
            else {
                bx_diag(diag, "option requires an argument -- 'q'");
                return false;
            }

            if (!bx_kill_parse_queue_value(value, &options->queue_value)) {
                bx_diag(diag, "invalid queue value '%s'", value);
                return false;
            }
            options->queue_set = true;
            i++;
            continue;
        }
        if (strcmp(arg, "-d") == 0 || strncmp(arg, "-d", 2) == 0) {
            const char* value = NULL;
            if (strlen(arg) > 2) {
                value = arg + 2;
            }
            else if (i + 1 < argc) {
                value = argv[++i];
            }
            else {
                bx_diag(diag, "option requires an argument -- 'd'");
                return false;
            }
            options->action = BX_KILL_ACTION_SHOW_PROCESS_STATE;
            options->show_state_pid = value;
            i++;
            continue;
        }

        if (!options->signal_explicit && bx_kill_parse_signal(arg + 1, &options->signal_number)) {
            options->signal_explicit = true;
            i++;
            continue;
        }

        if (bx_kill_looks_like_negative_pid_operand(arg)) {
            break;
        }

        bx_diag(diag, "invalid option -- '%s'", arg);
        return false;
    }

    options->first_operand_index = i;
    if (options->action == BX_KILL_ACTION_LIST && options->list_query == NULL && i < argc) {
        options->list_query = argv[i];
        options->first_operand_index = i + 1;
    }

    if (options->show_state_pid != NULL) {
        if (options->action != BX_KILL_ACTION_SHOW_PROCESS_STATE) {
            bx_diag(diag, "internal option parse error");
            return false;
        }
    }

    return true;
}

static bool bx_kill_validate_options(const struct bx_kill_options* options, int argc, struct bx_diag_ctx* diag) {
    if (options->action == BX_KILL_ACTION_LIST) {
        if (options->first_operand_index != argc) {
            bx_diag(diag, "-l accepts at most one argument");
            return false;
        }
        return true;
    }

    if (options->action == BX_KILL_ACTION_TABLE) {
        if (options->first_operand_index != argc) {
            bx_diag(diag, "-L does not accept operands");
            return false;
        }
        return true;
    }

    if (options->action == BX_KILL_ACTION_SHOW_PROCESS_STATE) {
        if (options->show_state_pid == NULL) {
            bx_diag(diag, "-d requires a PID");
            return false;
        }
        if (options->first_operand_index != argc) {
            bx_diag(diag, "-d accepts exactly one PID");
            return false;
        }
        return true;
    }

    if (options->timeout_count > 0 && options->action != BX_KILL_ACTION_SEND) {
        bx_diag(diag, "--timeout is only valid when sending signals");
        return false;
    }

    if (options->queue_set && options->action != BX_KILL_ACTION_SEND) {
        bx_diag(diag, "--queue is only valid when sending signals");
        return false;
    }

    if (options->require_handler && options->action != BX_KILL_ACTION_SEND) {
        bx_diag(diag, "--require-handler is only valid when sending signals");
        return false;
    }

    return true;
}

int bx_kill_main(int argc, char** argv) {
    struct bx_kill_options options;
    struct bx_diag_ctx diag = {
        .progname = "kill",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };

    if (!bx_kill_parse_options(argc, argv, &options, &diag)) {
        free(options.timeouts);
        bx_cli_print_try_help(options.progname != NULL ? options.progname : "kill");
        return 1;
    }

    if (options.show_help) {
        bx_kill_print_help(stdout, options.progname);
        free(options.timeouts);
        return 0;
    }

    if (options.show_version) {
        bx_cli_print_version(options.progname);
        free(options.timeouts);
        return 0;
    }

    if (!bx_kill_validate_options(&options, argc, &diag)) {
        free(options.timeouts);
        bx_cli_print_try_help(options.progname);
        return 1;
    }

    int rc = 0;
    if (options.action == BX_KILL_ACTION_LIST) {
        rc = bx_kill_print_list_query(options.list_query, &diag) ? 0 : 1;
    }
    else if (options.action == BX_KILL_ACTION_TABLE) {
        bx_kill_print_signal_table(stdout);
        rc = 0;
    }
    else if (options.action == BX_KILL_ACTION_SHOW_PROCESS_STATE) {
        rc = bx_kill_print_show_process_state(options.show_state_pid, &diag) ? 0 : 1;
    }
    else {
        struct bx_kill_outcome outcome = {0};
        bool exec_ok = bx_kill_execute_targets(&options, argc, argv, &diag, &outcome);
        if (exec_ok && outcome.failures == 0) {
            rc = 0;
        }
        else if (outcome.successes > 0 && (outcome.successes + outcome.failures) > 1) {
            rc = 64;
        }
        else {
            rc = 1;
        }
    }

    free(options.timeouts);
    return rc;
}
