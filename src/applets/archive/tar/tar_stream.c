#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "applets/archive/archive_common.h"
#include "applets/archive/tar/tar_reader.h"
#include "applets/archive/tar/tar_stream.h"
#include "bx/libbx.h"

#define BX_TAR_STREAM_BLOCK_SIZE 512u
#define BX_TAR_STREAM_RECORD_BLOCKS 20u
#define BX_TAR_STREAM_FILE_BUFFER_SIZE (1024u * 1024u)
#define BX_TAR_STREAM_FILE_CHUNK_SIZE (256u * 1024u)

struct bx_tar_hardlink_seen {
    dev_t dev;
    ino_t ino;
    char* first_name;
};

struct bx_tar_hardlink_seen_list {
    struct bx_tar_hardlink_seen* items;
    size_t len;
    size_t cap;
};

struct bx_tar_stream_counting_sink {
    const struct bx_tar_stream_sink* inner;
    size_t* bytes_written;
};

bool bx_tar_stream_write_raw_entry_chunk(struct bx_tar_stream_live_entry* entry,
                                         const void* data,
                                         size_t len,
                                         struct bx_diag_ctx* diag);

static size_t bx_tar_stream_round_up(size_t value, size_t align) {
    size_t rem = value % align;
    if (rem == 0u) {
        return value;
    }
    return value + (align - rem);
}

static bool bx_tar_stream_sink_write(const struct bx_tar_stream_sink* sink,
                                     const void* data,
                                     size_t len,
                                     struct bx_diag_ctx* diag) {
    if (len == 0u) {
        return true;
    }
    if (sink->write(sink->user, data, len)) {
        return true;
    }
    if (!sink->callback_owns_errors) {
        bx_diag(diag, "write error: %s", strerror(errno));
    }
    return false;
}

static bool bx_tar_stream_counting_sink_write(void* user, const void* data, size_t len) {
    struct bx_tar_stream_counting_sink* sink = user;

    if (!sink->inner->write(sink->inner->user, data, len)) {
        return false;
    }
    *sink->bytes_written += len;
    return true;
}

static void bx_tar_stream_format_octal_field(unsigned char* field, size_t len, size_t value) {
    char text[32];
    size_t text_len;
    memset(field, 0, len);
    snprintf(text, sizeof(text), "%0*lo", (int)(len - 1u), (unsigned long)value);
    text_len = strlen(text);
    if (text_len >= len) {
        memcpy(field, text + (text_len - (len - 1u)), len - 1u);
    }
    else {
        memcpy(field + (len - 1u - text_len), text, text_len);
    }
}

static void bx_tar_stream_write_checksum(unsigned char* header) {
    unsigned int sum = 0u;
    size_t i;
    memset(header + 148, ' ', 8u);
    for (i = 0u; i < BX_TAR_STREAM_BLOCK_SIZE; i++) {
        sum += header[i];
    }
    snprintf((char*)header + 148, 8u, "%06o", sum);
    header[154] = '\0';
    header[155] = ' ';
}

static bool bx_tar_stream_split_ustar_name(const char* path,
                                           bool directory,
                                           unsigned char* name_out,
                                           unsigned char* prefix_out) {
    char* stored = NULL;
    const char* slash;
    size_t len;
    bool ok = false;

    memset(name_out, 0, 100u);
    memset(prefix_out, 0, 155u);

    if (directory) {
        size_t path_len = strlen(path);
        stored = xmalloc(path_len + 2u);
        memcpy(stored, path, path_len);
        stored[path_len] = '/';
        stored[path_len + 1u] = '\0';
    }
    else {
        stored = xstrdup(path);
    }

    len = strlen(stored);
    if (len <= 100u) {
        memcpy(name_out, stored, len);
        ok = true;
        goto out;
    }

    slash = strrchr(stored, '/');
    while (slash != NULL) {
        size_t prefix_len = (size_t)(slash - stored);
        size_t name_len = len - prefix_len - 1u;
        if (prefix_len <= 155u && name_len <= 100u) {
            memcpy(prefix_out, stored, prefix_len);
            memcpy(name_out, slash + 1, name_len);
            ok = true;
            goto out;
        }
        if (slash == stored) {
            break;
        }
        {
            char* tmp = xmalloc(prefix_len + 1u);
            memcpy(tmp, stored, prefix_len);
            tmp[prefix_len] = '\0';
            slash = strrchr(tmp, '/');
            if (slash != NULL) {
                size_t next_offset = (size_t)(slash - tmp);
                free(tmp);
                slash = stored + next_offset;
            }
            else {
                free(tmp);
                slash = NULL;
            }
        }
    }

out:
    free(stored);
    return ok;
}

static size_t bx_tar_stream_decimal_digits(size_t value) {
    size_t digits = 1u;
    while (value >= 10u) {
        value /= 10u;
        digits++;
    }
    return digits;
}

static bool bx_tar_stream_pax_append_record(struct bx_archive_buffer* buffer,
                                            const char* key,
                                            const char* value) {
    size_t payload_len = strlen(key) + 1u + strlen(value) + 1u;
    size_t digits = bx_tar_stream_decimal_digits(payload_len + 2u);
    size_t total;
    char prefix[32];

    while (true) {
        total = payload_len + digits + 1u;
        if (bx_tar_stream_decimal_digits(total) == digits) {
            break;
        }
        digits = bx_tar_stream_decimal_digits(total);
    }

    snprintf(prefix, sizeof(prefix), "%zu ", total);
    return bx_archive_buffer_append(buffer, prefix, strlen(prefix))
        && bx_archive_buffer_append(buffer, key, strlen(key))
        && bx_archive_buffer_append_byte(buffer, '=')
        && bx_archive_buffer_append(buffer, value, strlen(value))
        && bx_archive_buffer_append_byte(buffer, '\n');
}

static bool bx_tar_stream_pax_append_size_record(struct bx_archive_buffer* buffer,
                                                 const char* key,
                                                 size_t value) {
    char text[32];

    snprintf(text, sizeof(text), "%zu", value);
    return bx_tar_stream_pax_append_record(buffer, key, text);
}

static bool bx_tar_stream_append_raw_header(const struct bx_tar_stream_sink* sink,
                                            const char* path,
                                            const char* linkname,
                                            char typeflag,
                                            mode_t mode,
                                            uid_t uid,
                                            gid_t gid,
                                            size_t size,
                                            struct timespec mtime,
                                            bool directory,
                                            struct bx_diag_ctx* diag) {
    unsigned char header[BX_TAR_STREAM_BLOCK_SIZE];
    unsigned char name[100u];
    unsigned char prefix[155u];

    if (!bx_tar_stream_split_ustar_name(path, directory, name, prefix)) {
        bx_diag(diag, "%s: file name too long", path);
        return false;
    }

    memset(header, 0, sizeof(header));
    memcpy(header, name, sizeof(name));
    bx_tar_stream_format_octal_field(header + 100, 8u, mode & 07777u);
    bx_tar_stream_format_octal_field(header + 108, 8u, uid);
    bx_tar_stream_format_octal_field(header + 116, 8u, gid);
    bx_tar_stream_format_octal_field(header + 124, 12u, size);
    bx_tar_stream_format_octal_field(header + 136, 12u, (size_t)mtime.tv_sec);
    header[156] = (unsigned char)typeflag;
    if (linkname != NULL) {
        size_t link_len = strlen(linkname);
        if (link_len > 100u) {
            link_len = 100u;
        }
        memcpy(header + 157, linkname, link_len);
    }
    memcpy(header + 257, "ustar", 5u);
    memcpy(header + 263, "00", 2u);
    memcpy(header + 345, prefix, sizeof(prefix));
    bx_tar_stream_write_checksum(header);
    return bx_tar_stream_sink_write(sink, header, sizeof(header), diag);
}

static bool bx_tar_stream_write_header(const struct bx_tar_stream_sink* sink,
                                       const char* path,
                                       const char* linkname,
                                       char typeflag,
                                       bool is_dir,
                                       mode_t mode,
                                       uid_t uid,
                                       gid_t gid,
                                       size_t size,
                                       struct timespec mtime,
                                       bool allow_pax,
                                       struct bx_diag_ctx* diag) {
    bool need_path_pax;
    bool need_link_pax = false;
    const char* stored_link = linkname;
    const char* actual_header_path = path;

    need_path_pax = !bx_tar_stream_split_ustar_name(path,
                                                    is_dir,
                                                    (unsigned char[100]){0},
                                                    (unsigned char[155]){0});
    if (linkname != NULL && strlen(linkname) > 100u) {
        need_link_pax = true;
    }

    if ((need_path_pax || need_link_pax) && !allow_pax) {
        bx_diag(diag, "%s: file name too long", path);
        return false;
    }

    if (need_path_pax || need_link_pax) {
        struct timespec zero_time = {0, 0};
        struct bx_archive_buffer pax_data = {0};
        size_t pax_size;

        bx_archive_buffer_init(&pax_data);
        if ((need_path_pax && !bx_tar_stream_pax_append_record(&pax_data, "path", path))
            || (need_link_pax && !bx_tar_stream_pax_append_record(&pax_data, "linkpath", linkname))) {
            bx_archive_buffer_free(&pax_data);
            bx_diag(diag, "archive write failed: %s", strerror(errno));
            return false;
        }
        if (!bx_tar_stream_append_raw_header(sink,
                                             "./PaxHeaders/bx",
                                             NULL,
                                             'x',
                                             0644u,
                                             0u,
                                             0u,
                                             pax_data.len,
                                             zero_time,
                                             false,
                                             diag)) {
            bx_archive_buffer_free(&pax_data);
            return false;
        }
        if (!bx_tar_stream_sink_write(sink, pax_data.data, pax_data.len, diag)) {
            bx_archive_buffer_free(&pax_data);
            return false;
        }
        pax_size = bx_tar_stream_round_up(pax_data.len, BX_TAR_STREAM_BLOCK_SIZE);
        if (pax_size > pax_data.len) {
            unsigned char zeros[BX_TAR_STREAM_BLOCK_SIZE] = {0};
            if (!bx_tar_stream_sink_write(sink, zeros, pax_size - pax_data.len, diag)) {
                bx_archive_buffer_free(&pax_data);
                return false;
            }
        }
        bx_archive_buffer_free(&pax_data);

        actual_header_path = need_path_pax ? "PaxPayload" : path;
        if (need_link_pax) {
            stored_link = "";
        }
    }

    return bx_tar_stream_append_raw_header(sink,
                                           actual_header_path,
                                           stored_link,
                                           typeflag,
                                           mode,
                                           uid,
                                           gid,
                                           typeflag == '0' ? size : 0u,
                                           mtime,
                                           is_dir,
                                           diag);
}

static bool bx_tar_stream_write_entry_data(const struct bx_tar_stream_sink* sink,
                                           const unsigned char* data,
                                           size_t len,
                                           struct bx_diag_ctx* diag) {
    size_t padded = bx_tar_stream_round_up(len, BX_TAR_STREAM_BLOCK_SIZE);
    unsigned char zeros[BX_TAR_STREAM_BLOCK_SIZE] = {0};

    if (!bx_tar_stream_sink_write(sink, data, len, diag)) {
        return false;
    }
    if (padded > len) {
        return bx_tar_stream_sink_write(sink, zeros, padded - len, diag);
    }
    return true;
}

static size_t bx_tar_stream_sparse_map_size(const struct bx_tar_sparse_extent* extents,
                                            size_t extent_count,
                                            bool* overflow_out) {
    size_t total = bx_tar_stream_decimal_digits(extent_count) + 1u;
    size_t i;

    *overflow_out = false;
    for (i = 0u; i < extent_count; i++) {
        size_t line_size = bx_tar_stream_decimal_digits(extents[i].offset) + 1u;

        if (line_size > SIZE_MAX - total) {
            *overflow_out = true;
            return 0u;
        }
        total += line_size;
        line_size = bx_tar_stream_decimal_digits(extents[i].size) + 1u;
        if (line_size > SIZE_MAX - total) {
            *overflow_out = true;
            return 0u;
        }
        total += line_size;
    }

    return total;
}

static bool bx_tar_stream_write_sparse_number_line(struct bx_tar_stream_live_entry* entry,
                                                   size_t value,
                                                   struct bx_diag_ctx* diag) {
    char line[64];
    int len = snprintf(line, sizeof(line), "%zu\n", value);

    if (len < 0 || (size_t)len >= sizeof(line)) {
        bx_diag(diag, "invalid sparse map");
        return false;
    }
    return bx_tar_stream_write_raw_entry_chunk(entry, line, (size_t)len, diag);
}

static bool bx_tar_stream_write_file_data(const struct bx_tar_stream_sink* sink,
                                          const char* path,
                                          size_t expected_size,
                                          struct bx_diag_ctx* diag) {
    unsigned char buffer[BX_TAR_STREAM_FILE_CHUNK_SIZE];
    FILE* stream = fopen(path, "rb");
    size_t total = 0u;

    if (stream == NULL) {
        bx_diag(diag, "%s: %s", path, strerror(errno));
        return false;
    }
    setvbuf(stream, NULL, _IOFBF, BX_TAR_STREAM_FILE_BUFFER_SIZE);

    while (total < expected_size) {
        size_t chunk = expected_size - total;
        size_t nread;

        if (chunk > sizeof(buffer)) {
            chunk = sizeof(buffer);
        }
        nread = fread(buffer, 1u, chunk, stream);
        if (nread != chunk) {
            if (ferror(stream)) {
                bx_diag(diag, "%s: %s", path, strerror(errno));
            }
            else {
                bx_diag(diag, "%s: file shrank while reading", path);
            }
            fclose(stream);
            return false;
        }
        if (!bx_tar_stream_sink_write(sink, buffer, nread, diag)) {
            fclose(stream);
            return false;
        }
        total += nread;
    }

    if (fclose(stream) != 0) {
        bx_diag(diag, "%s: %s", path, strerror(errno));
        return false;
    }
    return true;
}

static ssize_t bx_tar_stream_find_seen_hardlink(const struct bx_tar_hardlink_seen_list* seen,
                                                dev_t dev,
                                                ino_t ino) {
    size_t i;
    for (i = 0u; i < seen->len; i++) {
        if (seen->items[i].dev == dev && seen->items[i].ino == ino) {
            return (ssize_t)i;
        }
    }
    return -1;
}

static bool bx_tar_stream_record_seen_hardlink(struct bx_tar_hardlink_seen_list* seen,
                                               dev_t dev,
                                               ino_t ino,
                                               const char* name) {
    struct bx_tar_hardlink_seen* slot;
    if (seen->len == seen->cap) {
        size_t next_cap = seen->cap ? seen->cap * 2u : 16u;
        seen->items = xrealloc(seen->items, next_cap * sizeof(*seen->items));
        seen->cap = next_cap;
    }
    slot = &seen->items[seen->len++];
    slot->dev = dev;
    slot->ino = ino;
    slot->first_name = xstrdup(name);
    return true;
}

static void bx_tar_stream_seen_list_free(struct bx_tar_hardlink_seen_list* seen) {
    size_t i;
    for (i = 0u; i < seen->len; i++) {
        free(seen->items[i].first_name);
    }
    free(seen->items);
    seen->items = NULL;
    seen->len = 0u;
    seen->cap = 0u;
}

static bool bx_tar_stream_write_fs_entry(const struct bx_tar_stream_sink* sink,
                                         const struct bx_archive_fs_entry* fs_entry,
                                         const struct bx_tar_stream_options* options,
                                         struct bx_tar_hardlink_seen_list* seen,
                                         struct bx_diag_ctx* diag) {
    mode_t mode = fs_entry->st.st_mode & 07777u;
    uid_t uid = options->owner_set ? options->owner : fs_entry->st.st_uid;
    gid_t gid = options->group_set ? options->group : fs_entry->st.st_gid;
    struct timespec mtime = options->fixed_mtime ? options->mtime : fs_entry->st.st_mtim;
    unsigned char zeros[BX_TAR_STREAM_BLOCK_SIZE] = {0};
    size_t file_size = (size_t)fs_entry->st.st_size;
    size_t padded_size = bx_tar_stream_round_up(file_size, BX_TAR_STREAM_BLOCK_SIZE);

    if (S_ISDIR(fs_entry->st.st_mode)) {
        return bx_tar_stream_write_raw_entry(sink,
                                             fs_entry->archive_path,
                                             NULL,
                                             BX_TAR_STREAM_KIND_DIR,
                                             mode,
                                             uid,
                                             gid,
                                             NULL,
                                             0u,
                                             mtime,
                                             !options->format_ustar,
                                             diag);
    }
    if (S_ISLNK(fs_entry->st.st_mode)) {
        return bx_tar_stream_write_raw_entry(sink,
                                             fs_entry->archive_path,
                                             fs_entry->link_target,
                                             BX_TAR_STREAM_KIND_SYMLINK,
                                             mode,
                                             uid,
                                             gid,
                                             NULL,
                                             0u,
                                             mtime,
                                             !options->format_ustar,
                                             diag);
    }
    if (S_ISFIFO(fs_entry->st.st_mode)) {
        return bx_tar_stream_write_raw_entry(sink,
                                             fs_entry->archive_path,
                                             NULL,
                                             BX_TAR_STREAM_KIND_FIFO,
                                             mode,
                                             uid,
                                             gid,
                                             NULL,
                                             0u,
                                             mtime,
                                             !options->format_ustar,
                                             diag);
    }
    if (!S_ISREG(fs_entry->st.st_mode)) {
        bx_diag(diag, "%s: unsupported file type", fs_entry->source_path);
        return false;
    }

    if (fs_entry->st.st_nlink > 1) {
        ssize_t index = bx_tar_stream_find_seen_hardlink(seen, fs_entry->st.st_dev, fs_entry->st.st_ino);
        if (index >= 0) {
            return bx_tar_stream_write_raw_entry(sink,
                                                 fs_entry->archive_path,
                                                 seen->items[index].first_name,
                                                 BX_TAR_STREAM_KIND_HARDLINK,
                                                 mode,
                                                 uid,
                                                 gid,
                                                 NULL,
                                                 0u,
                                                 mtime,
                                                 !options->format_ustar,
                                                 diag);
        }
        bx_tar_stream_record_seen_hardlink(seen,
                                           fs_entry->st.st_dev,
                                           fs_entry->st.st_ino,
                                           fs_entry->archive_path);
    }

    if (!bx_tar_stream_write_raw_entry(sink,
                                       fs_entry->archive_path,
                                       NULL,
                                       BX_TAR_STREAM_KIND_REG,
                                       mode,
                                       uid,
                                       gid,
                                       NULL,
                                       file_size,
                                       mtime,
                                        !options->format_ustar,
                                       diag)) {
        return false;
    }
    if (!bx_tar_stream_write_file_data(sink, fs_entry->source_path, file_size, diag)) {
        return false;
    }
    if (padded_size > file_size
        && !bx_tar_stream_sink_write(sink, zeros, padded_size - file_size, diag)) {
        return false;
    }
    return true;
}

static bool bx_tar_stream_finish_archive(const struct bx_tar_stream_sink* sink,
                                         size_t bytes_written,
                                         struct bx_diag_ctx* diag) {
    size_t with_trailer = bytes_written + 2u * BX_TAR_STREAM_BLOCK_SIZE;
    size_t padded = bx_tar_stream_round_up(with_trailer,
                                           BX_TAR_STREAM_BLOCK_SIZE * BX_TAR_STREAM_RECORD_BLOCKS);
    size_t zeros_needed = padded - bytes_written;
    unsigned char zeros[BX_TAR_STREAM_BLOCK_SIZE * BX_TAR_STREAM_RECORD_BLOCKS] = {0};

    while (zeros_needed > 0u) {
        size_t chunk = zeros_needed > sizeof(zeros) ? sizeof(zeros) : zeros_needed;

        if (!bx_tar_stream_sink_write(sink, zeros, chunk, diag)) {
            return false;
        }
        zeros_needed -= chunk;
    }
    return true;
}

bool bx_tar_stream_write_raw_entry(const struct bx_tar_stream_sink* sink,
                                   const char* path,
                                   const char* linkname,
                                   enum bx_tar_stream_kind kind,
                                   mode_t mode,
                                   uid_t uid,
                                   gid_t gid,
                                   const unsigned char* data,
                                   size_t data_len,
                                   struct timespec mtime,
                                   bool allow_pax,
                                   struct bx_diag_ctx* diag) {
    bool is_dir = false;
    char typeflag = '0';

    switch (kind) {
        case BX_TAR_STREAM_KIND_REG:
            typeflag = '0';
            break;
        case BX_TAR_STREAM_KIND_DIR:
            typeflag = '5';
            is_dir = true;
            data_len = 0u;
            break;
        case BX_TAR_STREAM_KIND_SYMLINK:
            typeflag = '2';
            data_len = 0u;
            break;
        case BX_TAR_STREAM_KIND_HARDLINK:
            typeflag = '1';
            data_len = 0u;
            break;
        case BX_TAR_STREAM_KIND_FIFO:
            typeflag = '6';
            data_len = 0u;
            break;
    }

    if (!bx_tar_stream_write_header(sink,
                                    path,
                                    linkname,
                                    typeflag,
                                    is_dir,
                                    mode,
                                    uid,
                                    gid,
                                    data_len,
                                    mtime,
                                    allow_pax,
                                    diag)) {
        return false;
    }
    if (kind == BX_TAR_STREAM_KIND_REG && data != NULL) {
        return bx_tar_stream_write_entry_data(sink, data, data_len, diag);
    }
    return true;
}

bool bx_tar_stream_start_raw_entry(struct bx_tar_stream_live_entry* entry,
                                   const struct bx_tar_stream_sink* sink,
                                   const char* path,
                                   const char* linkname,
                                   enum bx_tar_stream_kind kind,
                                   mode_t mode,
                                   uid_t uid,
                                   gid_t gid,
                                   size_t data_len,
                                   struct timespec mtime,
                                   bool allow_pax,
                                   struct bx_diag_ctx* diag) {
    bool is_dir = false;
    char typeflag = '0';

    if (entry == NULL || sink == NULL) {
        bx_diag(diag, "invalid tar stream live entry");
        return false;
    }

    memset(entry, 0, sizeof(*entry));
    switch (kind) {
        case BX_TAR_STREAM_KIND_REG:
            typeflag = '0';
            break;
        case BX_TAR_STREAM_KIND_DIR:
            typeflag = '5';
            is_dir = true;
            data_len = 0u;
            break;
        case BX_TAR_STREAM_KIND_SYMLINK:
            typeflag = '2';
            data_len = 0u;
            break;
        case BX_TAR_STREAM_KIND_HARDLINK:
            typeflag = '1';
            data_len = 0u;
            break;
        case BX_TAR_STREAM_KIND_FIFO:
            typeflag = '6';
            data_len = 0u;
            break;
    }

    if (!bx_tar_stream_write_header(sink,
                                    path,
                                    linkname,
                                    typeflag,
                                    is_dir,
                                    mode,
                                    uid,
                                    gid,
                                    data_len,
                                    mtime,
                                    allow_pax,
                                    diag)) {
        return false;
    }

    entry->sink = sink;
    entry->data_remaining = data_len;
    entry->padding_remaining = bx_tar_stream_round_up(data_len, BX_TAR_STREAM_BLOCK_SIZE) - data_len;
    entry->active = true;
    return true;
}

bool bx_tar_stream_start_sparse_v1_entry(struct bx_tar_stream_live_entry* entry,
                                         const struct bx_tar_stream_sink* sink,
                                         const char* path,
                                         mode_t mode,
                                         uid_t uid,
                                         gid_t gid,
                                         const struct bx_tar_sparse_extent* extents,
                                         size_t extent_count,
                                         size_t logical_size,
                                         size_t compact_size,
                                         struct timespec mtime,
                                         struct bx_diag_ctx* diag) {
    struct bx_archive_buffer pax_data = {0};
    struct timespec zero_time = {0, 0};
    size_t map_size;
    size_t padded_map_size;
    size_t payload_size;
    size_t expected_compact_size = 0u;
    size_t i;
    bool overflow = false;

    if (entry == NULL || sink == NULL || (extent_count > 0u && extents == NULL)) {
        bx_diag(diag, "invalid sparse tar stream live entry");
        return false;
    }

    for (i = 0u; i < extent_count; i++) {
        if (extents[i].size > SIZE_MAX - expected_compact_size) {
            bx_diag(diag, "invalid sparse map");
            return false;
        }
        expected_compact_size += extents[i].size;
    }
    if (expected_compact_size != compact_size) {
        bx_diag(diag, "invalid sparse payload");
        return false;
    }

    bx_archive_buffer_init(&pax_data);
    if (!bx_tar_stream_pax_append_record(&pax_data, "path", path)
        || !bx_tar_stream_pax_append_record(&pax_data, "GNU.sparse.major", "1")
        || !bx_tar_stream_pax_append_record(&pax_data, "GNU.sparse.minor", "0")
        || !bx_tar_stream_pax_append_size_record(&pax_data, "GNU.sparse.realsize", logical_size)) {
        bx_archive_buffer_free(&pax_data);
        bx_diag(diag, "archive write failed: %s", strerror(errno));
        return false;
    }
    if (!bx_tar_stream_append_raw_header(sink,
                                         "./PaxHeaders/bx",
                                         NULL,
                                         'x',
                                         0644u,
                                         0u,
                                         0u,
                                         pax_data.len,
                                         zero_time,
                                         false,
                                         diag)) {
        bx_archive_buffer_free(&pax_data);
        return false;
    }
    if (!bx_tar_stream_sink_write(sink, pax_data.data, pax_data.len, diag)) {
        bx_archive_buffer_free(&pax_data);
        return false;
    }
    {
        size_t pax_size = bx_tar_stream_round_up(pax_data.len, BX_TAR_STREAM_BLOCK_SIZE);

        if (pax_size > pax_data.len) {
            unsigned char zeros[BX_TAR_STREAM_BLOCK_SIZE] = {0};
            if (!bx_tar_stream_sink_write(sink, zeros, pax_size - pax_data.len, diag)) {
                bx_archive_buffer_free(&pax_data);
                return false;
            }
        }
    }
    bx_archive_buffer_free(&pax_data);

    map_size = bx_tar_stream_sparse_map_size(extents, extent_count, &overflow);
    if (overflow) {
        bx_diag(diag, "invalid sparse map");
        return false;
    }
    padded_map_size = bx_tar_stream_round_up(map_size, BX_TAR_STREAM_BLOCK_SIZE);
    if (padded_map_size < map_size || compact_size > SIZE_MAX - padded_map_size) {
        bx_diag(diag, "invalid sparse payload");
        return false;
    }
    payload_size = padded_map_size + compact_size;

    if (!bx_tar_stream_start_raw_entry(entry,
                                       sink,
                                       "PaxPayload",
                                       NULL,
                                       BX_TAR_STREAM_KIND_REG,
                                       mode,
                                       uid,
                                       gid,
                                       payload_size,
                                       mtime,
                                       true,
                                       diag)) {
        return false;
    }
    if (!bx_tar_stream_write_sparse_number_line(entry, extent_count, diag)) {
        return false;
    }
    for (i = 0u; i < extent_count; i++) {
        if (!bx_tar_stream_write_sparse_number_line(entry, extents[i].offset, diag)
            || !bx_tar_stream_write_sparse_number_line(entry, extents[i].size, diag)) {
            return false;
        }
    }
    if (padded_map_size > map_size) {
        unsigned char zeros[BX_TAR_STREAM_BLOCK_SIZE] = {0};
        size_t remaining = padded_map_size - map_size;

        while (remaining > 0u) {
            size_t chunk = remaining > sizeof(zeros) ? sizeof(zeros) : remaining;

            if (!bx_tar_stream_write_raw_entry_chunk(entry, zeros, chunk, diag)) {
                return false;
            }
            remaining -= chunk;
        }
    }
    return true;
}

bool bx_tar_stream_write_raw_entry_chunk(struct bx_tar_stream_live_entry* entry,
                                         const void* data,
                                         size_t len,
                                         struct bx_diag_ctx* diag) {
    if (entry == NULL || !entry->active) {
        bx_diag(diag, "invalid tar stream live entry state");
        return false;
    }
    if (len > entry->data_remaining) {
        bx_diag(diag, "tar entry payload overflow");
        return false;
    }
    if (!bx_tar_stream_sink_write(entry->sink, data, len, diag)) {
        return false;
    }
    entry->data_remaining -= len;
    return true;
}

bool bx_tar_stream_finish_raw_entry(struct bx_tar_stream_live_entry* entry,
                                    struct bx_diag_ctx* diag) {
    unsigned char zeros[BX_TAR_STREAM_BLOCK_SIZE] = {0};

    if (entry == NULL || !entry->active) {
        bx_diag(diag, "invalid tar stream live entry state");
        return false;
    }
    if (entry->data_remaining != 0u) {
        bx_diag(diag, "truncated tar entry payload");
        return false;
    }

    while (entry->padding_remaining > 0u) {
        size_t chunk = entry->padding_remaining > sizeof(zeros)
            ? sizeof(zeros)
            : entry->padding_remaining;

        if (!bx_tar_stream_sink_write(entry->sink, zeros, chunk, diag)) {
            return false;
        }
        entry->padding_remaining -= chunk;
    }

    memset(entry, 0, sizeof(*entry));
    return true;
}

bool bx_tar_stream_write_trailer(const struct bx_tar_stream_sink* sink,
                                 size_t bytes_written,
                                 struct bx_diag_ctx* diag) {
    return bx_tar_stream_finish_archive(sink, bytes_written, diag);
}

bool bx_tar_stream_write_fs_list_body(const struct bx_archive_fs_list* files,
                                      const struct bx_tar_stream_options* options,
                                      const struct bx_tar_stream_sink* sink,
                                      size_t* bytes_written_io,
                                      struct bx_diag_ctx* diag) {
    struct bx_tar_hardlink_seen_list seen = {0};
    struct bx_tar_stream_counting_sink counting_user = {
        .inner = sink,
        .bytes_written = NULL,
    };
    struct bx_tar_stream_sink counting_sink = {
        .user = &counting_user,
        .write = bx_tar_stream_counting_sink_write,
    };
    size_t i;

    if (bytes_written_io == NULL) {
        bx_diag(diag, "invalid tar stream byte counter");
        return false;
    }
    counting_user.bytes_written = bytes_written_io;

    for (i = 0u; i < files->len; i++) {
        if (!bx_tar_stream_write_fs_entry(&counting_sink, &files->entries[i], options, &seen, diag)) {
            bx_tar_stream_seen_list_free(&seen);
            return false;
        }
    }

    bx_tar_stream_seen_list_free(&seen);
    return true;
}

bool bx_tar_stream_encode_fs_list(const struct bx_archive_fs_list* files,
                                  const struct bx_tar_stream_options* options,
                                  const struct bx_tar_stream_sink* sink,
                                  struct bx_diag_ctx* diag) {
    size_t bytes_written = 0u;

    if (!bx_tar_stream_write_fs_list_body(files, options, sink, &bytes_written, diag)) {
        return false;
    }
    return bx_tar_stream_finish_archive(sink, bytes_written, diag);
}
