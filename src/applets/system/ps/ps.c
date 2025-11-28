#define _GNU_SOURCE

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"
#include "bx/libbx.h"
#include "lib/cli_common.h"

struct bx_ps_options {
    const char* progname;
    bool show_help;
    bool show_version;
};

struct bx_ps_row {
    long pid;
    long ppid;
    char state;
    char* tty;
    char* command;
};

enum bx_ps_row_status {
    BX_PS_ROW_OK = 0,
    BX_PS_ROW_SKIP,
};

static void bx_ps_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]...\n", progname);
    fprintf(stream, "Display a simple /proc process listing.\n");
    fprintf(stream, "\n");
    fprintf(stream, "Columns: PID, PPID, STAT, TTY, COMMAND.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -h, --help     display this help and exit\n");
    fprintf(stream, "  -V, --version  output version information and exit\n");
}

static bool bx_ps_parse_options(int argc, char** argv, struct bx_ps_options* options, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"help", no_argument, NULL, 'h'},
        {"version", no_argument, NULL, 'V'},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "ps");
    diag->progname = options->progname;

    opterr = 0;
    optind = 1;

    while (true) {
        int c = getopt_long(argc, argv, "+hV", long_options, NULL);
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

    return true;
}

static bool bx_ps_make_proc_path(char* path, size_t path_size, long pid, const char* leaf) {
    int rc = snprintf(path, path_size, "/proc/%ld/%s", pid, leaf);
    return rc > 0 && (size_t)rc < path_size;
}

static bool bx_ps_parse_pid_name(const char* name, long* pid_out) {
    if (name == NULL || name[0] == '\0') {
        return false;
    }

    for (const char* p = name; *p != '\0'; p++) {
        if (!isdigit((unsigned char)*p)) {
            return false;
        }
    }

    errno = 0;
    char* end = NULL;
    long pid = strtol(name, &end, 10);
    if (errno != 0 || end == name || *end != '\0' || pid <= 0) {
        return false;
    }

    *pid_out = pid;
    return true;
}

static void bx_ps_parse_status_line(const char* line, long* ppid_out, char* state_out) {
    if (strncmp(line, "PPid:", 5) == 0) {
        const char* p = line + 5;
        while (*p != '\0' && isspace((unsigned char)*p)) {
            p++;
        }

        errno = 0;
        char* end = NULL;
        long value = strtol(p, &end, 10);
        if (errno == 0 && end != p) {
            *ppid_out = value;
        }
        return;
    }

    if (strncmp(line, "State:", 6) == 0) {
        const char* p = line + 6;
        while (*p != '\0' && isspace((unsigned char)*p)) {
            p++;
        }

        if (*p != '\0' && *p != '\n') {
            *state_out = *p;
        }
    }
}

static void bx_ps_read_status(long pid, long* ppid_out, char* state_out, bool* vanished_out) {
    *ppid_out = -1;
    *state_out = '?';
    *vanished_out = false;

    char path[64];
    if (!bx_ps_make_proc_path(path, sizeof(path), pid, "status")) {
        return;
    }

    FILE* stream = fopen(path, "r");
    if (stream == NULL) {
        if (errno == ENOENT) {
            *vanished_out = true;
        }
        return;
    }

    char* line = NULL;
    size_t line_cap = 0;
    while (getline(&line, &line_cap, stream) != -1) {
        bx_ps_parse_status_line(line, ppid_out, state_out);
    }

    if (ferror(stream) && errno == ENOENT) {
        *vanished_out = true;
    }

    free(line);
    fclose(stream);
}

static char* bx_ps_read_cmdline(long pid, bool* vanished_out) {
    *vanished_out = false;

    char path[64];
    if (!bx_ps_make_proc_path(path, sizeof(path), pid, "cmdline")) {
        return NULL;
    }

    FILE* stream = fopen(path, "rb");
    if (stream == NULL) {
        if (errno == ENOENT) {
            *vanished_out = true;
        }
        return NULL;
    }

    size_t cap = 256;
    size_t len = 0;
    char* buf = xmalloc(cap + 1);

    while (true) {
        if (len == cap) {
            cap *= 2;
            buf = xrealloc(buf, cap + 1);
        }

        size_t read_count = fread(buf + len, 1, cap - len, stream);
        len += read_count;
        if (read_count == 0) {
            break;
        }
    }

    if (ferror(stream)) {
        if (errno == ENOENT) {
            *vanished_out = true;
        }
        free(buf);
        fclose(stream);
        return NULL;
    }

    fclose(stream);

    if (len == 0) {
        free(buf);
        return NULL;
    }

    size_t write_index = 0;
    bool previous_was_space = false;
    for (size_t i = 0; i < len; i++) {
        if (buf[i] == '\0') {
            if (!previous_was_space && i + 1 < len) {
                buf[write_index++] = ' ';
                previous_was_space = true;
            }
            continue;
        }

        buf[write_index++] = buf[i];
        previous_was_space = false;
    }

    while (write_index > 0 && buf[write_index - 1] == ' ') {
        write_index--;
    }

    if (write_index == 0) {
        free(buf);
        return NULL;
    }

    buf[write_index] = '\0';
    return buf;
}

static char* bx_ps_read_comm(long pid, bool* vanished_out) {
    *vanished_out = false;

    char path[64];
    if (!bx_ps_make_proc_path(path, sizeof(path), pid, "comm")) {
        return NULL;
    }

    FILE* stream = fopen(path, "r");
    if (stream == NULL) {
        if (errno == ENOENT) {
            *vanished_out = true;
        }
        return NULL;
    }

    char* line = NULL;
    size_t line_cap = 0;
    ssize_t line_len = getline(&line, &line_cap, stream);
    if (line_len < 0) {
        if (ferror(stream) && errno == ENOENT) {
            *vanished_out = true;
        }
        free(line);
        fclose(stream);
        return NULL;
    }

    fclose(stream);

    while (line_len > 0 && (line[line_len - 1] == '\n' || line[line_len - 1] == '\r')) {
        line_len--;
    }
    line[line_len] = '\0';

    if (line_len == 0) {
        free(line);
        return NULL;
    }

    return line;
}

static char* bx_ps_read_tty(long pid, bool* vanished_out) {
    *vanished_out = false;

    char path[64];
    if (!bx_ps_make_proc_path(path, sizeof(path), pid, "fd/0")) {
        return NULL;
    }

    char target[4096];
    ssize_t len = readlink(path, target, sizeof(target) - 1);
    if (len < 0) {
        if (errno == ENOENT) {
            *vanished_out = true;
        }
        return NULL;
    }

    target[len] = '\0';

    if (strncmp(target, "/dev/", 5) == 0 && target[5] != '\0') {
        return xstrdup(target + 5);
    }

    return xstrdup(target);
}

static enum bx_ps_row_status bx_ps_collect_row(long pid, struct bx_ps_row* row) {
    memset(row, 0, sizeof(*row));
    row->pid = pid;
    row->ppid = -1;
    row->state = '?';

    bool vanished = false;
    bx_ps_read_status(pid, &row->ppid, &row->state, &vanished);
    if (vanished) {
        return BX_PS_ROW_SKIP;
    }

    row->tty = bx_ps_read_tty(pid, &vanished);
    if (vanished) {
        return BX_PS_ROW_SKIP;
    }
    if (row->tty == NULL) {
        row->tty = xstrdup("?");
    }

    row->command = bx_ps_read_cmdline(pid, &vanished);
    if (vanished) {
        free(row->tty);
        row->tty = NULL;
        return BX_PS_ROW_SKIP;
    }

    if (row->command == NULL) {
        row->command = bx_ps_read_comm(pid, &vanished);
        if (vanished) {
            free(row->tty);
            row->tty = NULL;
            return BX_PS_ROW_SKIP;
        }
    }

    if (row->command == NULL) {
        row->command = xstrdup("?");
    }

    return BX_PS_ROW_OK;
}

static void bx_ps_free_rows(struct bx_ps_row* rows, size_t row_count) {
    for (size_t i = 0; i < row_count; i++) {
        free(rows[i].tty);
        free(rows[i].command);
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

static bool bx_ps_collect_rows(struct bx_ps_row** rows_out, size_t* row_count_out, struct bx_diag_ctx* diag) {
    *rows_out = NULL;
    *row_count_out = 0;

    DIR* proc_dir = opendir("/proc");
    if (proc_dir == NULL) {
        bx_diag(diag, "failed to open /proc: %s", strerror(errno));
        return false;
    }

    struct bx_ps_row* rows = NULL;
    size_t row_count = 0;
    size_t row_cap = 0;

    struct dirent* entry = NULL;
    while (true) {
        errno = 0;
        entry = readdir(proc_dir);
        if (entry == NULL) {
            break;
        }

        long pid = -1;
        if (!bx_ps_parse_pid_name(entry->d_name, &pid)) {
            continue;
        }

        struct bx_ps_row row;
        enum bx_ps_row_status status = bx_ps_collect_row(pid, &row);
        if (status == BX_PS_ROW_SKIP) {
            continue;
        }

        if (row_count == row_cap) {
            row_cap = (row_cap == 0) ? 64 : row_cap * 2;
            rows = xrealloc(rows, row_cap * sizeof(*rows));
        }

        rows[row_count++] = row;
    }

    if (errno != 0) {
        bx_diag(diag, "failed while reading /proc: %s", strerror(errno));
        bx_ps_free_rows(rows, row_count);
        closedir(proc_dir);
        return false;
    }

    closedir(proc_dir);

    qsort(rows, row_count, sizeof(*rows), bx_ps_compare_rows);
    *rows_out = rows;
    *row_count_out = row_count;
    return true;
}

static void bx_ps_print_rows(const struct bx_ps_row* rows, size_t row_count) {
    printf("PID PPID STAT TTY COMMAND\n");

    for (size_t i = 0; i < row_count; i++) {
        char ppid_buf[32];
        if (rows[i].ppid >= 0) {
            snprintf(ppid_buf, sizeof(ppid_buf), "%ld", rows[i].ppid);
        }
        else {
            strcpy(ppid_buf, "?");
        }

        printf("%ld %s %c %s %s\n", rows[i].pid, ppid_buf, rows[i].state, rows[i].tty, rows[i].command);
    }
}

int bx_ps_main(int argc, char** argv) {
    struct bx_ps_options options;
    struct bx_diag_ctx diag = {
        .progname = "ps",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };

    if (!bx_ps_parse_options(argc, argv, &options, &diag)) {
        bx_cli_print_try_help(options.progname);
        return (diag.exit_status != 0) ? diag.exit_status : 1;
    }

    if (options.show_help) {
        bx_ps_print_help(stdout, options.progname);
        return 0;
    }

    if (options.show_version) {
        bx_cli_print_version(options.progname);
        return 0;
    }

    struct bx_ps_row* rows = NULL;
    size_t row_count = 0;
    if (!bx_ps_collect_rows(&rows, &row_count, &diag)) {
        return (diag.exit_status != 0) ? diag.exit_status : 1;
    }

    bx_ps_print_rows(rows, row_count);
    bx_ps_free_rows(rows, row_count);
    return 0;
}
