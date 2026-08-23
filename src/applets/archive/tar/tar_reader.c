#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "applets/archive/archive_codec.h"
#include "applets/archive/tar/tar_reader.h"
#include "bx/libbx.h"

#define BX_TAR_READER_FILE_CHUNK_SIZE (256u * 1024u)

struct bx_tar_pax_info {
    char* path;
    char* linkpath;
    char* uname;
    char* gname;
    int sparse_major;
    int sparse_minor;
    size_t sparse_realsize;
    bool sparse_enabled;
    bool active;
};

struct bx_tar_stream_input {
    struct bx_archive_codec_input* codec_input;
};

void bx_tar_entry_free(struct bx_tar_entry* entry) {
    if (entry->name != NULL) {
        free(entry->name);
    }
    if (entry->linkname != NULL) {
        free(entry->linkname);
    }
    if (entry->uname != NULL) {
        free(entry->uname);
    }
    if (entry->gname != NULL) {
        free(entry->gname);
    }
    if (entry->data != NULL) {
        free(entry->data);
    }
    if (entry->extents != NULL) {
        free(entry->extents);
    }
    entry->name = NULL;
    entry->linkname = NULL;
    entry->uname = NULL;
    entry->gname = NULL;
    entry->data = NULL;
    entry->extents = NULL;
}

void bx_tar_entry_list_free(struct bx_tar_entry_list* list) {
    size_t i;

    for (i = 0u; i < list->len; i++) {
        bx_tar_entry_free(&list->items[i]);
    }
    free(list->items);
    list->items = NULL;
    list->len = 0u;
    list->cap = 0u;
}

bool bx_tar_entry_list_push(struct bx_tar_entry_list* list, const struct bx_tar_entry* entry) {
    struct bx_tar_entry* slot;

    if (list->len == list->cap) {
        size_t next_cap = list->cap ? list->cap * 2u : 16u;
        list->items = xrealloc(list->items, next_cap * sizeof(*list->items));
        list->cap = next_cap;
    }
    slot = &list->items[list->len++];
    memset(slot, 0, sizeof(*slot));
    *slot = *entry;
    return true;
}

static void bx_tar_pax_info_clear(struct bx_tar_pax_info* pax) {
    if (!pax->active) {
        return;
    }
    if (pax->path != NULL) {
        free(pax->path);
    }
    if (pax->linkpath != NULL) {
        free(pax->linkpath);
    }
    if (pax->uname != NULL) {
        free(pax->uname);
    }
    if (pax->gname != NULL) {
        free(pax->gname);
    }
    memset(pax, 0, sizeof(*pax));
}

static size_t bx_tar_round_up(size_t value, size_t align) {
    size_t rem = value % align;

    if (rem == 0u) {
        return value;
    }
    return value + (align - rem);
}

static bool bx_tar_block_is_zero(const unsigned char* block) {
    size_t i;

    for (i = 0u; i < BX_TAR_BLOCK_SIZE; i++) {
        if (block[i] != 0u) {
            return false;
        }
    }
    return true;
}

static bool bx_tar_stream_visitor_finish_archive(const struct bx_tar_stream_visitor_ops* visitor_ops,
                                                 uint64_t block_index,
                                                 enum bx_tar_stream_end_kind end_kind,
                                                 uint64_t total_bytes_read,
                                                 struct bx_diag_ctx* diag) {
    if (visitor_ops->finish_archive == NULL) {
        return true;
    }
    return visitor_ops->finish_archive(visitor_ops->user,
                                       block_index,
                                       end_kind,
                                       total_bytes_read,
                                       diag);
}

static uint64_t bx_tar_stream_input_total_bytes_read(const struct bx_tar_stream_input* input) {
    return bx_archive_codec_input_total_bytes_read(input->codec_input);
}

static bool bx_tar_parse_octal_field_slow(const unsigned char* field, size_t len, size_t* value_out) {
    const unsigned char* cursor = field;
    const unsigned char* end = field + len;
    size_t value = 0u;

    while (cursor < end && (*cursor == ' ' || *cursor == '\0')) {
        cursor++;
    }
    while (cursor < end && *cursor == '0') {
        cursor++;
    }
    if (cursor == end || *cursor == '\0' || *cursor == ' ') {
        *value_out = 0u;
        return true;
    }
    while (cursor < end) {
        unsigned char ch = *cursor++;

        if (ch == '\0' || ch == ' ') {
            break;
        }
        if ((unsigned)(ch - '0') > 7u) {
            return false;
        }
        value = (value << 3) + (size_t)(ch - '0');
    }
    *value_out = value;
    return true;
}

static bool bx_tar_parse_octal_field(const unsigned char* field, size_t len, size_t* value_out) {
    if (len > 0u && (field[len - 1u] == '\0' || field[len - 1u] == ' ')) {
        size_t value = 0u;
        size_t i = 0u;
        size_t digit_end = len - 1u;

        while (i < digit_end && field[i] == '0') {
            i++;
        }
        if (i == digit_end) {
            *value_out = 0u;
            return true;
        }
        while (i < digit_end) {
            unsigned char ch = field[i++];

            if ((unsigned)(ch - '0') > 7u) {
                return bx_tar_parse_octal_field_slow(field, len, value_out);
            }
            value = (value << 3) + (size_t)(ch - '0');
        }
        *value_out = value;
        return true;
    }

    return bx_tar_parse_octal_field_slow(field, len, value_out);
}

static bool bx_tar_header_checksum_valid(const unsigned char* header) {
    size_t recorded = 0u;
    size_t unsigned_sum = 0u;
    int64_t signed_sum = 0;
    size_t i;

    if (!bx_tar_parse_octal_field(header + 148u, 8u, &recorded)) {
        return false;
    }
    for (i = 0u; i < BX_TAR_BLOCK_SIZE; i++) {
        unsigned char byte = (i >= 148u && i < 156u) ? (unsigned char)' ' : header[i];
        unsigned_sum += (size_t)byte;
        signed_sum += (int8_t)byte;
    }
    return recorded == unsigned_sum || (signed_sum >= 0 && recorded == (size_t)signed_sum);
}

static bool bx_tar_parse_sparse_map(const unsigned char* payload,
                                    size_t payload_size,
                                    struct bx_tar_entry* entry,
                                    struct bx_diag_ctx* diag) {
    size_t values[128];
    size_t value_count = 0u;
    size_t i = 0u;
    size_t data_start;
    size_t extent_count;
    size_t j;
    (void)diag;

    while (i < payload_size && value_count < sizeof(values) / sizeof(values[0])) {
        size_t start = i;
        size_t value = 0u;
        bool have_digit = false;

        while (i < payload_size && payload[i] != '\n') {
            if (payload[i] >= '0' && payload[i] <= '9') {
                have_digit = true;
                value = value * 10u + (size_t)(payload[i] - '0');
            }
            i++;
        }
        if (!have_digit) {
            break;
        }
        values[value_count++] = value;
        if (i < payload_size && payload[i] == '\n') {
            i++;
        }
        if (start == i) {
            break;
        }
        if (value_count >= 1u && value_count == (1u + values[0] * 2u)) {
            break;
        }
    }

    if (value_count < 1u) {
        return false;
    }
    extent_count = values[0];
    if (value_count < 1u + extent_count * 2u) {
        return false;
    }

    entry->extents = xrealloc(entry->extents, extent_count * sizeof(*entry->extents));
    entry->extent_count = 0u;
    for (j = 0u; j < extent_count; j++) {
        size_t offset = values[1u + j * 2u];
        size_t size = values[2u + j * 2u];

        entry->extents[entry->extent_count].offset = offset;
        entry->extents[entry->extent_count].size = size;
        entry->extent_count++;
    }

    data_start = bx_tar_round_up(i, BX_TAR_BLOCK_SIZE);
    if (data_start > payload_size) {
        data_start = payload_size;
    }
    entry->data_len = payload_size - data_start;
    entry->data = xmalloc(entry->data_len ? entry->data_len : 1u);
    memcpy(entry->data, payload + data_start, entry->data_len);
    return true;
}

static bool bx_tar_parse_pax_int_value(const char* value, int min_value, int max_value, int* value_out) {
    if (value == NULL || value[0] == '\0' || value_out == NULL || min_value > max_value) {
        return false;
    }

    errno = 0;
    char* end = NULL;
    intmax_t parsed = strtoimax(value, &end, 10);
    if (errno == ERANGE || end == value || end == NULL || *end != '\0'
        || parsed < (intmax_t)min_value || parsed > (intmax_t)max_value) {
        return false;
    }

    *value_out = (int)parsed;
    return true;
}

static bool bx_tar_parse_pax_size_value(const char* value, size_t* value_out) {
    if (value == NULL || value[0] == '\0' || value[0] == '-' || value_out == NULL) {
        return false;
    }

    errno = 0;
    char* end = NULL;
    uintmax_t parsed = strtoumax(value, &end, 10);
    if (errno == ERANGE || end == value || end == NULL || *end != '\0' || parsed > SIZE_MAX) {
        return false;
    }

    *value_out = (size_t)parsed;
    return true;
}

static bool bx_tar_apply_pax_record(struct bx_tar_pax_info* pax,
                                    char* record,
                                    bool skip_owner_group_names) {
    const char* key;
    const char* value;
    char* equal = strchr(record, '=');

    if (equal == NULL) {
        return false;
    }
    *equal = '\0';
    key = record;
    value = equal + 1;
    if (strcmp(key, "path") == 0) {
        free(pax->path);
        pax->path = xstrdup(value);
        pax->active = true;
    }
    else if (strcmp(key, "linkpath") == 0) {
        free(pax->linkpath);
        pax->linkpath = xstrdup(value);
        pax->active = true;
    }
    else if (!skip_owner_group_names && strcmp(key, "uname") == 0) {
        free(pax->uname);
        pax->uname = xstrdup(value);
        pax->active = true;
    }
    else if (!skip_owner_group_names && strcmp(key, "gname") == 0) {
        free(pax->gname);
        pax->gname = xstrdup(value);
        pax->active = true;
    }
    else if (strcmp(key, "GNU.sparse.name") == 0) {
        free(pax->path);
        pax->path = xstrdup(value);
        pax->sparse_enabled = true;
        pax->active = true;
    }
    else if (strcmp(key, "GNU.sparse.major") == 0) {
        int parsed = 0;
        if (!bx_tar_parse_pax_int_value(value, 0, INT_MAX, &parsed)) {
            return false;
        }
        pax->sparse_major = parsed;
        pax->sparse_enabled = true;
        pax->active = true;
    }
    else if (strcmp(key, "GNU.sparse.minor") == 0) {
        int parsed = 0;
        if (!bx_tar_parse_pax_int_value(value, 0, INT_MAX, &parsed)) {
            return false;
        }
        pax->sparse_minor = parsed;
        pax->sparse_enabled = true;
        pax->active = true;
    }
    else if (strcmp(key, "GNU.sparse.realsize") == 0) {
        size_t parsed = 0u;
        if (!bx_tar_parse_pax_size_value(value, &parsed)) {
            return false;
        }
        pax->sparse_realsize = parsed;
        pax->sparse_enabled = true;
        pax->active = true;
    }
    return true;
}

static size_t bx_tar_header_text_len(const unsigned char* field, size_t width) {
    const unsigned char* end = memchr(field, '\0', width);
    return end != NULL ? (size_t)(end - field) : width;
}

static char* bx_tar_header_text_dup_len(const unsigned char* field, size_t len) {
    char* text = xmalloc(len + 1u);

    memcpy(text, field, len);
    text[len] = '\0';
    return text;
}

static char* bx_tar_header_text_dup(const unsigned char* field, size_t width) {
    size_t len = bx_tar_header_text_len(field, width);
    return bx_tar_header_text_dup_len(field, len);
}

static char* bx_tar_header_name_join_dup(const unsigned char* name_field,
                                         size_t name_width,
                                         const unsigned char* prefix_field,
                                         size_t prefix_width) {
    size_t name_len = bx_tar_header_text_len(name_field, name_width);
    size_t prefix_len;

    if (prefix_field[0] == '\0') {
        return bx_tar_header_text_dup_len(name_field, name_len);
    }
    prefix_len = bx_tar_header_text_len(prefix_field, prefix_width);

    {
        char* text = xmalloc(prefix_len + 1u + name_len + 1u);

        memcpy(text, prefix_field, prefix_len);
        text[prefix_len] = '/';
        memcpy(text + prefix_len + 1u, name_field, name_len);
        text[prefix_len + 1u + name_len] = '\0';
        return text;
    }
}

/*
 * Old GNU headers use the POSIX prefix field (offset 345) for atime, ctime,
 * sparse, and multivolume metadata.  In particular, incremental dumpdir
 * records have type 'D'; treating that field as a ustar prefix corrupts every
 * member name with the stored atime.
 */
static bool bx_tar_header_is_oldgnu(const unsigned char* header) {
    static const unsigned char magic[] = {'u', 's', 't', 'a', 'r', ' ', ' ', '\0'};

    return memcmp(header + 257u, magic, sizeof(magic)) == 0;
}

static bool bx_tar_parse_pax_records(struct bx_tar_pax_info* pax,
                                     const unsigned char* data,
                                     size_t len,
                                     bool skip_owner_group_names) {
    size_t pos = 0u;

    while (pos < len) {
        size_t line_len = 0u;
        size_t digits_end = pos;
        size_t field_start;
        size_t field_len;
        char* record;

        while (digits_end < len && data[digits_end] != ' ') {
            if (data[digits_end] < '0' || data[digits_end] > '9') {
                return false;
            }
            if (line_len > (SIZE_MAX - 9u) / 10u) {
                return false;
            }
            line_len = line_len * 10u + (size_t)(data[digits_end] - '0');
            digits_end++;
        }
        if (digits_end == pos || digits_end >= len || data[digits_end] != ' '
            || line_len == 0u || pos + line_len > len || data[pos + line_len - 1u] != '\n') {
            return false;
        }
        field_start = digits_end + 1u;
        if (field_start > pos + line_len - 1u) {
            return false;
        }
        field_len = (pos + line_len - 1u) - field_start;
        record = xmalloc(field_len + 1u);
        memcpy(record, data + field_start, field_len);
        record[field_len] = '\0';
        if (!bx_tar_apply_pax_record(pax, record, skip_owner_group_names)) {
            free(record);
            return false;
        }
        free(record);
        pos += line_len;
    }
    return true;
}

static bool bx_tar_prepare_entry_from_header(const unsigned char* header,
                                             size_t size,
                                             unsigned char typeflag,
                                             const struct bx_tar_pax_info* pax,
                                             char** gnu_long_name,
                                             char** gnu_long_link,
                                             bool skip_owner_group_names,
                                             bool skip_owner_group_ids,
                                             struct bx_tar_entry* entry,
                                             struct bx_diag_ctx* diag) {
    char* name = NULL;

    (void)diag;
    memset(entry, 0, sizeof(*entry));
    name = bx_tar_header_is_oldgnu(header)
        ? bx_tar_header_text_dup(header, 100u)
        : bx_tar_header_name_join_dup(header, 100u, header + 345, 155u);

    if (pax->path != NULL) {
        free(name);
        name = xstrdup(pax->path);
    }
    else if (*gnu_long_name != NULL) {
        free(name);
        name = *gnu_long_name;
        *gnu_long_name = NULL;
    }
    if (typeflag == '5' || typeflag == 'D') {
        size_t name_len = strlen(name);
        while (name_len > 0u && name[name_len - 1u] == '/') {
            name[--name_len] = '\0';
        }
    }
    entry->name = name;
    entry->mode = 0644u;
    entry->size = size;
    entry->dumpdir = typeflag == 'D';
    {
        size_t parsed_mode = 0u;
        size_t parsed_mtime = 0u;

        bx_tar_parse_octal_field(header + 100, 8u, &parsed_mode);
        bx_tar_parse_octal_field(header + 136, 12u, &parsed_mtime);
        entry->mode = (mode_t)parsed_mode;
        entry->mtime.tv_sec = (time_t)parsed_mtime;
        entry->mtime.tv_nsec = 0;
        if (!skip_owner_group_ids) {
            size_t parsed_uid = 0u;
            size_t parsed_gid = 0u;

            bx_tar_parse_octal_field(header + 108, 8u, &parsed_uid);
            bx_tar_parse_octal_field(header + 116, 8u, &parsed_gid);
            entry->uid = (uid_t)parsed_uid;
            entry->gid = (gid_t)parsed_gid;
        }
    }
    if (!skip_owner_group_names) {
        if (pax->uname != NULL) {
            entry->uname = xstrdup(pax->uname);
        }
        else if (header[265] != '\0') {
            entry->uname = bx_tar_header_text_dup(header + 265, 32u);
        }
        if (pax->gname != NULL) {
            entry->gname = xstrdup(pax->gname);
        }
        else if (header[297] != '\0') {
            entry->gname = bx_tar_header_text_dup(header + 297, 32u);
        }
    }
    if (pax->linkpath != NULL) {
        entry->linkname = xstrdup(pax->linkpath);
    }
    else if ((typeflag == '1' || typeflag == '2') && *gnu_long_link != NULL) {
        entry->linkname = *gnu_long_link;
        *gnu_long_link = NULL;
    }
    else if (typeflag == '1' || typeflag == '2') {
        entry->linkname = bx_tar_header_text_dup(header + 157, 100u);
    }

    switch (typeflag) {
        case '\0':
        case '0':
            entry->kind = BX_TAR_KIND_REG;
            break;
        case '5':
        case 'D':
            entry->kind = BX_TAR_KIND_DIR;
            break;
        case '2':
            entry->kind = BX_TAR_KIND_SYMLINK;
            break;
        case '1':
            entry->kind = BX_TAR_KIND_HARDLINK;
            break;
        case '6':
            entry->kind = BX_TAR_KIND_FIFO;
            break;
        default:
            bx_tar_entry_free(entry);
            bx_diag(diag, "unsupported tar entry type");
            return false;
    }

    return true;
}

static char* bx_tar_header_name_dup(const unsigned char* header,
                                    const struct bx_tar_pax_info* pax,
                                    char** gnu_long_name) {
    char* name = bx_tar_header_is_oldgnu(header)
        ? bx_tar_header_text_dup(header, 100u)
        : bx_tar_header_name_join_dup(header, 100u, header + 345, 155u);

    if (pax->path != NULL) {
        free(name);
        name = xstrdup(pax->path);
    }
    else if (*gnu_long_name != NULL) {
        free(name);
        name = *gnu_long_name;
        *gnu_long_name = NULL;
    }
    return name;
}

bool bx_tar_parse_archive_buffer(const struct bx_archive_buffer* archive,
                                 struct bx_tar_entry_list* entries,
                                 struct bx_diag_ctx* diag) {
    size_t pos = 0u;
    struct bx_tar_pax_info pax = {0};
    char* gnu_long_name = NULL;
    char* gnu_long_link = NULL;

    while (pos + BX_TAR_BLOCK_SIZE <= archive->len) {
        const unsigned char* header = archive->data + pos;
        size_t size = 0u;
        size_t payload_start;
        size_t payload_padded;
        unsigned char typeflag;
        struct bx_tar_entry entry;

        if (bx_tar_block_is_zero(header)) {
            if (pos + 2u * BX_TAR_BLOCK_SIZE <= archive->len
                && bx_tar_block_is_zero(archive->data + pos + BX_TAR_BLOCK_SIZE)) {
                break;
            }
            pos += BX_TAR_BLOCK_SIZE;
            continue;
        }

        if (!bx_tar_header_checksum_valid(header)) {
            bx_tar_pax_info_clear(&pax);
            free(gnu_long_name);
            free(gnu_long_link);
            bx_diag(diag, "invalid tar header");
            return false;
        }
        if (!bx_tar_parse_octal_field(header + 124, 12u, &size)) {
            bx_tar_pax_info_clear(&pax);
            free(gnu_long_name);
            free(gnu_long_link);
            bx_diag(diag, "invalid tar header");
            return false;
        }
        typeflag = header[156];
        payload_start = pos + BX_TAR_BLOCK_SIZE;
        payload_padded = bx_tar_round_up(size, BX_TAR_BLOCK_SIZE);
        if (payload_start + payload_padded > archive->len) {
            bx_tar_pax_info_clear(&pax);
            free(gnu_long_name);
            free(gnu_long_link);
            bx_diag(diag, "truncated archive");
            return false;
        }

        if (typeflag == 'x') {
            if (!bx_tar_parse_pax_records(&pax, archive->data + payload_start, size, false)) {
                bx_tar_pax_info_clear(&pax);
                free(gnu_long_name);
                free(gnu_long_link);
                bx_diag(diag, "invalid pax header");
                return false;
            }
            pos = payload_start + payload_padded;
            continue;
        }
        if (typeflag == 'g') {
            pos = payload_start + payload_padded;
            continue;
        }
        if (typeflag == 'L' || typeflag == 'K') {
            char** target = (typeflag == 'L') ? &gnu_long_name : &gnu_long_link;
            size_t text_len = size;

            while (text_len > 0u && archive->data[payload_start + text_len - 1u] == '\0') {
                text_len--;
            }
            free(*target);
            *target = xmalloc(text_len + 1u);
            memcpy(*target, archive->data + payload_start, text_len);
            (*target)[text_len] = '\0';
            pos = payload_start + payload_padded;
            continue;
        }
        if (typeflag == 'V') {
            free(bx_tar_header_name_dup(header, &pax, &gnu_long_name));
            pos = payload_start + payload_padded;
            bx_tar_pax_info_clear(&pax);
            continue;
        }

        if (!bx_tar_prepare_entry_from_header(header,
                                              size,
                                              typeflag,
                                              &pax,
                                              &gnu_long_name,
                                              &gnu_long_link,
                                              false,
                                              false,
                                              &entry,
                                              diag)) {
            bx_tar_pax_info_clear(&pax);
            free(gnu_long_name);
            free(gnu_long_link);
            return false;
        }

        if (entry.dumpdir) {
            entry.data_len = size;
            entry.data = xmalloc(size ? size : 1u);
            memcpy(entry.data, archive->data + payload_start, size);
        }
        else if (entry.kind == BX_TAR_KIND_REG) {
            if (pax.sparse_enabled && pax.sparse_major == 1 && pax.sparse_minor == 0) {
                entry.sparse = true;
                entry.size = pax.sparse_realsize;
                if (!bx_tar_parse_sparse_map(archive->data + payload_start, size, &entry, diag)) {
                    bx_tar_entry_free(&entry);
                    bx_tar_pax_info_clear(&pax);
                    free(gnu_long_name);
                    free(gnu_long_link);
                    bx_diag(diag, "invalid sparse payload");
                    return false;
                }
            }
            else {
                entry.data_len = size;
                entry.data = xmalloc(size ? size : 1u);
                memcpy(entry.data, archive->data + payload_start, size);
            }
        }

        bx_tar_entry_list_push(entries, &entry);
        pos = payload_start + payload_padded;
        bx_tar_pax_info_clear(&pax);
    }

    bx_tar_pax_info_clear(&pax);
    free(gnu_long_name);
    free(gnu_long_link);
    return true;
}

static bool bx_tar_stream_input_open(struct bx_tar_stream_input* input,
                                     const struct bx_tar_reader_stream_options* options,
                                     struct bx_diag_ctx* diag) {
    memset(input, 0, sizeof(*input));
    return bx_archive_codec_input_open(&input->codec_input,
                                       &(struct bx_archive_codec_input_options){
                                           .archive_path = options->archive_path,
                                           .required_codec = options->required_codec,
                                           .seek_mode = options->seek_mode,
                                       },
                                       diag);
}

static bool bx_tar_stream_input_read_some(struct bx_tar_stream_input* input,
                                          unsigned char* buffer,
                                          size_t len,
                                          size_t* nread_out,
                                          struct bx_diag_ctx* diag) {
    return bx_archive_codec_input_read_some(input->codec_input, buffer, len, nread_out, diag);
}

static bool bx_tar_stream_input_read_exact(struct bx_tar_stream_input* input,
                                           unsigned char* buffer,
                                           size_t len,
                                           bool* eof_out,
                                           struct bx_diag_ctx* diag) {
    size_t total = 0u;

    while (total < len) {
        size_t nread = 0u;

        if (!bx_tar_stream_input_read_some(input, buffer + total, len - total, &nread, diag)) {
            return false;
        }
        if (nread == 0u) {
            if (total == 0u) {
                *eof_out = true;
                return true;
            }
            bx_diag(diag, "truncated archive");
            return false;
        }
        total += nread;
    }

    *eof_out = false;
    return true;
}

static bool bx_tar_stream_input_finish_success(struct bx_tar_stream_input* input,
                                               struct bx_diag_ctx* diag) {
    return bx_archive_codec_input_finish_success(input->codec_input, diag);
}

static bool bx_tar_stream_input_skip(struct bx_tar_stream_input* input,
                                     size_t len,
                                     struct bx_diag_ctx* diag) {
    return bx_archive_codec_input_skip(input->codec_input, len, diag);
}

static bool bx_tar_stream_input_read_text_payload(struct bx_tar_stream_input* input,
                                                  size_t size,
                                                  char** text_io,
                                                  struct bx_diag_ctx* diag) {
    char* text = xmalloc(size + 1u);
    bool eof = false;
    size_t padding = bx_tar_round_up(size, BX_TAR_BLOCK_SIZE) - size;
    size_t text_len = size;

    if (size > 0u && !bx_tar_stream_input_read_exact(input, (unsigned char*)text, size, &eof, diag)) {
        free(text);
        return false;
    }
    if (eof) {
        free(text);
        bx_diag(diag, "truncated archive");
        return false;
    }
    if (padding > 0u && !bx_tar_stream_input_skip(input, padding, diag)) {
        free(text);
        return false;
    }

    while (text_len > 0u && text[text_len - 1u] == '\0') {
        text_len--;
    }
    text[text_len] = '\0';
    free(*text_io);
    *text_io = text;
    return true;
}

static bool bx_tar_stream_input_skip_payload(struct bx_tar_stream_input* input,
                                             size_t size,
                                             struct bx_diag_ctx* diag) {
    size_t total = bx_tar_round_up(size, BX_TAR_BLOCK_SIZE);
    return bx_tar_stream_input_skip(input, total, diag);
}

static bool bx_tar_stream_input_read_pax_records(struct bx_tar_stream_input* input,
                                                 size_t size,
                                                 struct bx_tar_pax_info* pax,
                                                 bool skip_owner_group_names,
                                                 struct bx_diag_ctx* diag) {
    size_t remaining = size;
    size_t padding = bx_tar_round_up(size, BX_TAR_BLOCK_SIZE) - size;

    while (remaining > 0u) {
        size_t line_len = 0u;
        size_t consumed = 0u;
        bool have_digits = false;
        char* record = NULL;

        while (true) {
            unsigned char ch = '\0';
            bool eof = false;

            if (!bx_tar_stream_input_read_exact(input, &ch, 1u, &eof, diag)) {
                return false;
            }
            if (eof) {
                bx_diag(diag, "truncated archive");
                return false;
            }
            consumed++;
            if (ch == ' ') {
                break;
            }
            if (ch < '0' || ch > '9') {
                return false;
            }
            if (line_len > (SIZE_MAX - 9u) / 10u) {
                return false;
            }
            line_len = line_len * 10u + (size_t)(ch - '0');
            have_digits = true;
        }

        if (!have_digits || line_len == 0u || line_len > remaining || consumed >= line_len) {
            return false;
        }

        record = xmalloc(line_len - consumed + 1u);
        {
            bool eof = false;
            size_t record_len = line_len - consumed;

            if (!bx_tar_stream_input_read_exact(input, (unsigned char*)record, record_len, &eof, diag)) {
                free(record);
                return false;
            }
            if (eof) {
                free(record);
                bx_diag(diag, "truncated archive");
                return false;
            }
            if (record_len == 0u || record[record_len - 1u] != '\n') {
                free(record);
                return false;
            }
            record[record_len - 1u] = '\0';
        }
        if (!bx_tar_apply_pax_record(pax, record, skip_owner_group_names)) {
            free(record);
            return false;
        }
        free(record);
        remaining -= line_len;
    }

    return padding == 0u || bx_tar_stream_input_skip(input, padding, diag);
}

static bool bx_tar_stream_input_read_sparse_number_line(struct bx_tar_stream_input* input,
                                                        size_t* bytes_read_out,
                                                        size_t* value_out,
                                                        bool* diagnosed_out,
                                                        struct bx_diag_ctx* diag) {
    size_t bytes_read = 0u;
    size_t value = 0u;
    bool have_digit = false;

    *diagnosed_out = false;
    while (true) {
        unsigned char ch = '\0';
        bool eof = false;

        if (!bx_tar_stream_input_read_exact(input, &ch, 1u, &eof, diag)) {
            *diagnosed_out = true;
            return false;
        }
        if (eof) {
            bx_diag(diag, "truncated archive");
            *diagnosed_out = true;
            return false;
        }
        bytes_read++;
        if (ch == '\n') {
            break;
        }
        if (ch >= '0' && ch <= '9') {
            if (value > (SIZE_MAX - 9u) / 10u) {
                return false;
            }
            value = value * 10u + (size_t)(ch - '0');
            have_digit = true;
        }
    }

    if (!have_digit) {
        return false;
    }
    *bytes_read_out = bytes_read;
    *value_out = value;
    return true;
}

static bool bx_tar_stream_input_prepare_sparse_payload(struct bx_tar_stream_input* input,
                                                       size_t size,
                                                       struct bx_tar_entry* entry,
                                                       size_t* archive_padding_out,
                                                       struct bx_diag_ctx* diag) {
    size_t map_bytes = 0u;
    size_t extent_count = 0u;
    size_t padded_map_bytes;
    size_t archive_padding = bx_tar_round_up(size, BX_TAR_BLOCK_SIZE) - size;
    size_t i;
    bool diagnosed = false;

    if (!bx_tar_stream_input_read_sparse_number_line(input,
                                                     &map_bytes,
                                                     &extent_count,
                                                     &diagnosed,
                                                     diag)) {
        if (!diagnosed) {
            bx_diag(diag, "invalid sparse payload");
        }
        return false;
    }
    if (map_bytes > size) {
        bx_diag(diag, "invalid sparse payload");
        return false;
    }
    if (extent_count > SIZE_MAX / sizeof(*entry->extents)) {
        bx_diag(diag, "invalid sparse payload");
        return false;
    }

    entry->extents = xrealloc(entry->extents, extent_count * sizeof(*entry->extents));
    entry->extent_count = extent_count;
    for (i = 0u; i < extent_count; i++) {
        size_t bytes_read = 0u;
        size_t offset = 0u;
        size_t chunk_size = 0u;

        if (!bx_tar_stream_input_read_sparse_number_line(input,
                                                         &bytes_read,
                                                         &offset,
                                                         &diagnosed,
                                                         diag)) {
            if (!diagnosed) {
                bx_diag(diag, "invalid sparse payload");
            }
            return false;
        }
        if (map_bytes > size - bytes_read) {
            bx_diag(diag, "invalid sparse payload");
            return false;
        }
        map_bytes += bytes_read;

        if (!bx_tar_stream_input_read_sparse_number_line(input,
                                                         &bytes_read,
                                                         &chunk_size,
                                                         &diagnosed,
                                                         diag)) {
            if (!diagnosed) {
                bx_diag(diag, "invalid sparse payload");
            }
            return false;
        }
        if (map_bytes > size - bytes_read) {
            bx_diag(diag, "invalid sparse payload");
            return false;
        }
        map_bytes += bytes_read;

        entry->extents[i].offset = offset;
        entry->extents[i].size = chunk_size;
    }

    padded_map_bytes = bx_tar_round_up(map_bytes, BX_TAR_BLOCK_SIZE);
    if (padded_map_bytes > size) {
        bx_diag(diag, "invalid sparse payload");
        return false;
    }
    if (padded_map_bytes > map_bytes
        && !bx_tar_stream_input_skip(input, padded_map_bytes - map_bytes, diag)) {
        return false;
    }

    entry->data_len = size - padded_map_bytes;
    *archive_padding_out = archive_padding;
    return true;
}

static bool bx_tar_stream_input_read_sparse_payload_buffered(struct bx_tar_stream_input* input,
                                                             struct bx_tar_entry* entry,
                                                             size_t archive_padding,
                                                             struct bx_diag_ctx* diag) {
    entry->data = xmalloc(entry->data_len ? entry->data_len : 1u);
    if (entry->data_len > 0u) {
        bool eof = false;

        if (!bx_tar_stream_input_read_exact(input, entry->data, entry->data_len, &eof, diag)) {
            return false;
        }
        if (eof) {
            bx_diag(diag, "truncated archive");
            return false;
        }
    }

    return archive_padding == 0u || bx_tar_stream_input_skip(input, archive_padding, diag);
}

static bool bx_tar_stream_input_visit_payload(struct bx_tar_stream_input* input,
                                              const struct bx_tar_entry* entry,
                                              size_t size,
                                              const struct bx_tar_stream_visitor_ops* visitor_ops,
                                              struct bx_diag_ctx* diag) {
    unsigned char* buffer = NULL;
    size_t remaining = size;
    size_t padding = bx_tar_round_up(size, BX_TAR_BLOCK_SIZE) - size;

    if (size == 0u) {
        return padding == 0u || bx_tar_stream_input_skip(input, padding, diag);
    }
    if (visitor_ops->visit_payload == NULL) {
        return bx_tar_stream_input_skip_payload(input, size, diag);
    }

    buffer = xmalloc(BX_TAR_READER_FILE_CHUNK_SIZE);
    while (remaining > 0u) {
        bool eof = false;
        size_t chunk = remaining > BX_TAR_READER_FILE_CHUNK_SIZE ? BX_TAR_READER_FILE_CHUNK_SIZE : remaining;

        if (!bx_tar_stream_input_read_exact(input, buffer, chunk, &eof, diag)) {
            free(buffer);
            return false;
        }
        if (eof) {
            free(buffer);
            bx_diag(diag, "truncated archive");
            return false;
        }
        if (visitor_ops->visit_payload != NULL
            && !visitor_ops->visit_payload(visitor_ops->user, entry, buffer, chunk, diag)) {
            free(buffer);
            return false;
        }
        remaining -= chunk;
    }

    free(buffer);
    if (padding > 0u && !bx_tar_stream_input_skip(input, padding, diag)) {
        return false;
    }
    return true;
}

static bool bx_tar_stream_input_read_payload_buffered(struct bx_tar_stream_input* input,
                                                      size_t size,
                                                      unsigned char** data_out,
                                                      size_t* data_len_out,
                                                      struct bx_diag_ctx* diag) {
    unsigned char* data = xmalloc(size ? size : 1u);
    size_t padding = bx_tar_round_up(size, BX_TAR_BLOCK_SIZE) - size;
    bool eof = false;

    if (size > 0u && !bx_tar_stream_input_read_exact(input, data, size, &eof, diag)) {
        free(data);
        return false;
    }
    if (eof) {
        free(data);
        bx_diag(diag, "truncated archive");
        return false;
    }
    if (padding > 0u && !bx_tar_stream_input_skip(input, padding, diag)) {
        free(data);
        return false;
    }
    *data_out = data;
    *data_len_out = size;
    return true;
}

static bool bx_tar_stream_input_visit_sparse_payload(struct bx_tar_stream_input* input,
                                                     const struct bx_tar_entry* entry,
                                                     size_t archive_padding,
                                                     const struct bx_tar_stream_visitor_ops* visitor_ops,
                                                     struct bx_diag_ctx* diag) {
    unsigned char* buffer = NULL;
    size_t remaining = entry->data_len;

    if (remaining == 0u) {
        return archive_padding == 0u || bx_tar_stream_input_skip(input, archive_padding, diag);
    }
    if (visitor_ops->visit_payload == NULL) {
        if (!bx_tar_stream_input_skip(input, remaining, diag)) {
            return false;
        }
        return archive_padding == 0u || bx_tar_stream_input_skip(input, archive_padding, diag);
    }

    buffer = xmalloc(BX_TAR_READER_FILE_CHUNK_SIZE);
    while (remaining > 0u) {
        bool eof = false;
        size_t chunk = remaining > BX_TAR_READER_FILE_CHUNK_SIZE ? BX_TAR_READER_FILE_CHUNK_SIZE : remaining;

        if (!bx_tar_stream_input_read_exact(input, buffer, chunk, &eof, diag)) {
            free(buffer);
            return false;
        }
        if (eof) {
            free(buffer);
            bx_diag(diag, "truncated archive");
            return false;
        }
        if (!visitor_ops->visit_payload(visitor_ops->user, entry, buffer, chunk, diag)) {
            free(buffer);
            return false;
        }
        remaining -= chunk;
    }

    free(buffer);
    if (archive_padding > 0u && !bx_tar_stream_input_skip(input, archive_padding, diag)) {
        return false;
    }
    return true;
}

static void bx_tar_stream_input_close(struct bx_tar_stream_input* input) {
    bx_archive_codec_input_close(input->codec_input);
    input->codec_input = NULL;
}

struct bx_tar_collect_stream_state {
    struct bx_tar_entry_list* entries;
    struct bx_tar_entry current;
    struct bx_archive_buffer current_payload;
    bool have_current;
    bool collecting_payload;
};

static bool bx_tar_clone_entry(struct bx_tar_entry* dst,
                               const struct bx_tar_entry* src,
                               bool copy_data,
                               struct bx_diag_ctx* diag) {
    memset(dst, 0, sizeof(*dst));
    dst->kind = src->kind;
    dst->mode = src->mode;
    dst->uid = src->uid;
    dst->gid = src->gid;
    dst->uname = src->uname ? xstrdup(src->uname) : NULL;
    dst->gname = src->gname ? xstrdup(src->gname) : NULL;
    dst->mtime = src->mtime;
    dst->data_len = src->data_len;
    dst->size = src->size;
    dst->dumpdir = src->dumpdir;
    dst->sparse = src->sparse;
    dst->extent_count = src->extent_count;

    if (src->name != NULL) {
        dst->name = xstrdup(src->name);
    }
    if (src->linkname != NULL) {
        dst->linkname = xstrdup(src->linkname);
    }
    if (src->extent_count > 0u) {
        dst->extents = xmalloc(src->extent_count * sizeof(*dst->extents));
        memcpy(dst->extents, src->extents, src->extent_count * sizeof(*dst->extents));
    }
    if (copy_data && src->data_len > 0u) {
        dst->data = xmalloc(src->data_len);
        memcpy(dst->data, src->data, src->data_len);
    }
    if (copy_data && src->data_len == 0u && src->data != NULL) {
        dst->data = xmalloc(1u);
    }
    (void)diag;
    return true;
}

static bool bx_tar_collect_stream_begin_entry(void* user,
                                              const struct bx_tar_entry* entry,
                                              struct bx_diag_ctx* diag) {
    struct bx_tar_collect_stream_state* state = user;
    bool copy_data = entry->dumpdir
        || !(entry->kind == BX_TAR_KIND_REG && !entry->sparse);

    bx_archive_buffer_free(&state->current_payload);
    bx_archive_buffer_init(&state->current_payload);
    bx_tar_entry_free(&state->current);
    state->have_current = false;
    state->collecting_payload = false;

    if (!bx_tar_clone_entry(&state->current, entry, copy_data, diag)) {
        return false;
    }
    if (entry->kind == BX_TAR_KIND_REG && !entry->sparse) {
        state->current.data = NULL;
        state->current.data_len = 0u;
        state->collecting_payload = true;
    }
    state->have_current = true;
    return true;
}

static bool bx_tar_collect_stream_visit_payload(void* user,
                                                const struct bx_tar_entry* entry,
                                                const unsigned char* data,
                                                size_t len,
                                                struct bx_diag_ctx* diag) {
    struct bx_tar_collect_stream_state* state = user;

    (void)entry;
    if (!state->collecting_payload) {
        return true;
    }
    if (!bx_archive_buffer_append(&state->current_payload, data, len)) {
        bx_diag(diag, "buffer growth failed: %s", strerror(errno));
        return false;
    }
    return true;
}

static bool bx_tar_collect_stream_end_entry(void* user,
                                            const struct bx_tar_entry* entry,
                                            struct bx_diag_ctx* diag) {
    struct bx_tar_collect_stream_state* state = user;

    (void)entry;
    if (!state->have_current) {
        return true;
    }
    if (state->collecting_payload) {
        state->current.data = state->current_payload.data;
        state->current.data_len = state->current_payload.len;
        state->current_payload.data = NULL;
        state->current_payload.len = 0u;
        state->current_payload.cap = 0u;
    }
    if (!bx_tar_entry_list_push(state->entries, &state->current)) {
        bx_diag(diag, "buffer growth failed: %s", strerror(errno));
        return false;
    }
    memset(&state->current, 0, sizeof(state->current));
    state->have_current = false;
    state->collecting_payload = false;
    return true;
}

bool bx_tar_collect_archive_stream(const struct bx_tar_reader_stream_options* options,
                                   struct bx_tar_entry_list* entries,
                                   struct bx_diag_ctx* diag) {
    struct bx_tar_collect_stream_state state;
    struct bx_tar_stream_visitor_ops visitor_ops = {
        .user = &state,
        .begin_entry = bx_tar_collect_stream_begin_entry,
        .visit_payload = bx_tar_collect_stream_visit_payload,
        .end_entry = bx_tar_collect_stream_end_entry,
    };
    bool ok;

    memset(&state, 0, sizeof(state));
    state.entries = entries;
    bx_archive_buffer_init(&state.current_payload);
    ok = bx_tar_visit_archive_stream(options, &visitor_ops, diag);
    if (!ok && state.have_current) {
        bx_tar_entry_free(&state.current);
    }
    bx_archive_buffer_free(&state.current_payload);
    return ok;
}

bool bx_tar_visit_archive_stream(const struct bx_tar_reader_stream_options* options,
                                 const struct bx_tar_stream_visitor_ops* visitor_ops,
                                 struct bx_diag_ctx* diag) {
    struct bx_tar_stream_input input;
    struct bx_tar_pax_info pax = {0};
    char* gnu_long_name = NULL;
    char* gnu_long_link = NULL;
    unsigned char header[BX_TAR_BLOCK_SIZE];
    bool have_header = false;
    bool ok = false;

    if (options == NULL || visitor_ops == NULL || visitor_ops->begin_entry == NULL) {
        bx_diag(diag, "invalid tar stream reader configuration");
        return false;
    }
    if (!bx_tar_stream_input_open(&input, options, diag)) {
        return false;
    }

    while (true) {
        size_t size = 0u;
        size_t sparse_archive_padding = 0u;
        uint64_t header_block_index = 0u;
        unsigned char typeflag;
        struct bx_tar_entry entry;
        bool eof = false;

        if (!have_header) {
            if (!bx_tar_stream_input_read_exact(&input, header, sizeof(header), &eof, diag)) {
                goto out;
            }
            if (eof) {
                if (!bx_tar_stream_visitor_finish_archive(visitor_ops,
                                                         bx_tar_stream_input_total_bytes_read(&input)
                                                             / BX_TAR_BLOCK_SIZE,
                                                         BX_TAR_STREAM_END_EOF,
                                                         bx_tar_stream_input_total_bytes_read(&input),
                                                         diag)) {
                    goto out;
                }
                ok = true;
                goto out;
            }
        }
        have_header = false;
        header_block_index = (bx_tar_stream_input_total_bytes_read(&input) / BX_TAR_BLOCK_SIZE) - 1u;

        if (bx_tar_block_is_zero(header)) {
            if (!bx_tar_stream_input_read_exact(&input, header, sizeof(header), &eof, diag)) {
                goto out;
            }
            if (eof || bx_tar_block_is_zero(header)) {
                if (!bx_tar_stream_visitor_finish_archive(visitor_ops,
                                                         header_block_index,
                                                         eof
                                                             ? BX_TAR_STREAM_END_EOF
                                                             : BX_TAR_STREAM_END_ZERO_BLOCKS,
                                                         bx_tar_stream_input_total_bytes_read(&input),
                                                         diag)) {
                    goto out;
                }
                if (!bx_tar_stream_input_finish_success(&input, diag)) {
                    goto out;
                }
                ok = true;
                goto out;
            }
            have_header = true;
            continue;
        }

        if (!bx_tar_header_checksum_valid(header)) {
            bx_diag(diag, "invalid tar header");
            goto out;
        }
        if (!bx_tar_parse_octal_field(header + 124, 12u, &size)) {
            bx_diag(diag, "invalid tar header");
            goto out;
        }
        typeflag = header[156];

        if (typeflag == 'x') {
            if (!bx_tar_stream_input_read_pax_records(&input,
                                                      size,
                                                      &pax,
                                                      options->skip_owner_group_names,
                                                      diag)) {
                bx_diag(diag, "invalid pax header");
                goto out;
            }
            continue;
        }
        if (typeflag == 'g') {
            if (!bx_tar_stream_input_skip_payload(&input, size, diag)) {
                goto out;
            }
            continue;
        }
        if (typeflag == 'L' || typeflag == 'K') {
            char** target = (typeflag == 'L') ? &gnu_long_name : &gnu_long_link;

            if (!bx_tar_stream_input_read_text_payload(&input, size, target, diag)) {
                goto out;
            }
            continue;
        }
        if (typeflag == 'V') {
            free(bx_tar_header_name_dup(header, &pax, &gnu_long_name));
            if (!bx_tar_stream_input_skip_payload(&input, size, diag)) {
                goto out;
            }
            bx_tar_pax_info_clear(&pax);
            continue;
        }

        if (!bx_tar_prepare_entry_from_header(header,
                                              size,
                                              typeflag,
                                              &pax,
                                              &gnu_long_name,
                                              &gnu_long_link,
                                              options->skip_owner_group_names,
                                              options->skip_owner_group_ids,
                                              &entry,
                                              diag)) {
            goto out;
        }
        entry.header_block_index = header_block_index;

        if (entry.dumpdir
            && !bx_tar_stream_input_read_payload_buffered(&input,
                                                          size,
                                                          &entry.data,
                                                          &entry.data_len,
                                                          diag)) {
            bx_tar_entry_free(&entry);
            goto out;
        }
        if (entry.kind == BX_TAR_KIND_REG
            && pax.sparse_enabled && pax.sparse_major == 1 && pax.sparse_minor == 0) {
            entry.sparse = true;
            entry.size = pax.sparse_realsize;
            if (!bx_tar_stream_input_prepare_sparse_payload(&input,
                                                            size,
                                                            &entry,
                                                            &sparse_archive_padding,
                                                            diag)) {
                bx_tar_entry_free(&entry);
                goto out;
            }
            if (!visitor_ops->stream_sparse_payload
                && !bx_tar_stream_input_read_sparse_payload_buffered(&input,
                                                                     &entry,
                                                                     sparse_archive_padding,
                                                                     diag)) {
                bx_tar_entry_free(&entry);
                goto out;
            }
        }
        if (!visitor_ops->begin_entry(visitor_ops->user, &entry, diag)) {
            bx_tar_entry_free(&entry);
            goto out;
        }
        if (entry.kind == BX_TAR_KIND_REG && !entry.sparse) {
            if (!bx_tar_stream_input_visit_payload(&input, &entry, size, visitor_ops, diag)) {
                bx_tar_entry_free(&entry);
                goto out;
            }
        }
        else if (entry.kind == BX_TAR_KIND_REG && entry.sparse && visitor_ops->stream_sparse_payload) {
            if (!bx_tar_stream_input_visit_sparse_payload(&input,
                                                          &entry,
                                                          sparse_archive_padding,
                                                          visitor_ops,
                                                          diag)) {
                bx_tar_entry_free(&entry);
                goto out;
            }
        }
        else if (!entry.dumpdir
                 && entry.kind != BX_TAR_KIND_REG
                 && !bx_tar_stream_input_skip_payload(&input, size, diag)) {
            bx_tar_entry_free(&entry);
            goto out;
        }
        if (visitor_ops->end_entry != NULL
            && !visitor_ops->end_entry(visitor_ops->user, &entry, diag)) {
            bx_tar_entry_free(&entry);
            goto out;
        }
        bx_tar_entry_free(&entry);
        bx_tar_pax_info_clear(&pax);
    }

out:
    bx_tar_pax_info_clear(&pax);
    free(gnu_long_name);
    free(gnu_long_link);
    bx_tar_stream_input_close(&input);
    return ok;
}

bool bx_tar_read_volume_label_stream(const struct bx_tar_reader_stream_options* options,
                                     char** label_out,
                                     struct bx_diag_ctx* diag) {
    struct bx_tar_stream_input input;
    struct bx_tar_pax_info pax = {0};
    char* gnu_long_name = NULL;
    char* gnu_long_link = NULL;
    unsigned char header[BX_TAR_BLOCK_SIZE];
    bool have_header = false;
    bool ok = false;

    if (label_out != NULL) {
        free(*label_out);
        *label_out = NULL;
    }
    if (options == NULL || label_out == NULL) {
        bx_diag(diag, "invalid tar stream reader configuration");
        return false;
    }
    if (!bx_tar_stream_input_open(&input, options, diag)) {
        return false;
    }

    while (true) {
        size_t size = 0u;
        unsigned char typeflag;
        bool eof = false;

        if (!have_header) {
            if (!bx_tar_stream_input_read_exact(&input, header, sizeof(header), &eof, diag)) {
                goto out;
            }
            if (eof) {
                ok = true;
                goto out;
            }
        }
        have_header = false;

        if (bx_tar_block_is_zero(header)) {
            if (!bx_tar_stream_input_read_exact(&input, header, sizeof(header), &eof, diag)) {
                goto out;
            }
            if (eof || bx_tar_block_is_zero(header)) {
                if (!bx_tar_stream_input_finish_success(&input, diag)) {
                    goto out;
                }
                ok = true;
                goto out;
            }
            have_header = true;
            continue;
        }

        if (!bx_tar_header_checksum_valid(header)) {
            bx_diag(diag, "invalid tar header");
            goto out;
        }
        if (!bx_tar_parse_octal_field(header + 124, 12u, &size)) {
            bx_diag(diag, "invalid tar header");
            goto out;
        }
        typeflag = header[156];

        if (typeflag == 'x') {
            if (!bx_tar_stream_input_read_pax_records(&input,
                                                      size,
                                                      &pax,
                                                      options->skip_owner_group_names,
                                                      diag)) {
                bx_diag(diag, "invalid pax header");
                goto out;
            }
            continue;
        }
        if (typeflag == 'g') {
            if (!bx_tar_stream_input_skip_payload(&input, size, diag)) {
                goto out;
            }
            continue;
        }
        if (typeflag == 'L' || typeflag == 'K') {
            char** target = (typeflag == 'L') ? &gnu_long_name : &gnu_long_link;

            if (!bx_tar_stream_input_read_text_payload(&input, size, target, diag)) {
                goto out;
            }
            continue;
        }
        if (typeflag == 'V') {
            free(*label_out);
            *label_out = bx_tar_header_name_dup(header, &pax, &gnu_long_name);
        }
        if (!bx_tar_stream_input_skip_payload(&input, size, diag)) {
            goto out;
        }
        bx_tar_pax_info_clear(&pax);
    }

out:
    bx_tar_pax_info_clear(&pax);
    free(gnu_long_name);
    free(gnu_long_link);
    bx_tar_stream_input_close(&input);
    return ok;
}
