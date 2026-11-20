#define _GNU_SOURCE

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <getopt.h>
#include <grp.h>
#include <inttypes.h>
#include <limits.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "applets.h"
#include "applets/system/psmisc/procfs.h"
#include "bx/diag.h"
#include "bx/libbx.h"
#include "lib/args_common.h"
#include "lib/cli_common.h"

enum bx_ps_field {
    BX_PS_FIELD_PID,
    BX_PS_FIELD_PPID,
    BX_PS_FIELD_PGID,
    BX_PS_FIELD_SID,
    BX_PS_FIELD_STAT,
    BX_PS_FIELD_TTY,
    BX_PS_FIELD_COMM,
    BX_PS_FIELD_ARGS,
    BX_PS_FIELD_UID,
    BX_PS_FIELD_USER,
    BX_PS_FIELD_GID,
    BX_PS_FIELD_GROUP,
    BX_PS_FIELD_PRI,
    BX_PS_FIELD_NI,
    BX_PS_FIELD_VSZ,
    BX_PS_FIELD_RSS,
    BX_PS_FIELD_TIME,
    BX_PS_FIELD_ETIME,
};

struct bx_ps_field_def {
    const char* name;
    enum bx_ps_field field;
    const char* header;
};

static const struct bx_ps_field_def bx_ps_field_defs[] = {
    {"pid", BX_PS_FIELD_PID, "PID"},
    {"ppid", BX_PS_FIELD_PPID, "PPID"},
    {"pgid", BX_PS_FIELD_PGID, "PGID"},
    {"sid", BX_PS_FIELD_SID, "SID"},
    {"stat", BX_PS_FIELD_STAT, "STAT"},
    {"state", BX_PS_FIELD_STAT, "STAT"},
    {"tty", BX_PS_FIELD_TTY, "TTY"},
    {"tt", BX_PS_FIELD_TTY, "TTY"},
    {"comm", BX_PS_FIELD_COMM, "COMMAND"},
    {"args", BX_PS_FIELD_ARGS, "COMMAND"},
    {"cmd", BX_PS_FIELD_ARGS, "COMMAND"},
    {"command", BX_PS_FIELD_ARGS, "COMMAND"},
    {"uid", BX_PS_FIELD_UID, "UID"},
    {"user", BX_PS_FIELD_USER, "USER"},
    {"gid", BX_PS_FIELD_GID, "GID"},
    {"group", BX_PS_FIELD_GROUP, "GROUP"},
    {"pri", BX_PS_FIELD_PRI, "PRI"},
    {"ni", BX_PS_FIELD_NI, "NI"},
    {"nice", BX_PS_FIELD_NI, "NI"},
    {"vsz", BX_PS_FIELD_VSZ, "VSZ"},
    {"rss", BX_PS_FIELD_RSS, "RSS"},
    {"time", BX_PS_FIELD_TIME, "TIME"},
    {"etime", BX_PS_FIELD_ETIME, "ELAPSED"},
};

struct bx_ps_column {
    enum bx_ps_field field;
    char* header;
    bool show_header;
};

struct bx_ps_pid_filter {
    pid_t* items;
    size_t len;
    size_t cap;
    bool have_filter;
    bool all;
};

struct bx_ps_options {
    const char* progname;
    bool show_help;
    bool show_version;
    bool format_given;
    struct bx_ps_column* columns;
    size_t column_count;
    size_t column_cap;
    struct bx_ps_pid_filter pids;
};

struct bx_ps_row {
    pid_t pid;
    pid_t ppid;
    pid_t pgrp;
    pid_t session;
    bool have_stat;
    char state;
    char* tty;
    char* comm;
    char* args;
    uid_t uid;
    gid_t gid;
    long priority;
    long nice;
    unsigned long long utime_ticks;
    unsigned long long stime_ticks;
    unsigned long long starttime_ticks;
    unsigned long long vsize_bytes;
    long long rss_pages;
};

enum bx_ps_row_status {
    BX_PS_ROW_OK = 0,
    BX_PS_ROW_SKIP,
};

struct bx_ps_needs {
    bool stat;
    bool tty;
    bool comm;
    bool args;
    bool uid;
    bool gid;
};

static void bx_ps_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]...\n", progname);
    fprintf(stream, "Display /proc process information.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -A, -e                 select all processes (the default)\n");
    fprintf(stream, "  -o, --format FORMAT    select comma-separated output fields\n");
    fprintf(stream, "  -p, --pid PID[,PID]    select processes by PID\n");
    fprintf(stream, "  -h, --help             display this help and exit\n");
    fprintf(stream, "  -V, --version          output version information and exit\n");
    fprintf(stream, "\n");
    fprintf(stream, "Fields: pid, ppid, pgid, sid, stat, tty, comm, args, uid, user,\n");
    fprintf(stream, "        gid, group, pri, ni, vsz, rss, time, etime\n");
    fprintf(stream, "Append '=' to a field to suppress its header, or FIELD=HEADER to\n");
    fprintf(stream, "set a custom header.\n");
}

static bool bx_ps_field_name_matches(const char* name, size_t name_len, const char* candidate) {
    return strlen(candidate) == name_len && memcmp(name, candidate, name_len) == 0;
}

static bool bx_ps_lookup_field(const char* name, size_t name_len, enum bx_ps_field* field_out,
                               const char** header_out) {
    for (size_t i = 0; i < sizeof(bx_ps_field_defs) / sizeof(bx_ps_field_defs[0]); i++) {
        if (bx_ps_field_name_matches(name, name_len, bx_ps_field_defs[i].name)) {
            *field_out = bx_ps_field_defs[i].field;
            *header_out = bx_ps_field_defs[i].header;
            return true;
        }
    }
    return false;
}

static char* bx_ps_strndup(const char* text, size_t len) {
    char* copy = xmalloc(len + 1u);
    memcpy(copy, text, len);
    copy[len] = '\0';
    return copy;
}

static void bx_ps_trim(const char** begin, const char** end) {
    while (*begin < *end && isspace((unsigned char)**begin)) {
        (*begin)++;
    }
    while (*end > *begin && isspace((unsigned char)(*end)[-1])) {
        (*end)--;
    }
}

static bool bx_ps_add_column(struct bx_ps_options* options, enum bx_ps_field field,
                             const char* header, size_t header_len, bool show_header,
                             struct bx_diag_ctx* diag) {
    if (options->column_count == options->column_cap) {
        size_t next_cap = options->column_cap ? options->column_cap * 2u : 8u;
        options->columns = xrealloc(options->columns, next_cap * sizeof(*options->columns));
        options->column_cap = next_cap;
    }

    struct bx_ps_column* column = &options->columns[options->column_count++];
    column->field = field;
    column->show_header = show_header;
    column->header = show_header ? bx_ps_strndup(header, header_len) : NULL;

    if (show_header && column->header == NULL) {
        bx_diag(diag, "failed to allocate column header");
        return false;
    }
    return true;
}

static bool bx_ps_add_format(struct bx_ps_options* options, const char* format,
                             struct bx_diag_ctx* diag) {
    const char* cursor = format;

    if (format == NULL || format[0] == '\0') {
        bx_diag(diag, "empty output format");
        return false;
    }

    while (true) {
        const char* comma = strchr(cursor, ',');
        const char* token_end = comma ? comma : cursor + strlen(cursor);
        const char* begin = cursor;
        const char* end = token_end;
        const char* equals;
        const char* field_begin;
        const char* field_end;
        enum bx_ps_field field;
        const char* default_header;

        bx_ps_trim(&begin, &end);
        if (begin == end) {
            bx_diag(diag, "empty field in output format");
            return false;
        }

        equals = memchr(begin, '=', (size_t)(end - begin));
        field_begin = begin;
        field_end = equals ? equals : end;
        bx_ps_trim(&field_begin, &field_end);
        if (field_begin == field_end ||
            !bx_ps_lookup_field(field_begin, (size_t)(field_end - field_begin), &field, &default_header)) {
            bx_diag(diag, "unknown output field '%.*s'", (int)(field_end - field_begin), field_begin);
            return false;
        }

        if (equals != NULL) {
            const char* header_begin = equals + 1;
            const char* header_end = end;
            bx_ps_trim(&header_begin, &header_end);
            if (!bx_ps_add_column(options, field, header_begin, (size_t)(header_end - header_begin),
                                  header_begin != header_end, diag)) {
                return false;
            }
        } else if (!bx_ps_add_column(options, field, default_header, strlen(default_header), true, diag)) {
            return false;
        }

        if (comma == NULL) {
            return true;
        }
        cursor = comma + 1;
        if (*cursor == '\0') {
            bx_diag(diag, "output format ends with an empty field");
            return false;
        }
    }
}

static bool bx_ps_add_default_columns(struct bx_ps_options* options, struct bx_diag_ctx* diag) {
    static const enum bx_ps_field defaults[] = {
        BX_PS_FIELD_PID,
        BX_PS_FIELD_PPID,
        BX_PS_FIELD_STAT,
        BX_PS_FIELD_TTY,
        BX_PS_FIELD_ARGS,
    };
    static const char* const headers[] = {"PID", "PPID", "STAT", "TTY", "COMMAND"};

    for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++) {
        if (!bx_ps_add_column(options, defaults[i], headers[i], strlen(headers[i]), true, diag)) {
            return false;
        }
    }
    return true;
}

static void bx_ps_filter_add(struct bx_ps_pid_filter* filter, pid_t pid) {
    for (size_t i = 0; i < filter->len; i++) {
        if (filter->items[i] == pid) {
            return;
        }
    }
    if (filter->len == filter->cap) {
        size_t next_cap = filter->cap ? filter->cap * 2u : 8u;
        filter->items = xrealloc(filter->items, next_cap * sizeof(*filter->items));
        filter->cap = next_cap;
    }
    filter->items[filter->len++] = pid;
}

static bool bx_ps_add_pid_list(struct bx_ps_options* options, const char* list,
                               struct bx_diag_ctx* diag) {
    char* copy;
    char* saveptr = NULL;
    char* token;
    bool added = false;

    if (list == NULL || list[0] == '\0') {
        bx_diag(diag, "empty PID list");
        return false;
    }

    copy = xstrdup(list);
    token = strtok_r(copy, ", \t\r\n", &saveptr);
    while (token != NULL) {
        pid_t pid;
        if (!bx_proc_parse_pid_arg(token, &pid)) {
            bx_diag(diag, "invalid PID '%s'", token);
            free(copy);
            return false;
        }
        bx_ps_filter_add(&options->pids, pid);
        added = true;
        token = strtok_r(NULL, ", \t\r\n", &saveptr);
    }
    free(copy);

    if (!added) {
        bx_diag(diag, "empty PID list");
        return false;
    }
    options->pids.have_filter = true;
    return true;
}

static bool bx_ps_parse_options(int argc, char** argv, struct bx_ps_options* options,
                                struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"help", no_argument, NULL, 'h'},
        {"version", no_argument, NULL, 'V'},
        {"format", required_argument, NULL, 'o'},
        {"pid", required_argument, NULL, 'p'},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "ps");
    diag->progname = options->progname;

    bx_args_getopt_reset();

    while (true) {
        int c = bx_args_getopt_long(argc, argv, "+hVAeo:p:", long_options, NULL);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 'h':
                options->show_help = true;
                return true;
            case 'V':
                options->show_version = true;
                return true;
            case 'A':
            case 'e':
                options->pids.all = true;
                break;
            case 'o':
                options->format_given = true;
                if (!bx_ps_add_format(options, optarg, diag)) {
                    return false;
                }
                break;
            case 'p':
                if (!bx_ps_add_pid_list(options, optarg, diag)) {
                    return false;
                }
                break;
            case '?':
                bx_cli_diag_unrecognized_option(diag, optopt, optind, argc, argv);
                return false;
            default:
                return false;
        }
    }

    if (optind < argc) {
        bx_diag(diag, "unexpected operand '%s'", argv[optind]);
        return false;
    }

    if (!options->format_given && !bx_ps_add_default_columns(options, diag)) {
        return false;
    }

    return true;
}

static void bx_ps_free_options(struct bx_ps_options* options) {
    for (size_t i = 0; i < options->column_count; i++) {
        free(options->columns[i].header);
    }
    free(options->columns);
    free(options->pids.items);
    memset(options, 0, sizeof(*options));
}

static void bx_ps_row_clear(struct bx_ps_row* row) {
    free(row->tty);
    free(row->comm);
    free(row->args);
    row->tty = NULL;
    row->comm = NULL;
    row->args = NULL;
}

static char* bx_ps_read_tty(pid_t pid, bool* vanished_out) {
    char* target = NULL;

    *vanished_out = false;
    if (!bx_proc_readlink_leaf(pid, "fd/0", &target, vanished_out)) {
        return NULL;
    }

    if (strncmp(target, "/dev/", 5) == 0 && target[5] != '\0') {
        char* name = xstrdup(target + 5);
        free(target);
        return name;
    }
    return target;
}

static enum bx_ps_row_status bx_ps_collect_row(pid_t pid, const struct bx_ps_needs* needs,
                                               struct bx_ps_row* row) {
    struct bx_proc_stat stat_info = {0};
    bool vanished = false;
    char* args = NULL;

    memset(row, 0, sizeof(*row));
    row->pid = pid;
    row->ppid = -1;
    row->pgrp = -1;
    row->session = -1;
    row->state = '?';
    row->uid = (uid_t)-1;
    row->gid = (gid_t)-1;

    if (needs->stat || needs->comm) {
        if (bx_proc_read_stat(pid, &stat_info, &vanished)) {
            row->have_stat = true;
            row->ppid = stat_info.ppid;
            row->pgrp = stat_info.pgrp;
            row->session = stat_info.session;
            row->state = stat_info.state;
            row->priority = stat_info.priority;
            row->nice = stat_info.nice;
            row->utime_ticks = stat_info.utime_ticks;
            row->stime_ticks = stat_info.stime_ticks;
            row->starttime_ticks = stat_info.starttime_ticks;
            row->vsize_bytes = stat_info.vsize_bytes;
            row->rss_pages = stat_info.rss_pages;
            row->comm = stat_info.comm;
            stat_info.comm = NULL;
        }
        bx_proc_stat_free(&stat_info);
        if (!row->have_stat && vanished) {
            return BX_PS_ROW_SKIP;
        }
    }

    if (needs->tty) {
        row->tty = bx_ps_read_tty(pid, &vanished);
        if (vanished) {
            bx_ps_row_clear(row);
            return BX_PS_ROW_SKIP;
        }
        if (row->tty == NULL) {
            row->tty = xstrdup("?");
        }
    }

    if (needs->args) {
        if (bx_proc_read_cmdline(pid, &args, &vanished)) {
            if (args != NULL && args[0] != '\0') {
                row->args = args;
            } else {
                free(args);
            }
        }
        else if (vanished) {
            bx_ps_row_clear(row);
            return BX_PS_ROW_SKIP;
        }

        if (row->args == NULL) {
            row->args = xstrdup(row->comm ? row->comm : "?");
        }
    }

    if (needs->uid && !bx_proc_read_uid(pid, &row->uid, &vanished)) {
        if (vanished) {
            bx_ps_row_clear(row);
            return BX_PS_ROW_SKIP;
        }
        row->uid = (uid_t)-1;
    }

    if (needs->gid && !bx_proc_read_gid(pid, &row->gid, &vanished)) {
        if (vanished) {
            bx_ps_row_clear(row);
            return BX_PS_ROW_SKIP;
        }
        row->gid = (gid_t)-1;
    }

    return BX_PS_ROW_OK;
}

static void bx_ps_free_rows(struct bx_ps_row* rows, size_t row_count) {
    for (size_t i = 0; i < row_count; i++) {
        bx_ps_row_clear(&rows[i]);
    }
    free(rows);
}

static int bx_ps_compare_rows(const void* lhs_void, const void* rhs_void) {
    const struct bx_ps_row* lhs = lhs_void;
    const struct bx_ps_row* rhs = rhs_void;

    if (lhs->pid < rhs->pid) {
        return -1;
    }
    if (lhs->pid > rhs->pid) {
        return 1;
    }
    return 0;
}

static void bx_ps_rows_push(struct bx_ps_row** rows, size_t* row_count, size_t* row_cap,
                            struct bx_ps_row* row) {
    if (*row_count == *row_cap) {
        size_t next_cap = *row_cap ? *row_cap * 2u : 64u;
        *rows = xrealloc(*rows, next_cap * sizeof(**rows));
        *row_cap = next_cap;
    }
    (*rows)[(*row_count)++] = *row;
    memset(row, 0, sizeof(*row));
}

static struct bx_ps_needs bx_ps_required_data(const struct bx_ps_options* options) {
    struct bx_ps_needs needs = {0};

    for (size_t i = 0; i < options->column_count; i++) {
        switch (options->columns[i].field) {
            case BX_PS_FIELD_PID:
                break;
            case BX_PS_FIELD_TTY:
                needs.tty = true;
                break;
            case BX_PS_FIELD_COMM:
                needs.comm = true;
                break;
            case BX_PS_FIELD_ARGS:
                needs.args = true;
                needs.comm = true;
                break;
            case BX_PS_FIELD_UID:
            case BX_PS_FIELD_USER:
                needs.uid = true;
                break;
            case BX_PS_FIELD_GID:
            case BX_PS_FIELD_GROUP:
                needs.gid = true;
                break;
            default:
                needs.stat = true;
                break;
        }
    }

    return needs;
}

static bool bx_ps_collect_rows(const struct bx_ps_options* options, struct bx_ps_row** rows_out,
                               size_t* row_count_out, struct bx_diag_ctx* diag) {
    struct bx_ps_row* rows = NULL;
    size_t row_count = 0;
    size_t row_cap = 0;
    struct bx_ps_needs needs = bx_ps_required_data(options);

    *rows_out = NULL;
    *row_count_out = 0;

    if (options->pids.have_filter && !options->pids.all) {
        struct bx_ps_needs filter_needs = needs;
        filter_needs.stat = true; /* Selected PIDs must still exist. */
        for (size_t i = 0; i < options->pids.len; i++) {
            struct bx_ps_row row;
            if (bx_ps_collect_row(options->pids.items[i], &filter_needs, &row) == BX_PS_ROW_OK) {
                bx_ps_rows_push(&rows, &row_count, &row_cap, &row);
            }
        }
    }
    else {
        DIR* proc_dir = opendir("/proc");
        if (proc_dir == NULL) {
            bx_diag(diag, "failed to open /proc: %s", strerror(errno));
            return false;
        }

        struct dirent* entry = NULL;
        while (true) {
            errno = 0;
            entry = readdir(proc_dir);
            if (entry == NULL) {
                break;
            }

            pid_t pid;
            if (!bx_proc_parse_pid_arg(entry->d_name, &pid)) {
                continue;
            }

            struct bx_ps_row row;
            if (bx_ps_collect_row(pid, &needs, &row) == BX_PS_ROW_OK) {
                bx_ps_rows_push(&rows, &row_count, &row_cap, &row);
            }
        }

        if (errno != 0) {
            bx_diag(diag, "failed while reading /proc: %s", strerror(errno));
            bx_ps_free_rows(rows, row_count);
            closedir(proc_dir);
            return false;
        }

        closedir(proc_dir);
    }

    qsort(rows, row_count, sizeof(*rows), bx_ps_compare_rows);
    *rows_out = rows;
    *row_count_out = row_count;
    return true;
}

static void bx_ps_format_duration(unsigned long long seconds, char* out, size_t out_size) {
    unsigned long long days = seconds / 86400u;
    unsigned long long hours;
    unsigned long long minutes;

    seconds %= 86400u;
    hours = seconds / 3600u;
    seconds %= 3600u;
    minutes = seconds / 60u;
    seconds %= 60u;

    if (days > 0) {
        snprintf(out, out_size, "%llu-%02llu:%02llu:%02llu",
                 days, hours, minutes, seconds);
    }
    else if (hours > 0) {
        snprintf(out, out_size, "%llu:%02llu:%02llu", hours, minutes, seconds);
    }
    else {
        snprintf(out, out_size, "%llu:%02llu", minutes, seconds);
    }
}

static void bx_ps_format_user(uid_t uid, char* out, size_t out_size) {
    struct passwd pwd;
    struct passwd* result = NULL;
    char buffer[1024];

    if (uid != (uid_t)-1 &&
        getpwuid_r(uid, &pwd, buffer, sizeof(buffer), &result) == 0 &&
        result != NULL && result->pw_name != NULL) {
        snprintf(out, out_size, "%s", result->pw_name);
        return;
    }
    if (uid == (uid_t)-1) {
        snprintf(out, out_size, "?");
    }
    else {
        snprintf(out, out_size, "%" PRIuMAX, (uintmax_t)uid);
    }
}

static void bx_ps_format_group(gid_t gid, char* out, size_t out_size) {
    struct group grp;
    struct group* result = NULL;
    char buffer[1024];

    if (gid != (gid_t)-1 &&
        getgrgid_r(gid, &grp, buffer, sizeof(buffer), &result) == 0 &&
        result != NULL && result->gr_name != NULL) {
        snprintf(out, out_size, "%s", result->gr_name);
        return;
    }
    if (gid == (gid_t)-1) {
        snprintf(out, out_size, "?");
    }
    else {
        snprintf(out, out_size, "%" PRIuMAX, (uintmax_t)gid);
    }
}

static void bx_ps_render_column(const struct bx_ps_column* column, const struct bx_ps_row* row,
                                char* out, size_t out_size) {
    long hz;
    unsigned long long total_ticks;

    switch (column->field) {
        case BX_PS_FIELD_PID:
            snprintf(out, out_size, "%ld", (long)row->pid);
            return;
        case BX_PS_FIELD_PPID:
            if (row->have_stat)
                snprintf(out, out_size, "%ld", (long)row->ppid);
            else
                snprintf(out, out_size, "?");
            return;
        case BX_PS_FIELD_PGID:
            if (row->have_stat)
                snprintf(out, out_size, "%ld", (long)row->pgrp);
            else
                snprintf(out, out_size, "?");
            return;
        case BX_PS_FIELD_SID:
            if (row->have_stat)
                snprintf(out, out_size, "%ld", (long)row->session);
            else
                snprintf(out, out_size, "?");
            return;
        case BX_PS_FIELD_STAT:
            snprintf(out, out_size, "%c", row->state);
            return;
        case BX_PS_FIELD_TTY:
            snprintf(out, out_size, "%s", row->tty ? row->tty : "?");
            return;
        case BX_PS_FIELD_COMM:
            snprintf(out, out_size, "%s", row->comm ? row->comm : "?");
            return;
        case BX_PS_FIELD_ARGS:
            snprintf(out, out_size, "%s", row->args ? row->args : "?");
            return;
        case BX_PS_FIELD_UID:
            if (row->uid == (uid_t)-1)
                snprintf(out, out_size, "?");
            else
                snprintf(out, out_size, "%" PRIuMAX, (uintmax_t)row->uid);
            return;
        case BX_PS_FIELD_USER:
            bx_ps_format_user(row->uid, out, out_size);
            return;
        case BX_PS_FIELD_GID:
            if (row->gid == (gid_t)-1)
                snprintf(out, out_size, "?");
            else
                snprintf(out, out_size, "%" PRIuMAX, (uintmax_t)row->gid);
            return;
        case BX_PS_FIELD_GROUP:
            bx_ps_format_group(row->gid, out, out_size);
            return;
        case BX_PS_FIELD_PRI:
            if (row->have_stat)
                snprintf(out, out_size, "%ld", row->priority);
            else
                snprintf(out, out_size, "?");
            return;
        case BX_PS_FIELD_NI:
            if (row->have_stat)
                snprintf(out, out_size, "%ld", row->nice);
            else
                snprintf(out, out_size, "?");
            return;
        case BX_PS_FIELD_VSZ:
            if (row->have_stat)
                snprintf(out, out_size, "%llu", row->vsize_bytes / 1024u);
            else
                snprintf(out, out_size, "?");
            return;
        case BX_PS_FIELD_RSS:
            if (row->have_stat && row->rss_pages >= 0) {
                long page_size = sysconf(_SC_PAGESIZE);
                unsigned long long bytes;
                if (page_size <= 0)
                    page_size = 4096;
                bytes = (unsigned long long)row->rss_pages * (unsigned long long)page_size;
                snprintf(out, out_size, "%llu", bytes / 1024u);
            }
            else {
                snprintf(out, out_size, "?");
            }
            return;
        case BX_PS_FIELD_TIME:
            hz = bx_proc_clock_ticks_per_second();
            if (hz <= 0)
                hz = 100;
            total_ticks = row->utime_ticks > ULLONG_MAX - row->stime_ticks
                              ? ULLONG_MAX
                              : row->utime_ticks + row->stime_ticks;
            bx_ps_format_duration(total_ticks / (unsigned long long)hz, out, out_size);
            return;
        case BX_PS_FIELD_ETIME: {
            double uptime;
            hz = bx_proc_clock_ticks_per_second();
            if (hz <= 0)
                hz = 100;
            if (row->have_stat && bx_proc_uptime_seconds(&uptime)) {
                unsigned long long start_seconds =
                    row->starttime_ticks / (unsigned long long)hz;
                unsigned long long elapsed = 0;
                if (uptime > (double)start_seconds)
                    elapsed = (unsigned long long)(uptime - (double)start_seconds);
                bx_ps_format_duration(elapsed, out, out_size);
            }
            else {
                snprintf(out, out_size, "?");
            }
            return;
        }
    }

    snprintf(out, out_size, "?");
}

static bool bx_ps_print_rows(const struct bx_ps_options* options, const struct bx_ps_row* rows,
                             size_t row_count) {
    bool any_header = false;

    for (size_t i = 0; i < options->column_count; i++) {
        if (options->columns[i].show_header) {
            any_header = true;
            break;
        }
    }

    if (any_header) {
        for (size_t i = 0; i < options->column_count; i++) {
            if (i > 0)
                fputc(' ', stdout);
            if (options->columns[i].show_header)
                fputs(options->columns[i].header, stdout);
        }
        fputc('\n', stdout);
    }

    for (size_t row_index = 0; row_index < row_count; row_index++) {
        for (size_t i = 0; i < options->column_count; i++) {
            char value[512];
            if (i > 0)
                fputc(' ', stdout);
            switch (options->columns[i].field) {
                case BX_PS_FIELD_TTY:
                    fputs(rows[row_index].tty ? rows[row_index].tty : "?", stdout);
                    break;
                case BX_PS_FIELD_COMM:
                    fputs(rows[row_index].comm ? rows[row_index].comm : "?", stdout);
                    break;
                case BX_PS_FIELD_ARGS:
                    fputs(rows[row_index].args ? rows[row_index].args : "?", stdout);
                    break;
                default:
                    bx_ps_render_column(&options->columns[i], &rows[row_index], value, sizeof(value));
                    fputs(value, stdout);
                    break;
            }
        }
        fputc('\n', stdout);
    }

    return fflush(stdout) == 0 && !ferror(stdout);
}

int bx_ps_main(int argc, char** argv) {
    struct bx_ps_options options;
    struct bx_diag_ctx diag = {
        .progname = "ps",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };
    int rc = 1;

    if (!bx_ps_parse_options(argc, argv, &options, &diag)) {
        bx_cli_print_try_help(options.progname);
        rc = (diag.exit_status != 0) ? diag.exit_status : 1;
        goto done;
    }

    if (options.show_help) {
        bx_ps_print_help(stdout, options.progname);
        rc = 0;
        goto done;
    }

    if (options.show_version) {
        bx_cli_print_version(options.progname);
        rc = 0;
        goto done;
    }

    struct bx_ps_row* rows = NULL;
    size_t row_count = 0;
    if (!bx_ps_collect_rows(&options, &rows, &row_count, &diag)) {
        rc = (diag.exit_status != 0) ? diag.exit_status : 1;
        goto done;
    }

    if (!bx_ps_print_rows(&options, rows, row_count)) {
        bx_diag(&diag, "write error: %s", strerror(errno ? errno : EIO));
        rc = (diag.exit_status != 0) ? diag.exit_status : 1;
    }
    else {
        rc = 0;
    }
    bx_ps_free_rows(rows, row_count);

done:
    bx_ps_free_options(&options);
    return rc;
}
