#define _GNU_SOURCE

#include "mount_table.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "bx/libbx.h"
#include "fd_ops.h"

#ifdef __linux__
#include "path_ops.h"
#include "size_parse.h"
#include "statx_compat.h"
#endif

static void bx_mount_entry_free(struct bx_mount_entry* entry) {
    free(entry->source);
    free(entry->target);
    free(entry->fstype);
    free(entry->options);
    memset(entry, 0, sizeof(*entry));
}

static bool bx_mount_table_append(struct bx_mount_table* table, struct bx_mount_entry* entry) {
    if (table->len == table->cap) {
        size_t new_cap = table->cap == 0 ? 16 : table->cap * 2;
        struct bx_mount_entry* new_entries = xrealloc(table->entries, new_cap * sizeof(*new_entries));
        table->entries = new_entries;
        table->cap = new_cap;
    }
    table->entries[table->len++] = *entry;
    memset(entry, 0, sizeof(*entry));
    return true;
}

static int bx_mount_parse_octal_escape(const char* text) {
    int value = 0;
    for (int i = 0; i < 3; i++) {
        char ch = text[i];
        if (ch < '0' || ch > '7') {
            return -1;
        }
        value = (value << 3) | (ch - '0');
    }
    return value;
}

char* bx_mount_unescape_field(const char* text) {
    size_t len = strlen(text);
    char* out = xmalloc(len + 1);
    size_t j = 0;

    for (size_t i = 0; i < len; i++) {
        if (text[i] == '\\' && i + 3 < len) {
            int escaped = bx_mount_parse_octal_escape(text + i + 1);
            if (escaped >= 0) {
                out[j++] = (char)escaped;
                i += 3;
                continue;
            }
        }
        out[j++] = text[i];
    }

    out[j] = '\0';
    return out;
}

static bool fstab_field_valid(const char* field) {
    for (size_t i = 0; field[i]; i++) {
        if (field[i] != '\\')
            continue;
        const char* escape = field + i + 1;
        if (strncmp(escape, "040", 3) && strncmp(escape, "011", 3) &&
            strncmp(escape, "012", 3) && strncmp(escape, "134", 3))
            return false;
        i += 3;
    }
    return true;
}

bool bx_mount_table_load_fstab(struct bx_mount_table* table, const char* path, size_t* line) {
    enum { LINE_LIMIT = 8192, BYTE_LIMIT = 1024 * 1024, ROW_LIMIT = 1024 };
    memset(table, 0, sizeof(*table));
    *line = 0;
    int fd = bx_fd_open_cloexec(path, O_RDONLY | O_NONBLOCK | O_NOCTTY, 0);
    if (fd < 0)
        return false;
    struct stat st;
    bool stat_ok = fstat(fd, &st) == 0;
    if (!stat_ok || !S_ISREG(st.st_mode)) {
        int error = stat_ok ? EINVAL : errno;
        bx_fd_cleanup(&fd);
        errno = error;
        return false;
    }
    FILE* fp = fdopen(fd, "r");
    if (!fp) {
        int error = errno;
        bx_fd_cleanup(&fd);
        errno = error;
        return false;
    }
    size_t bytes = 0;
    int error = 0;
    for (;;) {
        char buf[LINE_LIMIT + 1];
        size_t len = 0;
        int ch;
        (*line)++;
        while ((ch = getc(fp)) != EOF) {
            if (++bytes > BYTE_LIMIT || (ch != '\n' && len == LINE_LIMIT)) {
                error = EFBIG;
                goto done;
            }
            if (ch == '\n')
                break;
            if (ch == '\0') {
                error = EINVAL;
                goto done;
            }
            buf[len++] = (char)ch;
        }
        if (ch == EOF) {
            if (ferror(fp))
                error = errno;
            if (len == 0 || error)
                break;
        }
        buf[len] = '\0';
        char* save;
        char* fields[6];
        size_t count = 0;
        for (char* token = strtok_r(buf, " \t\r", &save); token && *token != '#';
             token = strtok_r(NULL, " \t\r", &save)) {
            if (count == 6 || (count < 4 ? !fstab_field_valid(token)
                                        : token[strspn(token, "0123456789")] != '\0')) {
                error = EINVAL;
                goto done;
            }
            fields[count++] = token;
        }
        if (count == 0)
            continue;
        if (count < 4 || table->len == ROW_LIMIT) {
            error = count < 4 ? EINVAL : EFBIG;
            break;
        }
        struct bx_mount_entry entry = {
            .source = bx_mount_unescape_field(fields[0]),
            .target = bx_mount_unescape_field(fields[1]),
            .fstype = bx_mount_unescape_field(fields[2]),
            .options = bx_mount_unescape_field(fields[3]),
            .line = *line,
        };
        bx_mount_table_append(table, &entry);
    }
done:
    if (fclose(fp) != 0 && error == 0)
        error = errno;
    if (error) {
        bx_mount_table_free(table);
        errno = error;
        return false;
    }
    return true;
}

bool bx_mount_table_load(struct bx_mount_table* table) {
    memset(table, 0, sizeof(*table));

    FILE* fp = fopen("/proc/self/mounts", "r");
    if (fp == NULL) {
        return false;
    }

    char* line = NULL;
    size_t linecap = 0;
    bool ok = true;

    while (getline(&line, &linecap, fp) >= 0) {
        char* saveptr = NULL;
        char* source = strtok_r(line, " \t\r\n", &saveptr);
        char* target = strtok_r(NULL, " \t\r\n", &saveptr);
        char* fstype = strtok_r(NULL, " \t\r\n", &saveptr);
        char* options = strtok_r(NULL, " \t\r\n", &saveptr);
        if (source == NULL || target == NULL || fstype == NULL || options == NULL) {
            continue;
        }

        struct bx_mount_entry entry = {
            .source = bx_mount_unescape_field(source),
            .target = bx_mount_unescape_field(target),
            .fstype = bx_mount_unescape_field(fstype),
            .options = bx_mount_unescape_field(options),
        };
        if (!bx_mount_table_append(table, &entry)) {
            bx_mount_entry_free(&entry);
            ok = false;
            break;
        }
    }

    free(line);
    fclose(fp);

    if (!ok) {
        bx_mount_table_free(table);
    }
    return ok;
}

void bx_mount_table_free(struct bx_mount_table* table) {
    if (table == NULL) {
        return;
    }
    for (size_t i = 0; i < table->len; i++) {
        bx_mount_entry_free(&table->entries[i]);
    }
    free(table->entries);
    memset(table, 0, sizeof(*table));
}

const struct bx_mount_entry* bx_mount_table_find_target(const struct bx_mount_table* table, const char* target) {
    if (table == NULL || target == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < table->len; i++) {
        if (strcmp(table->entries[i].target, target) == 0) {
            return &table->entries[i];
        }
    }
    return NULL;
}

bool bx_mount_table_is_target_or_child(const char* parent, const char* candidate) {
    size_t parent_len;

    if (parent == NULL || candidate == NULL) {
        return false;
    }
    if (strcmp(parent, candidate) == 0) {
        return true;
    }

    parent_len = strlen(parent);
    if (parent_len == 0) {
        return false;
    }
    if (strcmp(parent, "/") == 0) {
        return candidate[0] == '/';
    }
    if (strncmp(parent, candidate, parent_len) != 0) {
        return false;
    }
    return candidate[parent_len] == '/';
}

#ifdef __linux__
#ifndef STATX_ATTR_MOUNT_ROOT
#define STATX_ATTR_MOUNT_ROOT 0x2000
#endif

/* Match both the path and its visible mount: a covered mount can still
 * appear in mountinfo at a path below the covering mount. */
static int lookup_mount(int fd) {
    static const char id_field[] = "mnt_id:";
    char proc_path[64];
    snprintf(proc_path, sizeof(proc_path), "/proc/self/fdinfo/%d", fd);
    FILE* fp = fopen(proc_path, "r");
    if (!fp)
        return -1;
    uintmax_t mount_id;
    char field[128];
    bool found = false;
    int err = EOPNOTSUPP;
    while (fgets(field, sizeof(field), fp)) {
        if (strncmp(field, id_field, sizeof(id_field) - 1) != 0)
            continue;
        char* value = field + sizeof(id_field) - 1;
        value += strspn(value, " \t");
        char* newline = strchr(value, '\n');
        if (newline) {
            *newline = '\0';
            found = bx_size_parse_uint(value, &mount_id) && mount_id != 0;
        }
        if (!found)
            err = EINVAL;
        break;
    }
    if (ferror(fp))
        err = errno;
    fclose(fp);
    if (!found) {
        errno = err;
        return -1;
    }
    snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d", fd);
    char* path = bx_path_readlink_dup(proc_path);
    if (!path)
        return -1;
    fp = fopen("/proc/self/mountinfo", "r");
    int result = -1;
    if (fp) {
        char* line = NULL;
        size_t cap = 0;
        err = EOPNOTSUPP;
        while (getline(&line, &cap, fp) >= 0) {
            char* rest;
            char* token = strtok_r(line, " \t\n", &rest);
            uintmax_t id;
            if (!bx_size_parse_uint(token, &id) || id != mount_id)
                continue;
            /* Skip parent ID, device number, and root to reach mountpoint. */
            for (int i = 0; i < 4 && token; i++)
                token = strtok_r(NULL, " \t\n", &rest);
            if (!token) {
                err = EINVAL;
                break;
            }
            char* target = bx_mount_unescape_field(token);
            result = strcmp(path, target) == 0;
            free(target);
            break;
        }
        if (ferror(fp))
            err = errno;
        free(line);
        fclose(fp);
    }
    else
        err = errno;
    free(path);
    errno = err;
    return result;
}

int bx_mount_is_mountpoint(const char* path) {
    struct statx info;
    if (statx(AT_FDCWD, path, 0, STATX_TYPE, &info) == 0) {
        if (info.stx_attributes_mask & STATX_ATTR_MOUNT_ROOT)
            return (info.stx_attributes & STATX_ATTR_MOUNT_ROOT) != 0;
    }
    else if (errno != ENOSYS && errno != EOPNOTSUPP && errno != EINVAL)
        return -1;
    int fd = bx_fd_open_cloexec(path, O_PATH, 0);
    if (fd < 0)
        return -1;
    int result = lookup_mount(fd);
    int err = errno;
    close(fd);
    errno = err;
    return result;
}
#endif
