#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include "applets/archive/archive_common.h"
#include "applets/archive/archive_temp.h"
#include "applets/archive/tar/tar_reader.h"
#include "applets/archive/tar/tar_stream.h"
#include "bx/libbx.h"
#include "lib/fd_ops.h"
#include "lib/dir_path.h"
#include "lib/id_parse.h"
#include "lib/mode_parse.h"
#include "lib/path_ops.h"
#include "lib/xreadwrite.h"

#ifdef S_ISVTX
#define BX_TAR_STICKY_BIT S_ISVTX
#elif defined(S_ISTXT)
#define BX_TAR_STICKY_BIT S_ISTXT
#else
#define BX_TAR_STICKY_BIT 01000
#endif

#define BX_TAR_STREAM_BLOCK_SIZE 512u
#define BX_TAR_STREAM_RECORD_BLOCKS 20u
#define BX_TAR_STREAM_FILE_BUFFER_SIZE (1024u * 1024u)
#define BX_TAR_STREAM_ID_NAME_CACHE_SIZE 16u
#define BX_TAR_USTAR_ID_MAX UINTMAX_C(07777777)
#define BX_TAR_USTAR_SIZE_TIME_MAX UINTMAX_C(077777777777)

static const unsigned char bx_tar_stream_zero_block[BX_TAR_STREAM_BLOCK_SIZE];
static const unsigned char
    bx_tar_stream_zero_record[BX_TAR_STREAM_BLOCK_SIZE * BX_TAR_STREAM_RECORD_BLOCKS];
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

struct bx_tar_stream_id_name_cache_entry {
    uintmax_t id;
    char* name;
    bool valid;
};

struct bx_tar_stream_id_name_cache {
    struct bx_tar_stream_id_name_cache_entry entries[BX_TAR_STREAM_ID_NAME_CACHE_SIZE];
    size_t next_slot;
    size_t last_slot;
    bool last_valid;
};

struct bx_tar_stream_name_caches {
    struct bx_tar_stream_id_name_cache users;
    struct bx_tar_stream_id_name_cache groups;
};

struct bx_tar_stream_ustar_name {
    unsigned char name[100u];
    unsigned char prefix[155u];
    size_t name_len;
    size_t prefix_len;
};

struct bx_tar_stream_fs_write_state {
    const struct bx_tar_stream_sink* sink;
    const struct bx_tar_stream_options* options;
    struct bx_tar_hardlink_seen_list seen;
    struct bx_tar_stream_name_caches name_caches;
    unsigned char* file_buffer;
    size_t file_buffer_size;
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

static void bx_tar_stream_id_name_cache_cleanup(struct bx_tar_stream_id_name_cache* cache) {
    size_t i;

    for (i = 0u; i < BX_TAR_STREAM_ID_NAME_CACHE_SIZE; i++) {
        free(cache->entries[i].name);
        cache->entries[i].name = NULL;
        cache->entries[i].valid = false;
    }
    cache->next_slot = 0u;
    cache->last_slot = 0u;
    cache->last_valid = false;
}

static const char* bx_tar_stream_id_name_cache_lookup(struct bx_tar_stream_id_name_cache* cache,
                                                      uintmax_t id) {
    if (cache->last_valid
        && cache->entries[cache->last_slot].valid
        && cache->entries[cache->last_slot].id == id) {
        return cache->entries[cache->last_slot].name;
    }
    for (size_t i = 0u; i < BX_TAR_STREAM_ID_NAME_CACHE_SIZE; i++) {
        if (cache->entries[i].valid && cache->entries[i].id == id) {
            cache->last_slot = i;
            cache->last_valid = true;
            return cache->entries[i].name;
        }
    }

    return NULL;
}

static const char* bx_tar_stream_id_name_cache_remember(struct bx_tar_stream_id_name_cache* cache,
                                                        uintmax_t id,
                                                        const char* name) {
    struct bx_tar_stream_id_name_cache_entry* entry;

    entry = &cache->entries[cache->next_slot];
    cache->last_slot = cache->next_slot;
    cache->last_valid = true;
    cache->next_slot = (cache->next_slot + 1u) % BX_TAR_STREAM_ID_NAME_CACHE_SIZE;
    free(entry->name);
    entry->name = xstrdup(name);
    entry->id = id;
    entry->valid = true;
    return entry->name;
}

static const char* bx_tar_stream_user_name(struct bx_tar_stream_name_caches* caches, uid_t uid) {
    const char* cached = bx_tar_stream_id_name_cache_lookup(&caches->users, (uintmax_t)uid);
    char numeric_buffer[32];

    if (cached != NULL) {
        return cached;
    }
    return bx_tar_stream_id_name_cache_remember(&caches->users,
                                                (uintmax_t)uid,
                                                bx_id_user_name(uid, numeric_buffer));
}

static const char* bx_tar_stream_group_name(struct bx_tar_stream_name_caches* caches, gid_t gid) {
    const char* cached = bx_tar_stream_id_name_cache_lookup(&caches->groups, (uintmax_t)gid);
    char numeric_buffer[32];

    if (cached != NULL) {
        return cached;
    }
    return bx_tar_stream_id_name_cache_remember(&caches->groups,
                                                (uintmax_t)gid,
                                                bx_id_group_name(gid, numeric_buffer));
}

static unsigned int bx_tar_stream_format_octal_field(unsigned char* field, size_t len, size_t value) {
    unsigned int sum = 0u;
    size_t i;

    memset(field, '0', len - 1u);
    field[len - 1u] = '\0';
    sum = (unsigned int)((len - 1u) * (unsigned int)'0');

    i = len - 1u;
    while (i > 0u && value > 0u) {
        unsigned char digit = (unsigned char)('0' + (value & 7u));

        field[--i] = digit;
        sum += (unsigned int)(digit - (unsigned char)'0');
        value >>= 3u;
    }
    return sum;
}

static unsigned int bx_tar_stream_copy_bytes(unsigned char* dest, const unsigned char* src, size_t len) {
    unsigned int sum = 0u;
    size_t i;

    memcpy(dest, src, len);
    for (i = 0u; i < len; i++) {
        sum += src[i];
    }
    return sum;
}

static unsigned int bx_tar_stream_copy_text(unsigned char* dest,
                                            size_t field_len,
                                            const char* text,
                                            size_t text_len) {
    if (text_len > field_len) {
        text_len = field_len;
    }
    return bx_tar_stream_copy_bytes(dest, (const unsigned char*)text, text_len);
}

static void bx_tar_stream_write_checksum_field(unsigned char* field, unsigned int sum) {
    bx_tar_stream_format_octal_field(field, 7u, sum);
    field[6] = '\0';
    field[7] = ' ';
}

static bool bx_tar_stream_split_ustar_name(const char* path,
                                           bool directory,
                                           struct bx_tar_stream_ustar_name* split) {
    size_t path_len;
    size_t total_len;
    size_t search_limit;

    memset(split, 0, sizeof(*split));

    path_len = strlen(path);
    total_len = path_len + (directory ? 1u : 0u);
    if (total_len <= sizeof(split->name)) {
        memcpy(split->name, path, path_len);
        if (directory) {
            split->name[path_len] = '/';
        }
        split->name_len = total_len;
        return true;
    }

    search_limit = total_len;
    while (search_limit > 0u) {
        size_t slash_index = search_limit - 1u;
        size_t prefix_len;
        size_t name_len;
        size_t name_offset;
        size_t copied = 0u;

        if ((!directory || slash_index != path_len) && path[slash_index] != '/') {
            search_limit = slash_index;
            continue;
        }

        prefix_len = slash_index;
        name_len = total_len - prefix_len - 1u;
        if (prefix_len <= sizeof(split->prefix) && name_len <= sizeof(split->name)) {
            memcpy(split->prefix, path, prefix_len);
            split->prefix_len = prefix_len;

            name_offset = prefix_len + 1u;
            if (name_offset < path_len) {
                copied = path_len - name_offset;
                if (copied > name_len) {
                    copied = name_len;
                }
                memcpy(split->name, path + name_offset, copied);
            }
            if (directory && copied < name_len) {
                split->name[copied] = '/';
            }
            split->name_len = name_len;
            return true;
        }

        search_limit = slash_index;
    }

    return false;
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
    return bx_tar_pax_append(buffer, key, value, strlen(value));
}

static bool bx_tar_stream_pax_append_number_record(struct bx_archive_buffer* buffer,
                                                 const char* key,
                                                 uintmax_t value) {
    char text[32];

    snprintf(text, sizeof(text), "%ju", value);
    return bx_tar_stream_pax_append_record(buffer, key, text);
}

static bool bx_tar_stream_pax_append_mtime(struct bx_archive_buffer* buffer, struct timespec time) {
    char text[64];
    intmax_t seconds = (intmax_t)time.tv_sec;
    if (!time.tv_nsec) {
        snprintf(text, sizeof(text), "%jd", seconds);
    } else if (seconds < 0) {
        /* timespec is floor-normalized; PAX is a signed decimal number. */
        snprintf(text, sizeof(text), "-%ju.%09ld",
                 (uintmax_t)(-(seconds + 1)), 1000000000L - time.tv_nsec);
    } else {
        snprintf(text, sizeof(text), "%jd.%09ld", seconds, time.tv_nsec);
    }
    return bx_tar_stream_pax_append_record(buffer, "mtime", text);
}

static bool bx_tar_stream_append_prepared_raw_header(const struct bx_tar_stream_sink* sink,
                                                     const struct bx_tar_stream_ustar_name* path_name,
                                                     const char* linkname,
                                                     const char* uname,
                                                     const char* gname,
                                                     char typeflag,
                                                     mode_t mode,
                                                     uid_t uid,
                                                     gid_t gid,
                                                     dev_t rdev,
                                                     size_t size,
                                                     struct timespec mtime,
                                                     struct bx_diag_ctx* diag) {
    unsigned char header[BX_TAR_STREAM_BLOCK_SIZE];
    unsigned int checksum = 8u * (unsigned int)' ';

    memset(header, 0, sizeof(header));
    checksum += bx_tar_stream_copy_bytes(header, path_name->name, path_name->name_len);
    checksum += bx_tar_stream_format_octal_field(header + 100, 8u, mode & 07777u);
    checksum += bx_tar_stream_format_octal_field(header + 108, 8u, uid);
    checksum += bx_tar_stream_format_octal_field(header + 116, 8u, gid);
    checksum += bx_tar_stream_format_octal_field(header + 124, 12u, size);
    checksum += bx_tar_stream_format_octal_field(header + 136, 12u, (size_t)mtime.tv_sec);
    header[156] = (unsigned char)typeflag;
    checksum += (unsigned char)typeflag;
    if (linkname != NULL) {
        size_t link_len = strlen(linkname);
        checksum += bx_tar_stream_copy_text(header + 157, 100u, linkname, link_len);
    }
    checksum += bx_tar_stream_copy_text(header + 257, 5u, "ustar", 5u);
    checksum += bx_tar_stream_copy_text(header + 263, 2u, "00", 2u);
    if (uname != NULL) {
        size_t owner_len = strlen(uname);
        checksum += bx_tar_stream_copy_text(header + 265, 32u, uname, owner_len);
    }
    if (gname != NULL) {
        size_t group_len = strlen(gname);
        checksum += bx_tar_stream_copy_text(header + 297, 32u, gname, group_len);
    }
    checksum += bx_tar_stream_copy_bytes(header + 345, path_name->prefix, path_name->prefix_len);
    if (typeflag == '3' || typeflag == '4') {
        checksum += bx_tar_stream_format_octal_field(header + 329, 8u, major(rdev));
        checksum += bx_tar_stream_format_octal_field(header + 337, 8u, minor(rdev));
    }
    memset(header + 148, ' ', 8u);
    bx_tar_stream_write_checksum_field(header + 148, checksum);
    return bx_tar_stream_sink_write(sink, header, sizeof(header), diag);
}

static bool bx_tar_stream_append_raw_header(const struct bx_tar_stream_sink* sink,
                                            const char* path,
                                            const char* linkname,
                                            const char* uname,
                                            const char* gname,
                                            char typeflag,
                                            mode_t mode,
                                            uid_t uid,
                                            gid_t gid,
                                            dev_t rdev,
                                            size_t size,
                                            struct timespec mtime,
                                            bool directory,
                                            struct bx_diag_ctx* diag) {
    struct bx_tar_stream_ustar_name path_name;

    if (!bx_tar_stream_split_ustar_name(path, directory, &path_name)) {
        bx_diag(diag, "%s: file name too long", path);
        return false;
    }

    return bx_tar_stream_append_prepared_raw_header(sink,
                                                    &path_name,
                                                    linkname,
                                                    uname,
                                                    gname,
                                                    typeflag,
                                                    mode,
                                                    uid,
                                                    gid,
                                                    rdev,
                                                    size,
                                                    mtime,
                                                    diag);
}

static bool bx_tar_stream_write_header(const struct bx_tar_stream_sink* sink,
                                       const char* path,
                                       const char* linkname,
                                       const char* uname,
                                       const char* gname,
                                       char typeflag,
                                       bool is_dir,
                                       mode_t mode,
                                       uid_t uid,
                                       gid_t gid,
                                       dev_t rdev,
                                       size_t size,
                                       struct timespec mtime,
                                       bool allow_pax,
                                       const struct bx_file_metadata* metadata,
                                       struct bx_diag_ctx* diag) {
    bool need_path_pax;
    bool need_link_pax = false;
    bool need_uid_pax = (uintmax_t)uid > BX_TAR_USTAR_ID_MAX;
    bool need_gid_pax = (uintmax_t)gid > BX_TAR_USTAR_ID_MAX;
    bool need_size_pax = (uintmax_t)size > BX_TAR_USTAR_SIZE_TIME_MAX;
    bool need_mtime_pax = mtime.tv_sec < 0 || mtime.tv_nsec != 0
        || (uintmax_t)mtime.tv_sec > BX_TAR_USTAR_SIZE_TIME_MAX;
    bool need_numeric_pax = need_uid_pax || need_gid_pax || need_size_pax || need_mtime_pax;
    const char* stored_link = linkname;
    const char* actual_header_path = path;
    struct bx_tar_stream_ustar_name split_path;

    if (mtime.tv_nsec < 0 || mtime.tv_nsec >= 1000000000L
        || size > SIZE_MAX - (BX_TAR_STREAM_BLOCK_SIZE - 1u)
        || uid == (uid_t)-1 || gid == (gid_t)-1) {
        bx_diag(diag, "%s: invalid numeric metadata", path);
        return false;
    }
    if (typeflag == '3' || typeflag == '4') {
        dev_t checked;
        if (size != 0u || !bx_fd_device_from_numbers(major(rdev), minor(rdev), &checked) || checked != rdev) {
            bx_diag(diag, "%s: invalid device metadata", path);
            return false;
        }
    }
    need_path_pax = !bx_tar_stream_split_ustar_name(path, is_dir, &split_path);
    if (linkname != NULL && strlen(linkname) > 100u) {
        need_link_pax = true;
    }

    if ((need_path_pax || need_link_pax) && !allow_pax) {
        bx_diag(diag, "%s: file name too long", path);
        return false;
    }

    if ((need_numeric_pax || bx_tar_metadata_present(metadata)) && !allow_pax) {
        bx_diag(diag, "%s: metadata requires pax format", path);
        return false;
    }
    if (need_path_pax || need_link_pax || need_numeric_pax || bx_tar_metadata_present(metadata)) {
        struct timespec zero_time = {0, 0};
        struct bx_archive_buffer pax_data = {0};
        size_t pax_size;

        bx_archive_buffer_init(&pax_data);
        if (!bx_tar_metadata_write(&pax_data, metadata)
            || (need_path_pax && !bx_tar_stream_pax_append_record(&pax_data, "path", path))
            || (need_link_pax && !bx_tar_stream_pax_append_record(&pax_data, "linkpath", linkname))
            || (need_uid_pax && !bx_tar_stream_pax_append_number_record(&pax_data, "uid", uid))
            || (need_gid_pax && !bx_tar_stream_pax_append_number_record(&pax_data, "gid", gid))
            || (need_size_pax && !bx_tar_stream_pax_append_number_record(&pax_data, "size", size))
            || (need_mtime_pax && !bx_tar_stream_pax_append_mtime(&pax_data, mtime))) {
            bx_archive_buffer_free(&pax_data);
            bx_diag(diag, "archive write failed: %s", strerror(errno));
            return false;
        }
        if (!bx_tar_stream_append_raw_header(sink,
                                             "./PaxHeaders/bx",
                                             NULL,
                                             NULL,
                                             NULL,
                                             'x',
                                             0644u,
                                             0u,
                                             0u,
                                             0,
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
            if (!bx_tar_stream_sink_write(sink,
                                          bx_tar_stream_zero_block,
                                          pax_size - pax_data.len,
                                          diag)) {
                bx_archive_buffer_free(&pax_data);
                return false;
            }
        }
        bx_archive_buffer_free(&pax_data);

        actual_header_path = need_path_pax ? "PaxPayload" : path;
        if (need_link_pax) {
            stored_link = "";
        }
        if (need_uid_pax)
            uid = 0;
        if (need_gid_pax)
            gid = 0;
        if (need_size_pax)
            size = 0;
        if (need_mtime_pax)
            mtime = zero_time;
    }

    if (!need_path_pax) {
        return bx_tar_stream_append_prepared_raw_header(sink,
                                                        &split_path,
                                                        stored_link,
                                                        uname,
                                                        gname,
                                                        typeflag,
                                                        mode,
                                                        uid,
                                                        gid,
                                                        rdev,
                                                        (typeflag == '0' || typeflag == 'D')
                                                            ? size
                                                            : 0u,
                                                        mtime,
                                                        diag);
    }

    return bx_tar_stream_append_raw_header(sink,
                                           actual_header_path,
                                           stored_link,
                                           uname,
                                           gname,
                                           typeflag,
                                           mode,
                                           uid,
                                           gid,
                                           rdev,
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

    if (!bx_tar_stream_sink_write(sink, data, len, diag)) {
        return false;
    }
    if (padded > len) {
        return bx_tar_stream_sink_write(sink, bx_tar_stream_zero_block, padded - len, diag);
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
                                          int fd,
                                          const char* path,
                                          size_t expected_size,
                                          unsigned char* buffer,
                                          size_t buffer_size,
                                          struct bx_diag_ctx* diag) {
    size_t padding = bx_tar_stream_round_up(expected_size, BX_TAR_STREAM_BLOCK_SIZE) - expected_size;
    size_t total = 0u;
    bool padding_written = padding == 0u;

    while (total < expected_size) {
        size_t chunk = expected_size - total;
        size_t filled = 0u;
        bool final_chunk;
        size_t write_len;

        if (chunk > buffer_size) {
            chunk = buffer_size;
        }
        final_chunk = total + chunk == expected_size;
        while (filled < chunk) {
            ssize_t nread = bx_xread(fd, buffer + filled, chunk - filled);

            if (nread < 0) {
                bx_diag(diag, "%s: %s", path, strerror(errno));
                return false;
            }
            if (nread == 0) {
                bx_diag(diag, "%s: file shrank while reading", path);
                return false;
            }
            filled += (size_t)nread;
        }
        write_len = chunk;
        if (final_chunk && padding > 0u && buffer_size - chunk >= padding) {
            memset(buffer + chunk, 0, padding);
            write_len += padding;
            padding_written = true;
        }
        if (!bx_tar_stream_sink_write(sink, buffer, write_len, diag)) {
            return false;
        }
        total += chunk;
    }

    if (!padding_written
        && !bx_tar_stream_sink_write(sink, bx_tar_stream_zero_block, padding, diag)) {
        return false;
    }
    return true;
}

static const char* bx_tar_stream_find_seen_hardlink(const struct bx_tar_hardlink_seen_list* seen,
                                                dev_t dev,
                                                ino_t ino) {
    size_t i;
    for (i = 0u; i < seen->len; i++) {
        if (seen->items[i].dev == dev && seen->items[i].ino == ino) {
            return seen->items[i].first_name;
        }
    }
    return NULL;
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

static bool bx_tar_stream_apply_mode_text(mode_t initial_mode,
                                          bool is_directory,
                                          const char* mode_text,
                                          mode_t* mode_out) {
    struct bx_mode_parse_params params = {
        .initial_mode = initial_mode & 07777u,
        .result_mask = 07777u,
        .max_numeric_mode = 07777u,
        .umask_value = bx_mode_current_umask(),
        .sticky_bit = BX_TAR_STICKY_BIT,
        .x_policy = BX_MODE_X_IF_DIRECTORY_OR_ANY_EXEC,
        .is_directory = is_directory,
        .apply_umask_when_who_omitted = true,
        .allow_setuid = true,
        .allow_setgid = true,
        .allow_sticky = true,
    };

    return bx_mode_parse(mode_text, &params, mode_out);
}

static bool bx_tar_stream_write_fs_raw_entry(
    const struct bx_tar_stream_fs_write_state* state,
    const struct bx_archive_fs_visit_entry* fs_entry,
    int source_fd,
    enum bx_tar_stream_kind kind,
    const char* linkname,
    const unsigned char* data,
    size_t data_len,
    mode_t mode,
    uid_t uid,
    gid_t gid,
    const char* uname,
    const char* gname,
    struct timespec mtime,
    struct bx_diag_ctx* diag) {
    struct bx_file_metadata metadata = {0};
    if (!bx_tar_metadata_collect(&metadata, source_fd,
                                  S_ISLNK(fs_entry->st->st_mode),
                                  S_ISDIR(fs_entry->st->st_mode),
                                  state->options->numeric_owner,
                                  state->options->metadata)) {
        bx_diag(diag, "%s: cannot read metadata: %s", fs_entry->source_path, strerror(errno));
        bx_file_metadata_free(&metadata);
        return false;
    }
    bool ok = bx_tar_stream_write_raw_entry(state->sink,
                                                   fs_entry->archive_path,
                                                   linkname,
                                                   uname,
                                                   gname,
                                                   kind,
                                                   mode,
                                                   uid,
                                                   gid,
                                                   fs_entry->st->st_rdev,
                                                   data,
                                                   data_len,
                                                   mtime,
                                                   !state->options->format_ustar,
                                                   &metadata,
                                                   diag);
    bx_file_metadata_free(&metadata);
    return ok;
}

static bool bx_tar_stream_write_sparse_fs_entry(struct bx_tar_stream_fs_write_state* state,
                                                const struct bx_archive_fs_visit_entry* source,
                                                int fd,
                                                mode_t mode,
                                                uid_t uid,
                                                gid_t gid,
                                                const char* uname,
                                                const char* gname,
                                                struct timespec mtime,
                                                struct bx_diag_ctx* diag) {
    struct bx_tar_sparse_extent* extents = NULL;
    size_t count = 0, cap = 0, compact_size = 0;
    off_t offset = 0;
    bool ok = false;
    struct bx_file_metadata metadata = {0};
    struct bx_tar_stream_live_entry entry = {0};
    if (source->st->st_size < 0 || (uintmax_t)source->st->st_size > SIZE_MAX) {
        errno = EOVERFLOW;
        goto fail;
    }
    do {
        if (bx_archive_temp_pending_signal()) {
            errno = EINTR;
            goto fail;
        }
        off_t begin, end;
        int result = bx_fd_next_data_extent(fd, offset, source->st->st_size, &begin, &end);
        if (result < 0)
            goto fail;
        if (result == 0)
            break;
        if (count == 65536u) {
            errno = E2BIG;
            goto fail;
        }
        if (count == cap) {
            cap = cap ? cap * 2u : 16u;
            extents = xrealloc(extents, cap * sizeof(*extents));
        }
        extents[count++] = (struct bx_tar_sparse_extent){.offset = (size_t)begin, .size = (size_t)(end - begin)};
        compact_size += (size_t)(end - begin);
        offset = end;
    } while (offset < source->st->st_size);
    if (!bx_tar_metadata_collect(&metadata, fd, false, false, state->options->numeric_owner, state->options->metadata))
        goto fail;
    if (!bx_tar_stream_start_sparse_v1_entry(&entry, state->sink, source->archive_path, uname, gname, mode, uid, gid, extents, count, (size_t)source->st->st_size, compact_size, mtime, &metadata,
                                             diag))
        goto out;
    for (size_t i = 0; i < count; i++) {
        size_t remaining = extents[i].size;
        if (lseek(fd, (off_t)extents[i].offset, SEEK_SET) < 0)
            goto fail;
        while (remaining) {
            if (bx_archive_temp_pending_signal()) {
                errno = EINTR;
                goto fail;
            }
            size_t chunk = remaining < state->file_buffer_size ? remaining : state->file_buffer_size;
            ssize_t got = bx_xread(fd, state->file_buffer, chunk);
            if (got <= 0) {
                if (got == 0)
                    errno = ESTALE;
                goto fail;
            }
            if (!bx_tar_stream_write_raw_entry_chunk(&entry, state->file_buffer, (size_t)got, diag))
                goto out;
            remaining -= (size_t)got;
        }
    }
    ok = bx_tar_stream_finish_raw_entry(&entry, diag);
    goto out;
fail:
    bx_diag(diag, "%s: cannot archive sparse file: %s", source->source_path, strerror(errno));
out:
    bx_file_metadata_free(&metadata);
    free(extents);
    return ok;
}

static bool bx_tar_stream_write_opened_fs_entry(struct bx_tar_stream_fs_write_state* state,
                                         const struct bx_archive_fs_visit_entry* fs_entry,
                                         int source_fd,
                                         struct bx_diag_ctx* diag) {
    const struct bx_tar_stream_sink* sink = state->sink;
    const struct bx_tar_stream_options* options = state->options;
    mode_t mode = fs_entry->st->st_mode & 07777u;
    uid_t uid = options->owner_set ? options->owner : fs_entry->st->st_uid;
    gid_t gid = options->group_set ? options->group : fs_entry->st->st_gid;
    const char* uname = NULL;
    const char* gname = NULL;
    struct timespec mtime = options->fixed_mtime ? options->mtime : fs_entry->st->st_mtim;
    size_t file_size = (size_t)fs_entry->st->st_size;

    if (!options->numeric_owner) {
        if (uname == NULL) {
            uname = bx_tar_stream_user_name(&state->name_caches, uid);
        }
        if (gname == NULL) {
            gname = bx_tar_stream_group_name(&state->name_caches, gid);
        }
    }
    if (options->mode_text != NULL
        && !bx_tar_stream_apply_mode_text(mode,
                                          S_ISDIR(fs_entry->st->st_mode),
                                          options->mode_text,
                                          &mode)) {
        bx_diag(diag, "invalid mode '%s'", options->mode_text);
        return false;
    }

    if (S_ISDIR(fs_entry->st->st_mode)) {
        enum bx_tar_stream_kind kind = BX_TAR_STREAM_KIND_DIR;

        return bx_tar_stream_write_fs_raw_entry(state,
                                                fs_entry,
                                                source_fd,
                                                kind,
                                                NULL,
                                                NULL,
                                                0u,
                                                mode,
                                                uid,
                                                gid,
                                                uname,
                                                gname,
                                                mtime,
                                                diag);
    }
    if (S_ISLNK(fs_entry->st->st_mode)) {
        return bx_tar_stream_write_fs_raw_entry(state,
                                                fs_entry,
                                                source_fd,
                                                BX_TAR_STREAM_KIND_SYMLINK,
                                                fs_entry->link_target,
                                                NULL,
                                                0u,
                                                mode,
                                                uid,
                                                gid,
                                                uname,
                                                gname,
                                                mtime,
                                                diag);
    }
    if ((S_ISREG(fs_entry->st->st_mode) || S_ISCHR(fs_entry->st->st_mode) || S_ISBLK(fs_entry->st->st_mode)) && fs_entry->st->st_nlink > 1) {
        const char* first_name = bx_tar_stream_find_seen_hardlink(&state->seen, fs_entry->st->st_dev, fs_entry->st->st_ino);
        if (first_name != NULL) {
            return bx_tar_stream_write_fs_raw_entry(state,
                                                    fs_entry,
                                                    source_fd,
                                                    BX_TAR_STREAM_KIND_HARDLINK,
                                                    first_name,
                                                    NULL,
                                                    0u,
                                                    mode,
                                                    uid,
                                                    gid,
                                                    uname,
                                                    gname,
                                                    mtime,
                                                    diag);
        }
        bx_tar_stream_record_seen_hardlink(&state->seen,
                                           fs_entry->st->st_dev,
                                           fs_entry->st->st_ino,
                                           fs_entry->archive_path);
    }
    if (S_ISFIFO(fs_entry->st->st_mode) || S_ISCHR(fs_entry->st->st_mode) || S_ISBLK(fs_entry->st->st_mode)) {
        return bx_tar_stream_write_fs_raw_entry(state,
                                                fs_entry,
                                                source_fd,
                                                S_ISCHR(fs_entry->st->st_mode) ? BX_TAR_STREAM_KIND_CHAR
                                                    : S_ISBLK(fs_entry->st->st_mode) ? BX_TAR_STREAM_KIND_BLOCK : BX_TAR_STREAM_KIND_FIFO,
                                                NULL,
                                                NULL,
                                                0u,
                                                mode,
                                                uid,
                                                gid,
                                                uname,
                                                gname,
                                                mtime,
                                                diag);
    }
    if (!S_ISREG(fs_entry->st->st_mode)) {
        bx_diag(diag, "%s: unsupported file type", fs_entry->source_path);
        return false;
    }

    if (options->sparse)
        return bx_tar_stream_write_sparse_fs_entry(state, fs_entry, source_fd, mode, uid, gid, uname, gname, mtime, diag);

    if (!bx_tar_stream_write_fs_raw_entry(state,
                                          fs_entry,
                                          source_fd,
                                          BX_TAR_STREAM_KIND_REG,
                                          NULL,
                                          NULL,
                                          file_size,
                                          mode,
                                          uid,
                                          gid,
                                          uname,
                                          gname,
                                          mtime,
                                          diag)) {
        return false;
    }
    if (!bx_tar_stream_write_file_data(sink,
                                       source_fd,
                                       fs_entry->source_path,
                                       file_size,
                                       state->file_buffer,
                                       state->file_buffer_size,
                                       diag)) {
        return false;
    }
    return true;
}

static bool bx_tar_stream_write_fs_entry(struct bx_tar_stream_fs_write_state* state,
                                         const struct bx_archive_fs_visit_entry* entry,
                                         struct bx_diag_ctx* diag) {
    const struct bx_tar_metadata_options* metadata = state->options->metadata;
    bool symlink = S_ISLNK(entry->st->st_mode);
    bool needs_metadata = metadata && (metadata->xattrs || metadata->selinux || metadata->file_flags || (metadata->acls && !symlink));
    struct stat opened;
    bool borrowed = symlink && entry->source_fd >= 0;
    int fd;
    if (S_ISREG(entry->st->st_mode))
        fd = bx_fd_openat_regular_verified(entry->source_parent_fd, entry->source_name,
                                            entry->st, &opened);
    else {
        int flags = S_ISDIR(entry->st->st_mode) && needs_metadata
            ? O_RDONLY | O_DIRECTORY : O_PATH;
        fd = borrowed ? entry->source_fd
            : bx_fd_openat_cloexec(entry->source_parent_fd, entry->source_name, flags | O_NOFOLLOW, 0);
        if (fd >= 0 && bx_fd_fstat_expected(fd, entry->st, &opened) != 0) {
            int error = errno;
            if (!borrowed)
                close(fd);
            fd = -1;
            errno = error;
        }
    }
    if (fd < 0) {
        bx_diag(diag, "%s: %s", entry->source_path, strerror(errno));
        return false;
    }
    /* Metadata and payload refer to this opened inode. A streaming symlink
     * borrows the walker's FD; buffered inputs must acquire their own. */
    struct bx_archive_fs_visit_entry verified = *entry;
    verified.st = &opened;
    char* target = symlink && !borrowed ? bx_path_readlinkat_dup(fd, "") : NULL;
    bool ok = !symlink || borrowed || target;
    if (!ok)
        bx_diag(diag, "%s: %s", entry->source_path, strerror(errno));
    if (target)
        verified.link_target = target;
    if (ok)
        ok = bx_tar_stream_write_opened_fs_entry(state, &verified, fd, diag);
    if (ok) {
        struct stat after;
        if (fstat(fd, &after) != 0) {
            bx_diag(diag, "%s: %s", entry->source_path, strerror(errno));
            ok = false;
        }
        /* Reading may change atime. Identity pinning does not freeze contents,
         * link counts, or metadata; compare the state used for this entry. */
        else if (after.st_dev != opened.st_dev || after.st_ino != opened.st_ino
                 || after.st_mode != opened.st_mode || after.st_uid != opened.st_uid
                 || after.st_gid != opened.st_gid || after.st_nlink != opened.st_nlink
                 || after.st_rdev != opened.st_rdev || after.st_size != opened.st_size
                 || after.st_mtim.tv_sec != opened.st_mtim.tv_sec
                 || after.st_mtim.tv_nsec != opened.st_mtim.tv_nsec
                 || after.st_ctim.tv_sec != opened.st_ctim.tv_sec
                 || after.st_ctim.tv_nsec != opened.st_ctim.tv_nsec) {
            errno = ESTALE;
            bx_diag(diag, "%s: file changed while reading", entry->source_path);
            ok = false;
        }
    }
    free(target);
    bool closed = borrowed || bx_fd_close(&fd, entry->source_path, ok ? diag : NULL);
    return ok && closed;
}

static bool bx_tar_stream_visit_fs_entry(const struct bx_archive_fs_visit_entry* fs_entry,
                                         void* user,
                                         struct bx_diag_ctx* diag) {
    struct bx_tar_stream_fs_write_state* state = user;

    return bx_tar_stream_write_fs_entry(state, fs_entry, diag);
}

static bool bx_tar_stream_finish_archive(const struct bx_tar_stream_sink* sink,
                                         size_t bytes_written,
                                         struct bx_diag_ctx* diag) {
    size_t with_trailer = bytes_written + 2u * BX_TAR_STREAM_BLOCK_SIZE;
    size_t padded = bx_tar_stream_round_up(with_trailer,
                                           BX_TAR_STREAM_BLOCK_SIZE * BX_TAR_STREAM_RECORD_BLOCKS);
    size_t zeros_needed = padded - bytes_written;

    while (zeros_needed > 0u) {
        size_t chunk = zeros_needed > sizeof(bx_tar_stream_zero_record)
            ? sizeof(bx_tar_stream_zero_record)
            : zeros_needed;

        if (!bx_tar_stream_sink_write(sink, bx_tar_stream_zero_record, chunk, diag)) {
            return false;
        }
        zeros_needed -= chunk;
    }
    return true;
}

bool bx_tar_stream_write_raw_entry(const struct bx_tar_stream_sink* sink,
                                   const char* path,
                                   const char* linkname,
                                   const char* uname,
                                   const char* gname,
                                   enum bx_tar_stream_kind kind,
                                   mode_t mode,
                                   uid_t uid,
                                   gid_t gid,
                                   dev_t rdev,
                                   const unsigned char* data,
                                   size_t data_len,
                                   struct timespec mtime,
                                   bool allow_pax,
                                   const struct bx_file_metadata* metadata,
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
        case BX_TAR_STREAM_KIND_CHAR:
        case BX_TAR_STREAM_KIND_BLOCK:
            typeflag = kind == BX_TAR_STREAM_KIND_CHAR ? '3' : '4';
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
                                    uname,
                                    gname,
                                    typeflag,
                                    is_dir,
                                    mode,
                                    uid,
                                    gid,
                                    rdev,
                                    data_len,
                                    mtime,
                                    allow_pax,
                                    metadata,
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
                                   const char* uname,
                                   const char* gname,
                                   enum bx_tar_stream_kind kind,
                                   mode_t mode,
                                   uid_t uid,
                                   gid_t gid,
                                   dev_t rdev,
                                   size_t data_len,
                                   struct timespec mtime,
                                   bool allow_pax,
                                   const struct bx_file_metadata* metadata,
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
        case BX_TAR_STREAM_KIND_CHAR:
        case BX_TAR_STREAM_KIND_BLOCK:
            typeflag = kind == BX_TAR_STREAM_KIND_CHAR ? '3' : '4';
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
                                    uname,
                                    gname,
                                    typeflag,
                                    is_dir,
                                    mode,
                                    uid,
                                    gid,
                                    rdev,
                                    data_len,
                                    mtime,
                                    allow_pax,
                                    metadata,
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
                                         const char* uname,
                                         const char* gname,
                                         mode_t mode,
                                         uid_t uid,
                                         gid_t gid,
                                         const struct bx_tar_sparse_extent* extents,
                                         size_t extent_count,
                                         size_t logical_size,
                                         size_t compact_size,
                                         struct timespec mtime,
                                         const struct bx_file_metadata* metadata,
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
    if (!bx_tar_metadata_write(&pax_data, metadata)
        || !bx_tar_stream_pax_append_record(&pax_data, "path", path)
        || !bx_tar_stream_pax_append_record(&pax_data, "GNU.sparse.major", "1")
        || !bx_tar_stream_pax_append_record(&pax_data, "GNU.sparse.minor", "0")
        || !bx_tar_stream_pax_append_number_record(&pax_data, "GNU.sparse.realsize", logical_size)) {
        bx_archive_buffer_free(&pax_data);
        bx_diag(diag, "archive write failed: %s", strerror(errno));
        return false;
    }
    if (!bx_tar_stream_append_raw_header(sink,
                                         "./PaxHeaders/bx",
                                         NULL,
                                         NULL,
                                         NULL,
                                         'x',
                                         0644u,
                                         0u,
                                         0u,
                                         0,
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
            if (!bx_tar_stream_sink_write(sink,
                                          bx_tar_stream_zero_block,
                                          pax_size - pax_data.len,
                                          diag)) {
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
                                       uname,
                                       gname,
                                       BX_TAR_STREAM_KIND_REG,
                                       mode,
                                       uid,
                                       gid,
                                       0,
                                       payload_size,
                                       mtime,
                                       true,
                                       NULL,
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
        size_t remaining = padded_map_size - map_size;

        while (remaining > 0u) {
            size_t chunk = remaining > sizeof(bx_tar_stream_zero_block)
                ? sizeof(bx_tar_stream_zero_block)
                : remaining;

            if (!bx_tar_stream_write_raw_entry_chunk(entry,
                                                     bx_tar_stream_zero_block,
                                                     chunk,
                                                     diag)) {
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
    if (entry == NULL || !entry->active) {
        bx_diag(diag, "invalid tar stream live entry state");
        return false;
    }
    if (entry->data_remaining != 0u) {
        bx_diag(diag, "truncated tar entry payload");
        return false;
    }

    while (entry->padding_remaining > 0u) {
        size_t chunk = entry->padding_remaining > sizeof(bx_tar_stream_zero_block)
            ? sizeof(bx_tar_stream_zero_block)
            : entry->padding_remaining;

        if (!bx_tar_stream_sink_write(entry->sink, bx_tar_stream_zero_block, chunk, diag)) {
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
    struct bx_tar_stream_counting_sink counting_user = {
        .inner = sink,
        .bytes_written = NULL,
    };
    struct bx_tar_stream_sink counting_sink = {
        .user = &counting_user,
        .write = bx_tar_stream_counting_sink_write,
    };
    struct bx_tar_stream_fs_write_state state = {
        .sink = &counting_sink,
        .options = options,
        .file_buffer = xmalloc(BX_TAR_STREAM_FILE_BUFFER_SIZE),
        .file_buffer_size = BX_TAR_STREAM_FILE_BUFFER_SIZE,
    };
    size_t i;

    if (bytes_written_io == NULL) {
        bx_diag(diag, "invalid tar stream byte counter");
        return false;
    }
    counting_user.bytes_written = bytes_written_io;

    for (i = 0u; i < files->len; i++) {
        const struct bx_archive_fs_entry* saved = &files->entries[i];
        char* name = NULL;
        /* Tar keeps cwd/root unchanged during creation. Resolve every parent
         * component no-follow, then verify the selected parent. Keep only this
         * entry's descriptors, not one FD per buffered input. */
        int parent = bx_dir_path_open_source_parent(saved->source_path, &name);
        struct stat expected = {
            .st_dev = saved->source_parent_dev,
            .st_ino = saved->source_parent_ino,
            .st_mode = S_IFDIR,
        };
        struct stat opened;
        bool ok = parent >= 0 && bx_fd_fstat_expected(parent, &expected, &opened) == 0;
        if (!ok)
            bx_diag(diag, "%s: cannot open source parent: %s", saved->source_path, strerror(errno));
        struct bx_archive_fs_visit_entry entry = {
            .source_parent_fd = parent,
            .source_fd = -1,
            .source_name = name,
            .source_path = saved->source_path,
            .archive_path = saved->archive_path,
            .st = &saved->st,
            .link_target = saved->link_target,
        };
        if (ok)
            ok = bx_tar_stream_write_fs_entry(&state, &entry, diag);
        bool closed = bx_fd_close(&parent, saved->source_path, ok ? diag : NULL);
        free(name);
        if (!ok || !closed) {
            free(state.file_buffer);
            bx_tar_stream_id_name_cache_cleanup(&state.name_caches.groups);
            bx_tar_stream_id_name_cache_cleanup(&state.name_caches.users);
            bx_tar_stream_seen_list_free(&state.seen);
            return false;
        }
    }

    free(state.file_buffer);
    bx_tar_stream_id_name_cache_cleanup(&state.name_caches.groups);
    bx_tar_stream_id_name_cache_cleanup(&state.name_caches.users);
    bx_tar_stream_seen_list_free(&state.seen);
    return true;
}

bool bx_tar_stream_write_fs_entries_body(bx_tar_stream_fs_entry_producer_fn producer,
                                         void* producer_user,
                                         const struct bx_tar_stream_options* options,
                                         const struct bx_tar_stream_sink* sink,
                                         size_t* bytes_written_io,
                                         struct bx_diag_ctx* diag) {
    struct bx_tar_stream_counting_sink counting_user = {
        .inner = sink,
        .bytes_written = NULL,
    };
    struct bx_tar_stream_sink counting_sink = {
        .user = &counting_user,
        .write = bx_tar_stream_counting_sink_write,
    };
    struct bx_tar_stream_fs_write_state state = {
        .sink = &counting_sink,
        .options = options,
        .file_buffer = xmalloc(BX_TAR_STREAM_FILE_BUFFER_SIZE),
        .file_buffer_size = BX_TAR_STREAM_FILE_BUFFER_SIZE,
    };

    if (producer == NULL) {
        bx_diag(diag, "invalid tar stream fs entry producer");
        return false;
    }
    if (bytes_written_io == NULL) {
        bx_diag(diag, "invalid tar stream byte counter");
        return false;
    }

    counting_user.bytes_written = bytes_written_io;
    if (!producer(producer_user, bx_tar_stream_visit_fs_entry, &state, diag)) {
        free(state.file_buffer);
        bx_tar_stream_id_name_cache_cleanup(&state.name_caches.groups);
        bx_tar_stream_id_name_cache_cleanup(&state.name_caches.users);
        bx_tar_stream_seen_list_free(&state.seen);
        return false;
    }

    free(state.file_buffer);
    bx_tar_stream_id_name_cache_cleanup(&state.name_caches.groups);
    bx_tar_stream_id_name_cache_cleanup(&state.name_caches.users);
    bx_tar_stream_seen_list_free(&state.seen);
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
