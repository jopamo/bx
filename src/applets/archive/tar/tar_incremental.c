#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>

#include "applets/archive/archive_common.h"
#include "applets/archive/archive_fs.h"
#include "applets/archive/tar/tar_incremental.h"
#include "bx/libbx.h"

/*
 * GNU tar's current listed-incremental snapshot format is deliberately
 * binary: decimal fields are NUL terminated and dumpdir records are
 * marker-prefixed names followed by a second NUL record terminator.
 */
#define BX_TAR_INCREMENTAL_SNAPSHOT_VERSION 2u
#define BX_TAR_INCREMENTAL_SNAPSHOT_HEADER "GNU tar-1.35.90-2\n"

struct bx_tar_incremental_dump_item {
    char marker;
    char* name;
};

struct bx_tar_incremental_directory {
    char* name;
    struct timespec mtime;
    dev_t device;
    ino_t inode;
    size_t traversal_index;
    bool nfs;
    bool all_children;
    const struct bx_tar_incremental_directory* previous;
    struct bx_tar_incremental_dump_item* dump;
    size_t dump_len;
    size_t dump_cap;
    struct bx_archive_buffer payload;
};

struct bx_tar_incremental_state {
    char* snapshot_path;
    struct timespec previous_start;
    struct timespec current_start;
    bool has_previous_snapshot;
    struct bx_tar_incremental_directory* previous;
    size_t previous_len;
    struct bx_tar_incremental_directory* current;
    size_t current_len;
    bool* include;
    size_t include_len;
};

struct bx_tar_incremental_ordered_entry {
    struct bx_archive_fs_entry entry;
    size_t group;
    size_t order;
    size_t sequence;
};

static char* bx_tar_incremental_dup_range(const char* text, size_t len) {
    char* copy = xmalloc(len + 1u);

    memcpy(copy, text, len);
    copy[len] = '\0';
    return copy;
}

static void bx_tar_incremental_dump_free(struct bx_tar_incremental_directory* directory) {
    size_t i;

    for (i = 0u; i < directory->dump_len; i++) {
        free(directory->dump[i].name);
    }
    free(directory->dump);
    directory->dump = NULL;
    directory->dump_len = 0u;
    directory->dump_cap = 0u;
}

static void bx_tar_incremental_directory_free(struct bx_tar_incremental_directory* directory) {
    free(directory->name);
    directory->name = NULL;
    bx_tar_incremental_dump_free(directory);
    bx_archive_buffer_free(&directory->payload);
    directory->previous = NULL;
}

static void bx_tar_incremental_directory_list_free(struct bx_tar_incremental_directory** directories, size_t* length) {
    size_t i;

    if (*directories == NULL) {
        *length = 0u;
        return;
    }
    for (i = 0u; i < *length; i++) {
        bx_tar_incremental_directory_free(&(*directories)[i]);
    }
    free(*directories);
    *directories = NULL;
    *length = 0u;
}

static void bx_tar_incremental_state_free(struct bx_tar_incremental_state* state) {
    if (state == NULL) {
        return;
    }
    free(state->snapshot_path);
    state->snapshot_path = NULL;
    bx_tar_incremental_directory_list_free(&state->previous, &state->previous_len);
    bx_tar_incremental_directory_list_free(&state->current, &state->current_len);
    free(state->include);
    state->include = NULL;
    state->include_len = 0u;
    free(state);
}

static bool bx_tar_incremental_read_field(const unsigned char* data, size_t data_len, size_t* offset, const unsigned char** field, size_t* field_len) {
    const unsigned char* terminator;

    if (*offset > data_len) {
        return false;
    }
    terminator = memchr(data + *offset, '\0', data_len - *offset);
    if (terminator == NULL) {
        return false;
    }
    *field = data + *offset;
    *field_len = (size_t)(terminator - *field);
    *offset = (size_t)(terminator - data) + 1u;
    return true;
}

static bool bx_tar_incremental_parse_uint(const unsigned char* field, size_t field_len, uintmax_t max_value, uintmax_t* value_out) {
    uintmax_t value;
    size_t i;

    if (field_len == 0u) {
        return false;
    }
    value = 0u;
    for (i = 0u; i < field_len; i++) {
        unsigned int digit;

        if (field[i] < '0' || field[i] > '9') {
            return false;
        }
        digit = (unsigned int)(field[i] - '0');
        if (digit > max_value || value > (max_value - digit) / 10u) {
            return false;
        }
        value = value * 10u + digit;
    }
    *value_out = value;
    return true;
}

static bool bx_tar_incremental_parse_time(const unsigned char* field, size_t field_len, time_t* value_out) {
    char text[64];
    char* end = NULL;
    intmax_t value;
    time_t converted;
    size_t first_digit = 0u;
    size_t i;

    if (field_len == 0u) {
        return false;
    }
    if (field[0] == '-') {
        first_digit = 1u;
    }
    if (first_digit == field_len) {
        return false;
    }
    for (i = first_digit; i < field_len; i++) {
        if (field[i] < '0' || field[i] > '9') {
            return false;
        }
    }
    if (field_len >= sizeof(text)) {
        return false;
    }
    memcpy(text, field, field_len);
    text[field_len] = '\0';
    errno = 0;
    value = strtoimax(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0') {
        return false;
    }
    converted = (time_t)value;
    if ((intmax_t)converted != value) {
        return false;
    }
    *value_out = converted;
    return true;
}

static bool bx_tar_incremental_parse_version(const unsigned char* line, size_t line_len) {
    static const char prefix[] = "GNU tar-";
    const unsigned char* last_dash = NULL;
    size_t i;
    uintmax_t version;

    if (line_len <= sizeof(prefix) - 1u || memcmp(line, prefix, sizeof(prefix) - 1u) != 0) {
        return false;
    }
    for (i = sizeof(prefix) - 1u; i < line_len; i++) {
        if (line[i] == '-') {
            last_dash = line + i;
        }
    }
    if (last_dash == NULL || (size_t)(last_dash - line) + 1u >= line_len || !bx_tar_incremental_parse_uint(last_dash + 1u, line_len - (size_t)(last_dash - line) - 1u, UINTMAX_MAX, &version)) {
        return false;
    }
    return version == BX_TAR_INCREMENTAL_SNAPSHOT_VERSION;
}

static bool bx_tar_incremental_is_nfs(const struct stat* stat_data) {
    return (((uintmax_t)stat_data->st_dev >> (sizeof(stat_data->st_dev) * CHAR_BIT - 1u)) & 1u) != 0u;
}

static int bx_tar_incremental_dump_compare_qsort(const void* left, const void* right) {
    const struct bx_tar_incremental_dump_item* a = left;
    const struct bx_tar_incremental_dump_item* b = right;

    return strcmp(a->name, b->name);
}

static int bx_tar_incremental_directory_compare(const void* left, const void* right) {
    const struct bx_tar_incremental_directory* a = left;
    const struct bx_tar_incremental_directory* b = right;

    return strcmp(a->name, b->name);
}

static int bx_tar_incremental_ordered_entry_compare(const void* left, const void* right) {
    const struct bx_tar_incremental_ordered_entry* a = left;
    const struct bx_tar_incremental_ordered_entry* b = right;

    if (a->group != b->group) {
        return a->group < b->group ? -1 : 1;
    }
    if (a->order != b->order) {
        return a->order < b->order ? -1 : 1;
    }
    if (a->sequence != b->sequence) {
        return a->sequence < b->sequence ? -1 : 1;
    }
    return 0;
}

static bool bx_tar_incremental_dump_append(struct bx_tar_incremental_directory* directory, char marker, const char* name) {
    size_t i;

    for (i = 0u; i < directory->dump_len; i++) {
        if (strcmp(directory->dump[i].name, name) == 0) {
            if (directory->dump[i].marker != 'D' && marker == 'Y') {
                directory->dump[i].marker = marker;
            }
            return true;
        }
    }
    if (directory->dump_len == directory->dump_cap) {
        size_t next_cap = directory->dump_cap ? directory->dump_cap * 2u : 16u;
        directory->dump = xrealloc(directory->dump, next_cap * sizeof(*directory->dump));
        directory->dump_cap = next_cap;
    }
    directory->dump[directory->dump_len].marker = marker;
    directory->dump[directory->dump_len].name = xstrdup(name);
    directory->dump_len++;
    return true;
}

static const struct bx_tar_incremental_dump_item* bx_tar_incremental_dump_find(const struct bx_tar_incremental_directory* directory, const char* name) {
    size_t low = 0u;
    size_t high = directory->dump_len;

    while (low < high) {
        size_t middle = low + (high - low) / 2u;
        int comparison = strcmp(directory->dump[middle].name, name);

        if (comparison == 0) {
            return &directory->dump[middle];
        }
        if (comparison < 0) {
            low = middle + 1u;
        }
        else {
            high = middle;
        }
    }
    return NULL;
}

static char* bx_tar_incremental_trim_slashes_dup(const char* path) {
    size_t len = strlen(path);
    char* copy;

    while (len > 1u && path[len - 1u] == '/') {
        len--;
    }
    copy = xmalloc(len + 1u);
    memcpy(copy, path, len);
    copy[len] = '\0';
    return copy;
}

static bool bx_tar_incremental_split_parent(const char* path, char** parent_out, char** basename_out) {
    char* normalized = bx_tar_incremental_trim_slashes_dup(path);
    char* slash = strrchr(normalized, '/');

    if (slash == NULL || slash[1] == '\0') {
        free(normalized);
        return false;
    }
    *basename_out = xstrdup(slash + 1u);
    if (slash == normalized) {
        slash[1] = '\0';
    }
    else {
        *slash = '\0';
    }
    *parent_out = normalized;
    return true;
}

static struct bx_tar_incremental_directory* bx_tar_incremental_directory_find(struct bx_tar_incremental_directory* directories, size_t length, const char* name) {
    size_t low = 0u;
    size_t high = length;

    while (low < high) {
        size_t middle = low + (high - low) / 2u;
        int comparison = strcmp(directories[middle].name, name);

        if (comparison == 0) {
            return &directories[middle];
        }
        if (comparison < 0) {
            low = middle + 1u;
        }
        else {
            high = middle;
        }
    }
    return NULL;
}

static bool bx_tar_incremental_read_snapshot(struct bx_tar_incremental_state* state, struct bx_diag_ctx* diag) {
    struct bx_archive_buffer buffer = {0};
    FILE* stream;
    const unsigned char* line_end;
    size_t offset;
    const unsigned char* field;
    size_t field_len;

    stream = fopen(state->snapshot_path, "rb");
    if (stream == NULL) {
        if (errno == ENOENT) {
            return true;
        }
        bx_diag(diag, "%s: %s", state->snapshot_path, strerror(errno));
        return false;
    }
    if (!bx_archive_buffer_read_all(stream, &buffer, diag)) {
        fclose(stream);
        bx_archive_buffer_free(&buffer);
        return false;
    }
    if (fclose(stream) != 0) {
        bx_diag(diag, "%s: %s", state->snapshot_path, strerror(errno));
        bx_archive_buffer_free(&buffer);
        return false;
    }
    if (buffer.len == 0u) {
        bx_archive_buffer_free(&buffer);
        return true;
    }

    line_end = memchr(buffer.data, '\n', buffer.len);
    if (line_end == NULL || !bx_tar_incremental_parse_version(buffer.data, (size_t)(line_end - buffer.data))) {
        bx_diag(diag, "%s: invalid or unsupported incremental snapshot format", state->snapshot_path);
        bx_archive_buffer_free(&buffer);
        return false;
    }
    offset = (size_t)(line_end - buffer.data) + 1u;
    if (!bx_tar_incremental_read_field(buffer.data, buffer.len, &offset, &field, &field_len) || !bx_tar_incremental_parse_time(field, field_len, &state->previous_start.tv_sec) ||
        !bx_tar_incremental_read_field(buffer.data, buffer.len, &offset, &field, &field_len)) {
        bx_diag(diag, "%s: invalid incremental snapshot timestamp", state->snapshot_path);
        bx_archive_buffer_free(&buffer);
        return false;
    }
    {
        uintmax_t nsec;
        if (!bx_tar_incremental_parse_uint(field, field_len, 999999999u, &nsec)) {
            bx_diag(diag, "%s: invalid incremental snapshot timestamp", state->snapshot_path);
            bx_archive_buffer_free(&buffer);
            return false;
        }
        state->previous_start.tv_nsec = (long)nsec;
    }
    state->has_previous_snapshot = true;

    while (offset < buffer.len) {
        struct bx_tar_incremental_directory directory = {0};
        uintmax_t value;
        time_t seconds;

        if (!bx_tar_incremental_read_field(buffer.data, buffer.len, &offset, &field, &field_len) || !bx_tar_incremental_parse_uint(field, field_len, 1u, &value)) {
            bx_diag(diag, "%s: invalid incremental snapshot directory record", state->snapshot_path);
            bx_archive_buffer_free(&buffer);
            bx_tar_incremental_directory_free(&directory);
            return false;
        }
        directory.nfs = value != 0u;
        if (!bx_tar_incremental_read_field(buffer.data, buffer.len, &offset, &field, &field_len) || !bx_tar_incremental_parse_time(field, field_len, &seconds)) {
            bx_diag(diag, "%s: invalid incremental snapshot directory timestamp", state->snapshot_path);
            bx_archive_buffer_free(&buffer);
            bx_tar_incremental_directory_free(&directory);
            return false;
        }
        directory.mtime.tv_sec = seconds;
        if (!bx_tar_incremental_read_field(buffer.data, buffer.len, &offset, &field, &field_len) || !bx_tar_incremental_parse_uint(field, field_len, 999999999u, &value)) {
            bx_diag(diag, "%s: invalid incremental snapshot directory timestamp", state->snapshot_path);
            bx_archive_buffer_free(&buffer);
            bx_tar_incremental_directory_free(&directory);
            return false;
        }
        directory.mtime.tv_nsec = (long)value;
        if (!bx_tar_incremental_read_field(buffer.data, buffer.len, &offset, &field, &field_len) || !bx_tar_incremental_parse_uint(field, field_len, (uintmax_t)(dev_t)-1, &value)) {
            bx_diag(diag, "%s: invalid incremental snapshot device number", state->snapshot_path);
            bx_archive_buffer_free(&buffer);
            bx_tar_incremental_directory_free(&directory);
            return false;
        }
        directory.device = (dev_t)value;
        if (!bx_tar_incremental_read_field(buffer.data, buffer.len, &offset, &field, &field_len) || !bx_tar_incremental_parse_uint(field, field_len, (uintmax_t)(ino_t)-1, &value)) {
            bx_diag(diag, "%s: invalid incremental snapshot inode number", state->snapshot_path);
            bx_archive_buffer_free(&buffer);
            bx_tar_incremental_directory_free(&directory);
            return false;
        }
        directory.inode = (ino_t)value;
        if (!bx_tar_incremental_read_field(buffer.data, buffer.len, &offset, &field, &field_len) || field_len == 0u) {
            bx_diag(diag, "%s: invalid incremental snapshot directory name", state->snapshot_path);
            bx_archive_buffer_free(&buffer);
            bx_tar_incremental_directory_free(&directory);
            return false;
        }
        directory.name = bx_tar_incremental_dup_range((const char*)field, field_len);
        bx_archive_buffer_init(&directory.payload);

        while (true) {
            if (!bx_tar_incremental_read_field(buffer.data, buffer.len, &offset, &field, &field_len)) {
                bx_diag(diag, "%s: unterminated incremental snapshot dumpdir", state->snapshot_path);
                bx_archive_buffer_free(&buffer);
                bx_tar_incremental_directory_free(&directory);
                return false;
            }
            if (field_len == 0u) {
                if (!bx_tar_incremental_read_field(buffer.data, buffer.len, &offset, &field, &field_len) || field_len != 0u) {
                    bx_diag(diag, "%s: invalid incremental snapshot dumpdir terminator", state->snapshot_path);
                    bx_archive_buffer_free(&buffer);
                    bx_tar_incremental_directory_free(&directory);
                    return false;
                }
                break;
            }
            if (field_len < 2u || (field[0] != 'Y' && field[0] != 'N' && field[0] != 'D')) {
                bx_diag(diag, "%s: invalid incremental snapshot dumpdir record", state->snapshot_path);
                bx_archive_buffer_free(&buffer);
                bx_tar_incremental_directory_free(&directory);
                return false;
            }
            if (!bx_tar_incremental_dump_append(&directory, (char)field[0], (const char*)field + 1u)) {
                bx_archive_buffer_free(&buffer);
                bx_tar_incremental_directory_free(&directory);
                return false;
            }
        }
        if (state->previous_len == SIZE_MAX) {
            bx_archive_buffer_free(&buffer);
            bx_tar_incremental_directory_free(&directory);
            return false;
        }
        state->previous = xrealloc(state->previous, (state->previous_len + 1u) * sizeof(*state->previous));
        state->previous[state->previous_len++] = directory;
    }

    if (state->previous_len > 1u) {
        qsort(state->previous, state->previous_len, sizeof(*state->previous), bx_tar_incremental_directory_compare);
    }
    for (offset = 0u; offset < state->previous_len; offset++) {
        if (state->previous[offset].dump_len > 1u) {
            qsort(state->previous[offset].dump, state->previous[offset].dump_len, sizeof(*state->previous[offset].dump), bx_tar_incremental_dump_compare_qsort);
        }
        if (offset > 0u && strcmp(state->previous[offset - 1u].name, state->previous[offset].name) == 0) {
            bx_diag(diag, "%s: duplicate incremental snapshot directory", state->snapshot_path);
            bx_archive_buffer_free(&buffer);
            return false;
        }
    }
    bx_archive_buffer_free(&buffer);
    return true;
}

static int bx_tar_incremental_timespec_compare(struct timespec left, struct timespec right) {
    if (left.tv_sec < right.tv_sec) {
        return -1;
    }
    if (left.tv_sec > right.tv_sec) {
        return 1;
    }
    if (left.tv_nsec < right.tv_nsec) {
        return -1;
    }
    if (left.tv_nsec > right.tv_nsec) {
        return 1;
    }
    return 0;
}

static bool bx_tar_incremental_append_current_directory(struct bx_tar_incremental_state* state, const struct bx_archive_fs_entry* entry) {
    struct bx_tar_incremental_directory* directory;

    state->current = xrealloc(state->current, (state->current_len + 1u) * sizeof(*state->current));
    directory = &state->current[state->current_len++];
    memset(directory, 0, sizeof(*directory));
    directory->name = bx_tar_incremental_trim_slashes_dup(entry->archive_path);
    directory->mtime = entry->st.st_mtim;
    directory->device = entry->st.st_dev;
    directory->inode = entry->st.st_ino;
    directory->traversal_index = state->current_len - 1u;
    directory->nfs = bx_tar_incremental_is_nfs(&entry->st);
    bx_archive_buffer_init(&directory->payload);
    return true;
}

static bool bx_tar_incremental_build_payloads(struct bx_tar_incremental_state* state, struct bx_diag_ctx* diag) {
    size_t i;

    for (i = 0u; i < state->current_len; i++) {
        struct bx_tar_incremental_directory* directory = &state->current[i];
        size_t j;

        if (directory->dump_len > 1u) {
            qsort(directory->dump, directory->dump_len, sizeof(*directory->dump), bx_tar_incremental_dump_compare_qsort);
        }
        for (j = 0u; j < directory->dump_len; j++) {
            if (!bx_archive_buffer_append_byte(&directory->payload, (unsigned char)directory->dump[j].marker) ||
                !bx_archive_buffer_append(&directory->payload, directory->dump[j].name, strlen(directory->dump[j].name) + 1u)) {
                bx_diag(diag, "incremental snapshot dumpdir is too large");
                return false;
            }
        }
        if (!bx_archive_buffer_append_byte(&directory->payload, '\0')) {
            bx_diag(diag, "incremental snapshot dumpdir is too large");
            return false;
        }
    }
    return true;
}

static bool bx_tar_incremental_file_should_be_written(const struct bx_tar_incremental_state* state,
                                                      const struct bx_tar_incremental_directory* parent,
                                                      const char* basename,
                                                      const struct stat* stat_data) {
    const struct bx_tar_incremental_dump_item* previous_item;

    if (parent->all_children || parent->previous == NULL) {
        return true;
    }
    previous_item = bx_tar_incremental_dump_find(parent->previous, basename);
    if (previous_item == NULL) {
        return true;
    }
    return bx_tar_incremental_timespec_compare(stat_data->st_mtim, state->previous_start) >= 0 || bx_tar_incremental_timespec_compare(stat_data->st_ctim, state->previous_start) >= 0;
}

bool bx_tar_incremental_plan_init(struct bx_tar_incremental_plan* plan, const char* snapshot_path, struct bx_diag_ctx* diag) {
    struct bx_tar_incremental_state* state;

    if (plan == NULL) {
        bx_diag(diag, "invalid incremental snapshot plan");
        return false;
    }
    memset(plan, 0, sizeof(*plan));
    if (snapshot_path == NULL || snapshot_path[0] == '\0') {
        bx_diag(diag, "invalid incremental snapshot path");
        return false;
    }
    state = xmalloc(sizeof(*state));
    memset(state, 0, sizeof(*state));
    state->snapshot_path = xstrdup(snapshot_path);
    if (clock_gettime(CLOCK_REALTIME, &state->current_start) != 0) {
        bx_diag(diag, "could not get current time for incremental snapshot: %s", strerror(errno));
        bx_tar_incremental_state_free(state);
        return false;
    }
    plan->state = state;
    if (!bx_tar_incremental_read_snapshot(state, diag)) {
        bx_tar_incremental_plan_cleanup(plan);
        return false;
    }
    return true;
}

bool bx_tar_incremental_plan_prepare(struct bx_tar_incremental_plan* plan, const struct bx_archive_fs_list* files, struct bx_diag_ctx* diag) {
    struct bx_tar_incremental_state* state;
    size_t i;

    if (plan == NULL || plan->state == NULL || files == NULL) {
        bx_diag(diag, "invalid incremental snapshot plan");
        return false;
    }
    state = plan->state;
    bx_tar_incremental_directory_list_free(&state->current, &state->current_len);
    free(state->include);
    state->include = xmalloc((files->len ? files->len : 1u) * sizeof(*state->include));
    memset(state->include, 0, (files->len ? files->len : 1u) * sizeof(*state->include));
    state->include_len = files->len;

    for (i = 0u; i < files->len; i++) {
        if (S_ISDIR(files->entries[i].st.st_mode) && !bx_tar_incremental_append_current_directory(state, &files->entries[i])) {
            return false;
        }
    }
    if (state->current_len > 1u) {
        qsort(state->current, state->current_len, sizeof(*state->current), bx_tar_incremental_directory_compare);
    }
    for (i = 0u; i < state->current_len; i++) {
        if (i > 0u && strcmp(state->current[i - 1u].name, state->current[i].name) == 0) {
            bx_diag(diag, "duplicate directory name in incremental create: %s", state->current[i].name);
            return false;
        }
        state->current[i].previous = bx_tar_incremental_directory_find(state->previous, state->previous_len, state->current[i].name);
        state->current[i].all_children = state->current[i].previous == NULL;
        if (state->current[i].previous != NULL) {
            state->current[i].all_children = state->current[i].inode != state->current[i].previous->inode ||
                                             ((!state->current[i].nfs || !state->current[i].previous->nfs) && state->current[i].device != state->current[i].previous->device);
        }
    }

    for (i = 0u; i < files->len; i++) {
        char* parent_name = NULL;
        char* basename = NULL;
        char* normalized = bx_tar_incremental_trim_slashes_dup(files->entries[i].archive_path);
        struct bx_tar_incremental_directory* parent;
        bool is_directory = S_ISDIR(files->entries[i].st.st_mode);
        char marker;

        if (!bx_tar_incremental_split_parent(normalized, &parent_name, &basename)) {
            state->include[i] = is_directory || !state->has_previous_snapshot || bx_tar_incremental_timespec_compare(files->entries[i].st.st_mtim, state->previous_start) >= 0 ||
                                bx_tar_incremental_timespec_compare(files->entries[i].st.st_ctim, state->previous_start) >= 0;
            free(normalized);
            continue;
        }
        parent = bx_tar_incremental_directory_find(state->current, state->current_len, parent_name);
        if (parent == NULL) {
            state->include[i] = true;
            free(parent_name);
            free(basename);
            free(normalized);
            continue;
        }
        marker = is_directory ? 'D' : (bx_tar_incremental_file_should_be_written(state, parent, basename, &files->entries[i].st) ? 'Y' : 'N');
        if (!bx_tar_incremental_dump_append(parent, marker, basename)) {
            free(parent_name);
            free(basename);
            free(normalized);
            return false;
        }
        state->include[i] = is_directory || marker == 'Y';
        free(parent_name);
        free(basename);
        free(normalized);
    }

    return bx_tar_incremental_build_payloads(state, diag);
}

void bx_tar_incremental_plan_filter_files(struct bx_tar_incremental_plan* plan, struct bx_archive_fs_list* files) {
    struct bx_tar_incremental_state* state;
    size_t read_index;
    size_t write_index = 0u;

    if (plan == NULL || plan->state == NULL || files == NULL) {
        return;
    }
    state = plan->state;
    for (read_index = 0u; read_index < files->len; read_index++) {
        if (read_index >= state->include_len || !state->include[read_index]) {
            free(files->entries[read_index].source_path);
            free(files->entries[read_index].archive_path);
            free(files->entries[read_index].link_target);
            continue;
        }
        if (write_index != read_index) {
            files->entries[write_index] = files->entries[read_index];
        }
        write_index++;
    }
    files->len = write_index;
    free(state->include);
    state->include = NULL;
    state->include_len = 0u;
}

void bx_tar_incremental_plan_order_files(struct bx_tar_incremental_plan* plan, struct bx_archive_fs_list* files) {
    struct bx_tar_incremental_state* state;
    struct bx_tar_incremental_ordered_entry* ordered;
    struct bx_archive_fs_entry* reordered_entries;
    size_t i;

    if (plan == NULL || plan->state == NULL || files == NULL || files->len < 2u) {
        return;
    }
    state = plan->state;
    ordered = xmalloc(files->len * sizeof(*ordered));
    for (i = 0u; i < files->len; i++) {
        char* parent_name = NULL;
        char* basename = NULL;

        ordered[i].entry = files->entries[i];
        ordered[i].group = 0u;
        ordered[i].order = i;
        ordered[i].sequence = i;
        {
            char* normalized = bx_tar_incremental_trim_slashes_dup(files->entries[i].archive_path);

            if (S_ISDIR(files->entries[i].st.st_mode)) {
                struct bx_tar_incremental_directory* directory = bx_tar_incremental_directory_find(state->current, state->current_len, normalized);

                if (directory != NULL) {
                    ordered[i].order = directory->traversal_index;
                }
            }
            else if (bx_tar_incremental_split_parent(normalized, &parent_name, &basename)) {
                struct bx_tar_incremental_directory* parent = bx_tar_incremental_directory_find(state->current, state->current_len, parent_name);

                if (parent != NULL) {
                    ordered[i].group = 1u;
                    ordered[i].order = parent->traversal_index;
                }
            }
            free(normalized);
        }
        free(parent_name);
        free(basename);
    }
    qsort(ordered, files->len, sizeof(*ordered), bx_tar_incremental_ordered_entry_compare);
    reordered_entries = xmalloc(files->len * sizeof(*reordered_entries));
    for (i = 0u; i < files->len; i++) {
        reordered_entries[i] = ordered[i].entry;
    }
    free(files->entries);
    files->entries = reordered_entries;
    free(ordered);
    files->cap = files->len;
}

bool bx_tar_incremental_directory_data(const char* archive_path, const unsigned char** data_out, size_t* data_len_out, void* user_data, struct bx_diag_ctx* diag) {
    struct bx_tar_incremental_plan* plan = user_data;
    struct bx_tar_incremental_state* state;
    char* normalized;
    struct bx_tar_incremental_directory* directory;

    if (plan == NULL || plan->state == NULL || data_out == NULL || data_len_out == NULL) {
        bx_diag(diag, "invalid incremental directory data request");
        return false;
    }
    state = plan->state;
    normalized = bx_tar_incremental_trim_slashes_dup(archive_path);
    directory = bx_tar_incremental_directory_find(state->current, state->current_len, normalized);
    free(normalized);
    if (directory == NULL) {
        bx_diag(diag, "incremental directory is missing from the create plan: %s", archive_path);
        return false;
    }
    *data_out = directory->payload.data;
    *data_len_out = directory->payload.len;
    return true;
}

static bool bx_tar_incremental_write_bytes(FILE* stream, const void* data, size_t len, const char* path, struct bx_diag_ctx* diag) {
    if (len != 0u && fwrite(data, 1u, len, stream) != len) {
        bx_diag(diag, "%s: %s", path, strerror(errno));
        return false;
    }
    return true;
}

static bool bx_tar_incremental_write_byte(FILE* stream, unsigned char value, const char* path, struct bx_diag_ctx* diag) {
    return bx_tar_incremental_write_bytes(stream, &value, 1u, path, diag);
}

static bool bx_tar_incremental_write_uint(FILE* stream, uintmax_t value, const char* path, struct bx_diag_ctx* diag) {
    char text[64];
    int length = snprintf(text, sizeof(text), "%" PRIuMAX, value);

    if (length < 0 || (size_t)length >= sizeof(text) || !bx_tar_incremental_write_bytes(stream, text, (size_t)length, path, diag)) {
        return false;
    }
    return bx_tar_incremental_write_byte(stream, '\0', path, diag);
}

static bool bx_tar_incremental_write_time(FILE* stream, struct timespec value, const char* path, struct bx_diag_ctx* diag) {
    char text[64];
    int length = snprintf(text, sizeof(text), "%" PRIdMAX, (intmax_t)value.tv_sec);

    if (length < 0 || (size_t)length >= sizeof(text) || !bx_tar_incremental_write_bytes(stream, text, (size_t)length, path, diag) || !bx_tar_incremental_write_byte(stream, '\0', path, diag) ||
        !bx_tar_incremental_write_uint(stream, (uintmax_t)value.tv_nsec, path, diag)) {
        return false;
    }
    return true;
}

bool bx_tar_incremental_plan_publish(const struct bx_tar_incremental_plan* plan, struct bx_diag_ctx* diag) {
    const struct bx_tar_incremental_state* state;
    struct bx_archive_output_file output = {0};
    size_t i;
    bool ok = true;

    if (plan == NULL || plan->state == NULL) {
        bx_diag(diag, "invalid incremental snapshot plan");
        return false;
    }
    state = plan->state;
    if (!bx_archive_output_file_open(&output, state->snapshot_path, diag)) {
        return false;
    }
    ok = bx_tar_incremental_write_bytes(output.stream, BX_TAR_INCREMENTAL_SNAPSHOT_HEADER, sizeof(BX_TAR_INCREMENTAL_SNAPSHOT_HEADER) - 1u, state->snapshot_path, diag) &&
         bx_tar_incremental_write_time(output.stream, state->current_start, state->snapshot_path, diag);
    for (i = 0u; ok && i < state->current_len; i++) {
        const struct bx_tar_incremental_directory* directory = &state->current[i];
        size_t j;

        ok = bx_tar_incremental_write_uint(output.stream, directory->nfs ? 1u : 0u, state->snapshot_path, diag) &&
             bx_tar_incremental_write_time(output.stream, directory->mtime, state->snapshot_path, diag) &&
             bx_tar_incremental_write_uint(output.stream, (uintmax_t)directory->device, state->snapshot_path, diag) &&
             bx_tar_incremental_write_uint(output.stream, (uintmax_t)directory->inode, state->snapshot_path, diag) &&
             bx_tar_incremental_write_bytes(output.stream, directory->name, strlen(directory->name) + 1u, state->snapshot_path, diag);
        for (j = 0u; ok && j < directory->dump_len; j++) {
            ok = bx_tar_incremental_write_byte(output.stream, (unsigned char)directory->dump[j].marker, state->snapshot_path, diag) &&
                 bx_tar_incremental_write_bytes(output.stream, directory->dump[j].name, strlen(directory->dump[j].name) + 1u, state->snapshot_path, diag);
        }
        if (ok) {
            ok = bx_tar_incremental_write_byte(output.stream, '\0', state->snapshot_path, diag) && bx_tar_incremental_write_byte(output.stream, '\0', state->snapshot_path, diag);
        }
    }
    if (ok && !bx_archive_output_file_finish(&output, diag)) {
        ok = false;
    }
    if (!ok) {
        bx_archive_output_file_discard(&output);
    }
    return ok;
}

void bx_tar_incremental_plan_cleanup(struct bx_tar_incremental_plan* plan) {
    if (plan == NULL) {
        return;
    }
    bx_tar_incremental_state_free(plan->state);
    plan->state = NULL;
}
