#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "applets/system/psmisc/procfs.h"
#include "bx/libbx.h"

struct bx_proc_buffer {
    unsigned char* data;
    size_t len;
    size_t cap;
};

static bool bx_proc_errno_is_vanished(int errnum) {
    return errnum == ENOENT || errnum == ESRCH;
}

static void bx_proc_buffer_init(struct bx_proc_buffer* buffer) {
    buffer->data = NULL;
    buffer->len = 0u;
    buffer->cap = 0u;
}

static void bx_proc_buffer_free(struct bx_proc_buffer* buffer) {
    free(buffer->data);
    buffer->data = NULL;
    buffer->len = 0u;
    buffer->cap = 0u;
}

static bool bx_proc_buffer_reserve(struct bx_proc_buffer* buffer, size_t extra) {
    size_t need;
    size_t cap;

    if (extra == 0u) {
        return true;
    }
    if (buffer->len > SIZE_MAX - extra) {
        errno = EOVERFLOW;
        return false;
    }
    need = buffer->len + extra;
    if (need <= buffer->cap) {
        return true;
    }
    cap = buffer->cap ? buffer->cap : 256u;
    while (cap < need) {
        if (cap > SIZE_MAX / 2u) {
            cap = need;
            break;
        }
        cap *= 2u;
    }
    buffer->data = xrealloc(buffer->data, cap);
    buffer->cap = cap;
    return true;
}

static bool bx_proc_buffer_append(struct bx_proc_buffer* buffer, const void* data, size_t len) {
    if (!bx_proc_buffer_reserve(buffer, len)) {
        return false;
    }
    memcpy(buffer->data + buffer->len, data, len);
    buffer->len += len;
    return true;
}

static bool bx_proc_make_path(char* path, size_t path_size, pid_t pid, const char* leaf) {
    int rc = snprintf(path, path_size, "/proc/%ld/%s", (long)pid, leaf);
    return rc > 0 && (size_t)rc < path_size;
}

static bool bx_proc_name_is_pid(const char* name) {
    size_t i;
    if (name == NULL || name[0] == '\0') {
        return false;
    }
    for (i = 0u; name[i] != '\0'; i++) {
        if (!isdigit((unsigned char)name[i])) {
            return false;
        }
    }
    return true;
}

static bool bx_proc_parse_pid_number(const char* text, const char** end_out, bool require_full, pid_t* pid_out) {
    char* end = NULL;
    intmax_t value;

    if (text == NULL || text[0] == '\0' || !isdigit((unsigned char)text[0]) || pid_out == NULL) {
        return false;
    }

    errno = 0;
    value = strtoimax(text, &end, 10);
    if (errno == ERANGE || end == text || end == NULL || value <= 0 || value > INT_MAX) {
        return false;
    }
    if (require_full && *end != '\0') {
        return false;
    }

    *pid_out = (pid_t)value;
    if (end_out != NULL) {
        *end_out = end;
    }
    return true;
}

static bool bx_proc_parse_int_number(const char* text, int* value_out) {
    char* end = NULL;
    intmax_t value;

    if (text == NULL || text[0] == '\0' || value_out == NULL) {
        return false;
    }

    errno = 0;
    value = strtoimax(text, &end, 10);
    if (errno == ERANGE || end == text || end == NULL || *end != '\0'
        || value < INT_MIN || value > INT_MAX) {
        return false;
    }

    *value_out = (int)value;
    return true;
}

static bool bx_proc_parse_uid_token(const char* text, uid_t* uid_out) {
    char* end = NULL;
    uintmax_t value;

    if (text == NULL || text[0] == '\0' || uid_out == NULL) {
        return false;
    }

    errno = 0;
    value = strtoumax(text, &end, 10);
    if (errno == ERANGE || end == text || end == NULL || value > (uintmax_t)((uid_t)-1)) {
        return false;
    }
    if (*end != '\0' && !isspace((unsigned char)*end)) {
        return false;
    }

    *uid_out = (uid_t)value;
    return true;
}

bool bx_proc_parse_pid_arg(const char* text, pid_t* pid_out) {
    const char* number = text;

    if (text == NULL || pid_out == NULL) {
        return false;
    }
    if (strncmp(text, "/proc/", 6u) == 0) {
        number = text + 6;
    }
    if (!bx_proc_name_is_pid(number)) {
        return false;
    }
    return bx_proc_parse_pid_number(number, NULL, true, pid_out);
}

static bool bx_proc_read_open_fd(int fd, struct bx_proc_buffer* buffer, bool* vanished_out) {
    unsigned char chunk[4096];

    *vanished_out = false;
    while (true) {
        ssize_t nread = read(fd, chunk, sizeof(chunk));
        if (nread == 0) {
            return true;
        }
        if (nread < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (bx_proc_errno_is_vanished(errno)) {
                *vanished_out = true;
            }
            return false;
        }
        if (!bx_proc_buffer_append(buffer, chunk, (size_t)nread)) {
            return false;
        }
    }
}

bool bx_proc_read_text_file(pid_t pid, const char* leaf, char** text_out, bool* vanished_out) {
    char path[128];
    int fd;
    struct bx_proc_buffer buffer;

    *text_out = NULL;
    *vanished_out = false;
    if (!bx_proc_make_path(path, sizeof(path), pid, leaf)) {
        errno = ENAMETOOLONG;
        return false;
    }
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (bx_proc_errno_is_vanished(errno)) {
            *vanished_out = true;
        }
        return false;
    }

    bx_proc_buffer_init(&buffer);
    if (!bx_proc_read_open_fd(fd, &buffer, vanished_out)) {
        int saved_errno = errno;
        close(fd);
        bx_proc_buffer_free(&buffer);
        errno = saved_errno;
        return false;
    }
    close(fd);

    if (!bx_proc_buffer_append(&buffer, "", 1u)) {
        int saved_errno = errno;
        bx_proc_buffer_free(&buffer);
        errno = saved_errno;
        return false;
    }
    *text_out = (char*)buffer.data;
    return true;
}

static bool bx_proc_readlink_path(const char* path, char** target_out, bool* vanished_out) {
    size_t cap = 128u;
    char* target = NULL;

    *target_out = NULL;
    *vanished_out = false;
    while (true) {
        ssize_t len;
        target = xrealloc(target, cap + 1u);
        len = readlink(path, target, cap);
        if (len < 0) {
            if (bx_proc_errno_is_vanished(errno)) {
                *vanished_out = true;
            }
            free(target);
            return false;
        }
        if ((size_t)len < cap) {
            target[len] = '\0';
            *target_out = target;
            return true;
        }
        if (cap > SIZE_MAX / 2u) {
            free(target);
            errno = EOVERFLOW;
            return false;
        }
        cap *= 2u;
    }
}

bool bx_proc_readlink_leaf(pid_t pid, const char* leaf, char** target_out, bool* vanished_out) {
    char path[128];

    if (!bx_proc_make_path(path, sizeof(path), pid, leaf)) {
        errno = ENAMETOOLONG;
        *target_out = NULL;
        *vanished_out = false;
        return false;
    }
    return bx_proc_readlink_path(path, target_out, vanished_out);
}

static bool bx_proc_parse_unsigned_long_long(const char* text, unsigned long long* value_out) {
    char* end;
    uintmax_t value;

    errno = 0;
    value = strtoumax(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value > ULLONG_MAX) {
        return false;
    }
    *value_out = (unsigned long long)value;
    return true;
}

static bool bx_proc_parse_long_long(const char* text, long long* value_out) {
    char* end;
    intmax_t value;

    errno = 0;
    value = strtoimax(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < LLONG_MIN || value > LLONG_MAX) {
        return false;
    }
    *value_out = (long long)value;
    return true;
}

static bool bx_proc_token_end(const char* end) {
    return end != NULL && (*end == '\0' || isspace((unsigned char)*end));
}

static bool bx_proc_parse_fdinfo_position(const char* text, off_t* position_out) {
    if (text == NULL || text[0] == '\0' || text[0] == '-') {
        return false;
    }

    errno = 0;
    char* end = NULL;
    intmax_t value = strtoimax(text, &end, 10);
    if (errno != 0 || end == text || !bx_proc_token_end(end)) {
        return false;
    }

    off_t position = (off_t)value;
    if ((intmax_t)position != value) {
        return false;
    }

    *position_out = position;
    return true;
}

static bool bx_proc_parse_fdinfo_flags(const char* text, unsigned long* flags_out) {
    if (text == NULL || text[0] == '\0' || text[0] == '-') {
        return false;
    }

    errno = 0;
    char* end = NULL;
    uintmax_t value = strtoumax(text, &end, 8);
    if (errno != 0 || end == text || !bx_proc_token_end(end) || value > ULONG_MAX) {
        return false;
    }

    *flags_out = (unsigned long)value;
    return true;
}

static bool bx_proc_parse_stat_text(const char* text, struct bx_proc_stat* stat_out) {
    const char* lparen;
    const char* rparen;
    const char* pid_end = NULL;
    char* remainder;
    char* saveptr = NULL;
    char* token;
    char* fields[64];
    size_t field_count = 0u;
    long long signed_value;
    unsigned long long unsigned_value;

    memset(stat_out, 0, sizeof(*stat_out));

    if (!bx_proc_parse_pid_number(text, &pid_end, false, &stat_out->pid)) {
        return false;
    }

    lparen = strchr(text, '(');
    rparen = strrchr(text, ')');
    if (lparen == NULL || rparen == NULL || rparen <= lparen || rparen[1] != ' ') {
        return false;
    }
    while (pid_end < lparen && isspace((unsigned char)*pid_end)) {
        pid_end++;
    }
    if (pid_end != lparen) {
        return false;
    }
    stat_out->comm = xmalloc((size_t)(rparen - lparen));
    memcpy(stat_out->comm, lparen + 1, (size_t)(rparen - lparen - 1));
    stat_out->comm[rparen - lparen - 1] = '\0';

    remainder = xstrdup(rparen + 2);
    token = strtok_r(remainder, " ", &saveptr);
    while (token != NULL && field_count < sizeof(fields) / sizeof(fields[0])) {
        fields[field_count++] = token;
        token = strtok_r(NULL, " ", &saveptr);
    }

    if (field_count < 22u) {
        free(remainder);
        return false;
    }

    stat_out->state = fields[0][0];

    if (!bx_proc_parse_long_long(fields[1], &signed_value)) {
        free(remainder);
        return false;
    }
    stat_out->ppid = (pid_t)signed_value;

    if (!bx_proc_parse_long_long(fields[2], &signed_value)) {
        free(remainder);
        return false;
    }
    stat_out->pgrp = (pid_t)signed_value;

    if (!bx_proc_parse_long_long(fields[3], &signed_value)) {
        free(remainder);
        return false;
    }
    stat_out->session = (pid_t)signed_value;

    if (!bx_proc_parse_long_long(fields[4], &signed_value)) {
        free(remainder);
        return false;
    }
    stat_out->tty_nr = (long)signed_value;

    if (!bx_proc_parse_long_long(fields[5], &signed_value)) {
        free(remainder);
        return false;
    }
    stat_out->tpgid = (long)signed_value;

    if (!bx_proc_parse_unsigned_long_long(fields[6], &unsigned_value)) {
        free(remainder);
        return false;
    }
    stat_out->flags = (unsigned long)unsigned_value;

    if (!bx_proc_parse_unsigned_long_long(fields[11], &stat_out->utime_ticks)
        || !bx_proc_parse_unsigned_long_long(fields[12], &stat_out->stime_ticks)) {
        free(remainder);
        return false;
    }

    if (!bx_proc_parse_long_long(fields[15], &signed_value)) {
        free(remainder);
        return false;
    }
    stat_out->priority = (long)signed_value;

    if (!bx_proc_parse_long_long(fields[16], &signed_value)) {
        free(remainder);
        return false;
    }
    stat_out->nice = (long)signed_value;

    if (!bx_proc_parse_long_long(fields[17], &signed_value)) {
        free(remainder);
        return false;
    }
    stat_out->num_threads = (long)signed_value;

    if (!bx_proc_parse_unsigned_long_long(fields[19], &stat_out->starttime_ticks)
        || !bx_proc_parse_unsigned_long_long(fields[20], &stat_out->vsize_bytes)) {
        free(remainder);
        return false;
    }

    if (!bx_proc_parse_long_long(fields[21], &signed_value)) {
        free(remainder);
        return false;
    }
    stat_out->rss_pages = signed_value;

    free(remainder);
    return true;
}

bool bx_proc_read_stat(pid_t pid, struct bx_proc_stat* stat_out, bool* vanished_out) {
    char* text = NULL;
    bool ok;

    if (!bx_proc_read_text_file(pid, "stat", &text, vanished_out)) {
        return false;
    }
    ok = bx_proc_parse_stat_text(text, stat_out);
    free(text);
    if (!ok) {
        errno = EINVAL;
    }
    return ok;
}

bool bx_proc_read_uid(pid_t pid, uid_t* uid_out, bool* vanished_out) {
    char* text = NULL;
    char* line;
    bool ok = false;

    *uid_out = (uid_t)-1;
    if (!bx_proc_read_text_file(pid, "status", &text, vanished_out)) {
        return false;
    }
    line = strtok(text, "\n");
    while (line != NULL) {
        if (strncmp(line, "Uid:", 4u) == 0) {
            char* cursor = line + 4;
            while (*cursor != '\0' && isspace((unsigned char)*cursor)) {
                cursor++;
            }
            uid_t value = (uid_t)-1;
            if (bx_proc_parse_uid_token(cursor, &value)) {
                *uid_out = value;
                ok = true;
            }
            break;
        }
        line = strtok(NULL, "\n");
    }
    free(text);
    if (!ok) {
        errno = EINVAL;
    }
    return ok;
}

bool bx_proc_read_cmdline(pid_t pid, char** cmdline_out, bool* vanished_out) {
    char path[128];
    int fd;
    struct bx_proc_buffer buffer;
    size_t i;

    *cmdline_out = NULL;
    *vanished_out = false;
    if (!bx_proc_make_path(path, sizeof(path), pid, "cmdline")) {
        errno = ENAMETOOLONG;
        return false;
    }
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (bx_proc_errno_is_vanished(errno)) {
            *vanished_out = true;
        }
        return false;
    }

    bx_proc_buffer_init(&buffer);
    if (!bx_proc_read_open_fd(fd, &buffer, vanished_out)) {
        int saved_errno = errno;
        close(fd);
        bx_proc_buffer_free(&buffer);
        errno = saved_errno;
        return false;
    }
    close(fd);

    if (buffer.len == 0u) {
        bx_proc_buffer_free(&buffer);
        *cmdline_out = xstrdup("");
        return true;
    }

    for (i = 0u; i < buffer.len; i++) {
        if (buffer.data[i] == '\0') {
            buffer.data[i] = ' ';
        }
    }
    while (buffer.len > 0u && buffer.data[buffer.len - 1u] == ' ') {
        buffer.len--;
    }
    if (!bx_proc_buffer_append(&buffer, "", 1u)) {
        int saved_errno = errno;
        bx_proc_buffer_free(&buffer);
        errno = saved_errno;
        return false;
    }

    *cmdline_out = (char*)buffer.data;
    return true;
}

bool bx_proc_read_exe(pid_t pid, char** exe_out, bool* vanished_out) {
    return bx_proc_readlink_leaf(pid, "exe", exe_out, vanished_out);
}

static bool bx_proc_read_last_status_pid_field(pid_t pid,
                                               const char* key,
                                               pid_t fallback,
                                               pid_t* value_out,
                                               bool* vanished_out) {
    char* text = NULL;
    char* line;
    size_t key_len;
    bool found = false;

    *value_out = fallback;
    if (!bx_proc_read_text_file(pid, "status", &text, vanished_out)) {
        return false;
    }

    key_len = strlen(key);
    line = strtok(text, "\n");
    while (line != NULL) {
        if (strncmp(line, key, key_len) == 0) {
            const char* cursor = line + key_len;
            pid_t last_value = fallback;

            while (*cursor != '\0') {
                const char* end = NULL;
                pid_t value;

                while (*cursor != '\0' && isspace((unsigned char)*cursor)) {
                    cursor++;
                }
                if (*cursor == '\0') {
                    break;
                }

                if (!bx_proc_parse_pid_number(cursor, &end, false, &value)) {
                    free(text);
                    errno = EINVAL;
                    return false;
                }
                last_value = value;
                cursor = end;
                found = true;
            }

            if (found) {
                *value_out = last_value;
            }
            break;
        }
        line = strtok(NULL, "\n");
    }

    free(text);
    return true;
}

bool bx_proc_read_ns_pid(pid_t pid, pid_t* ns_pid_out, bool* vanished_out) {
    return bx_proc_read_last_status_pid_field(pid, "NSpid:", pid, ns_pid_out, vanished_out);
}

bool bx_proc_read_ns_pgid(pid_t pid, pid_t host_pgid, pid_t* ns_pgid_out, bool* vanished_out) {
    return bx_proc_read_last_status_pid_field(pid, "NSpgid:", host_pgid, ns_pgid_out, vanished_out);
}

bool bx_proc_read_info(pid_t pid, unsigned flags, struct bx_proc_info* info_out, bool* vanished_out) {
    struct bx_proc_stat stat_info;
    uid_t uid = (uid_t)-1;
    bool local_vanished = false;

    memset(info_out, 0, sizeof(*info_out));
    if (!bx_proc_read_stat(pid, &stat_info, &local_vanished)) {
        if (vanished_out != NULL) {
            *vanished_out = local_vanished;
        }
        return false;
    }

    info_out->pid = stat_info.pid;
    info_out->ppid = stat_info.ppid;
    info_out->pgrp = stat_info.pgrp;
    info_out->session = stat_info.session;
    info_out->state = stat_info.state;
    info_out->starttime_ticks = stat_info.starttime_ticks;
    info_out->comm = stat_info.comm;
    stat_info.comm = NULL;

    if (bx_proc_read_uid(pid, &uid, &local_vanished)) {
        info_out->uid = uid;
    }
    else if (local_vanished) {
        bx_proc_info_free(info_out);
        if (vanished_out != NULL) {
            *vanished_out = true;
        }
        bx_proc_stat_free(&stat_info);
        return false;
    }
    else {
        info_out->uid = (uid_t)-1;
    }

    if ((flags & BX_PROC_READ_CMDLINE) != 0u) {
        if (!bx_proc_read_cmdline(pid, &info_out->cmdline, &local_vanished)) {
            if (local_vanished) {
                bx_proc_info_free(info_out);
                if (vanished_out != NULL) {
                    *vanished_out = true;
                }
                bx_proc_stat_free(&stat_info);
                return false;
            }
        }
    }
    if ((flags & BX_PROC_READ_EXE) != 0u) {
        if (!bx_proc_read_exe(pid, &info_out->exe, &local_vanished)) {
            if (local_vanished) {
                bx_proc_info_free(info_out);
                if (vanished_out != NULL) {
                    *vanished_out = true;
                }
                bx_proc_stat_free(&stat_info);
                return false;
            }
        }
    }

    if (vanished_out != NULL) {
        *vanished_out = false;
    }
    bx_proc_stat_free(&stat_info);
    return true;
}

void bx_proc_stat_free(struct bx_proc_stat* stat_info) {
    free(stat_info->comm);
    stat_info->comm = NULL;
}

void bx_proc_info_free(struct bx_proc_info* info) {
    free(info->comm);
    free(info->cmdline);
    free(info->exe);
    info->comm = NULL;
    info->cmdline = NULL;
    info->exe = NULL;
}

void bx_proc_list_free(struct bx_proc_list* list) {
    size_t i;
    for (i = 0u; i < list->len; i++) {
        bx_proc_info_free(&list->items[i]);
    }
    free(list->items);
    list->items = NULL;
    list->len = 0u;
    list->cap = 0u;
}

static bool bx_proc_list_push(struct bx_proc_list* list, const struct bx_proc_info* info) {
    if (list->len == list->cap) {
        size_t next_cap = list->cap ? list->cap * 2u : 64u;
        list->items = xrealloc(list->items, next_cap * sizeof(*list->items));
        list->cap = next_cap;
    }
    list->items[list->len++] = *info;
    return true;
}

static int bx_proc_info_compare_pid(const void* left, const void* right) {
    const struct bx_proc_info* a = left;
    const struct bx_proc_info* b = right;
    if (a->pid < b->pid) {
        return -1;
    }
    if (a->pid > b->pid) {
        return 1;
    }
    return 0;
}

bool bx_proc_list_read(struct bx_proc_list* list, unsigned flags) {
    DIR* dir;
    struct dirent* ent;

    memset(list, 0, sizeof(*list));
    dir = opendir("/proc");
    if (dir == NULL) {
        return false;
    }

    while ((ent = readdir(dir)) != NULL) {
        struct bx_proc_info info;
        bool vanished = false;
        pid_t pid;

        if (!bx_proc_name_is_pid(ent->d_name)) {
            continue;
        }
        if (!bx_proc_parse_pid_arg(ent->d_name, &pid)) {
            continue;
        }
        if (!bx_proc_read_info(pid, flags, &info, &vanished)) {
            if (vanished || errno == EACCES || errno == EPERM) {
                continue;
            }
            bx_proc_list_free(list);
            closedir(dir);
            return false;
        }
        bx_proc_list_push(list, &info);
    }

    if (closedir(dir) != 0) {
        bx_proc_list_free(list);
        return false;
    }

    if (list->len > 1u) {
        qsort(list->items, list->len, sizeof(*list->items), bx_proc_info_compare_pid);
    }
    return true;
}

static void bx_proc_fd_entry_free(struct bx_proc_fd_entry* entry) {
    free(entry->target);
    entry->target = NULL;
}

void bx_proc_fd_list_free(struct bx_proc_fd_list* list) {
    size_t i;
    for (i = 0u; i < list->len; i++) {
        bx_proc_fd_entry_free(&list->items[i]);
    }
    free(list->items);
    list->items = NULL;
    list->len = 0u;
    list->cap = 0u;
}

static bool bx_proc_fd_list_push(struct bx_proc_fd_list* list, const struct bx_proc_fd_entry* entry) {
    if (list->len == list->cap) {
        size_t next_cap = list->cap ? list->cap * 2u : 16u;
        list->items = xrealloc(list->items, next_cap * sizeof(*list->items));
        list->cap = next_cap;
    }
    list->items[list->len++] = *entry;
    return true;
}

static int bx_proc_fd_compare(const void* left, const void* right) {
    const struct bx_proc_fd_entry* a = left;
    const struct bx_proc_fd_entry* b = right;
    return (a->fd > b->fd) - (a->fd < b->fd);
}

static void bx_proc_parse_fdinfo_text(struct bx_proc_fd_entry* entry, const char* text) {
    const char* cursor = text;

    while (*cursor != '\0') {
        if (strncmp(cursor, "pos:\t", 5u) == 0 || strncmp(cursor, "pos:", 4u) == 0) {
            const char* value = cursor + (cursor[4] == '\t' ? 5 : 4);
            off_t pos;
            while (*value != '\0' && isspace((unsigned char)*value)) {
                value++;
            }
            if (bx_proc_parse_fdinfo_position(value, &pos)) {
                entry->position = pos;
                entry->have_position = true;
            }
        }
        else if (strncmp(cursor, "flags:\t", 7u) == 0 || strncmp(cursor, "flags:", 6u) == 0) {
            const char* value = cursor + (cursor[6] == '\t' ? 7 : 6);
            unsigned long flags;
            while (*value != '\0' && isspace((unsigned char)*value)) {
                value++;
            }
            if (bx_proc_parse_fdinfo_flags(value, &flags)) {
                entry->flags = flags;
                entry->have_flags = true;
            }
        }
        while (*cursor != '\0' && *cursor != '\n') {
            cursor++;
        }
        if (*cursor == '\n') {
            cursor++;
        }
    }
}

bool bx_proc_read_fds(pid_t pid, struct bx_proc_fd_list* list, bool* vanished_out) {
    char dir_path[128];
    DIR* dir;
    struct dirent* ent;

    memset(list, 0, sizeof(*list));
    *vanished_out = false;
    if (!bx_proc_make_path(dir_path, sizeof(dir_path), pid, "fd")) {
        errno = ENAMETOOLONG;
        return false;
    }
    dir = opendir(dir_path);
    if (dir == NULL) {
        if (bx_proc_errno_is_vanished(errno)) {
            *vanished_out = true;
        }
        return false;
    }

    while ((ent = readdir(dir)) != NULL) {
        struct bx_proc_fd_entry entry;
        char path[160];
        char fdinfo_leaf[64];
        char* fdinfo_text = NULL;
        bool local_vanished = false;
        int value;

        if (!bx_proc_name_is_pid(ent->d_name)) {
            continue;
        }
        if (!bx_proc_parse_int_number(ent->d_name, &value) || value < 0) {
            continue;
        }

        memset(&entry, 0, sizeof(entry));
        entry.fd = value;

        if (!bx_proc_make_path(path, sizeof(path), pid, "fd")) {
            bx_proc_fd_list_free(list);
            closedir(dir);
            errno = ENAMETOOLONG;
            return false;
        }
        {
            int rc = snprintf(path, sizeof(path), "/proc/%ld/fd/%s", (long)pid, ent->d_name);
            if (rc <= 0 || (size_t)rc >= sizeof(path)) {
                bx_proc_fd_list_free(list);
                closedir(dir);
                errno = ENAMETOOLONG;
                return false;
            }
        }

        if (!bx_proc_readlink_path(path, &entry.target, &local_vanished)) {
            if (local_vanished || errno == EACCES || errno == EPERM) {
                continue;
            }
            bx_proc_fd_list_free(list);
            closedir(dir);
            return false;
        }
        if (stat(path, &entry.st) == 0) {
            entry.have_stat = true;
        }

        snprintf(fdinfo_leaf, sizeof(fdinfo_leaf), "fdinfo/%d", entry.fd);
        if (bx_proc_read_text_file(pid, fdinfo_leaf, &fdinfo_text, &local_vanished)) {
            bx_proc_parse_fdinfo_text(&entry, fdinfo_text);
            free(fdinfo_text);
        }

        bx_proc_fd_list_push(list, &entry);
    }

    if (closedir(dir) != 0) {
        bx_proc_fd_list_free(list);
        return false;
    }
    if (list->len > 1u) {
        qsort(list->items, list->len, sizeof(*list->items), bx_proc_fd_compare);
    }
    return true;
}

bool bx_proc_uptime_seconds(double* uptime_out) {
#ifdef CLOCK_BOOTTIME
    struct timespec ts;
    if (clock_gettime(CLOCK_BOOTTIME, &ts) == 0) {
        *uptime_out = (double)ts.tv_sec + ((double)ts.tv_nsec / 1000000000.0);
        return true;
    }
#endif
    FILE* stream = fopen("/proc/uptime", "r");
    if (stream == NULL) {
        return false;
    }
    if (fscanf(stream, "%lf", uptime_out) != 1) {
        fclose(stream);
        errno = EINVAL;
        return false;
    }
    fclose(stream);
    return true;
}

long bx_proc_clock_ticks_per_second(void) {
    long ticks = sysconf(_SC_CLK_TCK);
    return ticks > 0 ? ticks : 100L;
}

char* bx_proc_basename_dup(const char* path) {
    const char* base;
    if (path == NULL) {
        return NULL;
    }
    base = strrchr(path, '/');
    if (base != NULL && base[1] != '\0') {
        return xstrdup(base + 1);
    }
    return xstrdup(path);
}

pid_t bx_proc_self_host_pid(void) {
    char path[64];
    ssize_t len;

    len = readlink("/proc/self", path, sizeof(path) - 1u);
    if (len <= 0) {
        return getpid();
    }
    path[len] = '\0';

    {
        pid_t value;

        if (!bx_proc_parse_pid_number(path, NULL, true, &value)) {
            return getpid();
        }
        return value;
    }
}
