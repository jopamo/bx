#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "applets/archive/archive_common.h"
#include "applets/archive/archive_fs.h"
#include "applets/archive/tar/tar_backend.h"
#include "bx/libbx.h"
#include "lib/cli_common.h"
#include "lib/copy_data.h"
#include "lib/path_ops.h"
#include "lib/xreadwrite.h"

#define BX_TAR_BLOCK_SIZE 512u
#define BX_TAR_RECORD_BLOCKS 20u

enum bx_tar_mode {
    BX_TAR_MODE_NONE = 0,
    BX_TAR_MODE_CREATE,
    BX_TAR_MODE_LIST,
    BX_TAR_MODE_EXTRACT,
    BX_TAR_MODE_APPEND,
    BX_TAR_MODE_DELETE,
};

enum bx_tar_kind {
    BX_TAR_KIND_REG = 0,
    BX_TAR_KIND_DIR,
    BX_TAR_KIND_SYMLINK,
    BX_TAR_KIND_HARDLINK,
    BX_TAR_KIND_FIFO,
};

struct bx_tar_sparse_extent {
    size_t offset;
    size_t size;
};

struct bx_tar_entry {
    char* name;
    char* linkname;
    enum bx_tar_kind kind;
    mode_t mode;
    uid_t uid;
    gid_t gid;
    struct timespec mtime;
    unsigned char* data;
    size_t data_len;
    size_t size;
    bool sparse;
    struct bx_tar_sparse_extent* extents;
    size_t extent_count;
};

struct bx_tar_entry_list {
    struct bx_tar_entry* items;
    size_t len;
    size_t cap;
};

struct bx_tar_pax_info {
    char* path;
    char* linkpath;
    int sparse_major;
    int sparse_minor;
    size_t sparse_realsize;
    bool sparse_enabled;
};

struct bx_tar_options {
    enum bx_tar_mode mode;
    const char* archive_path;
    const char* create_cwd;
    const char* extract_dir;
    bool to_stdout;
    bool keep_old_files;
    bool gzip;
    bool auto_compress;
    bool sort_name;
    bool format_ustar;
    bool owner_set;
    bool group_set;
    uid_t owner;
    gid_t group;
    bool fixed_mtime;
    struct timespec mtime;
    bool xattrs;
    bool acls;
    int operand_index;
};

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

static const char* bx_tar_progname(char** argv, int argc) {
    return bx_cli_progname((argc > 0) ? argv[0] : NULL, "tar");
}

static void bx_tar_entry_free(struct bx_tar_entry* entry) {
    free(entry->name);
    free(entry->linkname);
    free(entry->data);
    free(entry->extents);
    entry->name = NULL;
    entry->linkname = NULL;
    entry->data = NULL;
    entry->extents = NULL;
}

static void bx_tar_entry_list_free(struct bx_tar_entry_list* list) {
    size_t i;
    for (i = 0u; i < list->len; i++) {
        bx_tar_entry_free(&list->items[i]);
    }
    free(list->items);
    list->items = NULL;
    list->len = 0u;
    list->cap = 0u;
}

static bool bx_tar_entry_list_push(struct bx_tar_entry_list* list, const struct bx_tar_entry* entry) {
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
    free(pax->path);
    free(pax->linkpath);
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

static bool bx_tar_parse_octal_field(const unsigned char* field, size_t len, size_t* value_out) {
    size_t value = 0u;
    size_t i = 0u;
    while (i < len && (field[i] == ' ' || field[i] == '\0')) {
        i++;
    }
    for (; i < len; i++) {
        unsigned char ch = field[i];
        if (ch == '\0' || ch == ' ') {
            break;
        }
        if (ch < '0' || ch > '7') {
            return false;
        }
        value = (value << 3) + (size_t)(ch - '0');
    }
    *value_out = value;
    return true;
}

static void bx_tar_format_octal_field(unsigned char* field, size_t len, size_t value) {
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

static void bx_tar_write_checksum(unsigned char* header) {
    unsigned int sum = 0u;
    size_t i;
    memset(header + 148, ' ', 8u);
    for (i = 0u; i < BX_TAR_BLOCK_SIZE; i++) {
        sum += header[i];
    }
    snprintf((char*)header + 148, 8u, "%06o", sum);
    header[154] = '\0';
    header[155] = ' ';
}

static bool bx_tar_split_ustar_name(const char* path,
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

static size_t bx_tar_decimal_digits(size_t value) {
    size_t digits = 1u;
    while (value >= 10u) {
        value /= 10u;
        digits++;
    }
    return digits;
}

static bool bx_tar_pax_append_record(struct bx_archive_buffer* buffer, const char* key, const char* value) {
    size_t payload_len = strlen(key) + 1u + strlen(value) + 1u;
    size_t digits = bx_tar_decimal_digits(payload_len + 2u);
    size_t total;
    char prefix[32];

    while (true) {
        total = payload_len + digits + 1u;
        if (bx_tar_decimal_digits(total) == digits) {
            break;
        }
        digits = bx_tar_decimal_digits(total);
    }

    snprintf(prefix, sizeof(prefix), "%zu ", total);
    return bx_archive_buffer_append(buffer, prefix, strlen(prefix))
        && bx_archive_buffer_append(buffer, key, strlen(key))
        && bx_archive_buffer_append_byte(buffer, '=')
        && bx_archive_buffer_append(buffer, value, strlen(value))
        && bx_archive_buffer_append_byte(buffer, '\n');
}

static bool bx_tar_append_raw_header(struct bx_archive_buffer* archive,
                                     const char* path,
                                     const char* linkname,
                                     char typeflag,
                                     mode_t mode,
                                     uid_t uid,
                                     gid_t gid,
                                     size_t size,
                                     struct timespec mtime,
                                     bool directory) {
    unsigned char header[BX_TAR_BLOCK_SIZE];
    unsigned char name[100u];
    unsigned char prefix[155u];

    if (!bx_tar_split_ustar_name(path, directory, name, prefix)) {
        return false;
    }

    memset(header, 0, sizeof(header));
    memcpy(header, name, sizeof(name));
    bx_tar_format_octal_field(header + 100, 8u, mode & 07777u);
    bx_tar_format_octal_field(header + 108, 8u, uid);
    bx_tar_format_octal_field(header + 116, 8u, gid);
    bx_tar_format_octal_field(header + 124, 12u, size);
    bx_tar_format_octal_field(header + 136, 12u, (size_t)mtime.tv_sec);
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
    bx_tar_write_checksum(header);
    return bx_archive_buffer_append(archive, header, sizeof(header));
}

static bool bx_tar_write_header(struct bx_archive_buffer* archive,
                                const char* path,
                                const char* linkname,
                                enum bx_tar_kind kind,
                                mode_t mode,
                                uid_t uid,
                                gid_t gid,
                                size_t size,
                                struct timespec mtime,
                                bool allow_pax) {
    struct bx_archive_buffer pax = {0};
    bool is_dir = kind == BX_TAR_KIND_DIR;
    bool need_path_pax;
    bool need_link_pax = false;
    const char* stored_link = linkname;
    const char* actual_header_path = path;
    char typeflag;

    bx_archive_buffer_init(&pax);

    need_path_pax = !bx_tar_split_ustar_name(path, is_dir, (unsigned char[100]){0}, (unsigned char[155]){0});
    if (linkname != NULL && strlen(linkname) > 100u) {
        need_link_pax = true;
    }

    if ((need_path_pax || need_link_pax) && !allow_pax) {
        bx_archive_buffer_free(&pax);
        return false;
    }

    if (need_path_pax || need_link_pax) {
        struct timespec zero_time = {0, 0};
        struct bx_archive_buffer pax_data = {0};
        size_t pax_size;

        bx_archive_buffer_init(&pax_data);
        if (need_path_pax && !bx_tar_pax_append_record(&pax_data, "path", path)) {
            bx_archive_buffer_free(&pax_data);
            bx_archive_buffer_free(&pax);
            return false;
        }
        if (need_link_pax && !bx_tar_pax_append_record(&pax_data, "linkpath", linkname)) {
            bx_archive_buffer_free(&pax_data);
            bx_archive_buffer_free(&pax);
            return false;
        }
        if (!bx_tar_append_raw_header(&pax,
                                      "./PaxHeaders/bx",
                                      NULL,
                                      'x',
                                      0644u,
                                      0u,
                                      0u,
                                      pax_data.len,
                                      zero_time,
                                      false)) {
            bx_archive_buffer_free(&pax_data);
            bx_archive_buffer_free(&pax);
            return false;
        }
        pax_size = bx_tar_round_up(pax_data.len, BX_TAR_BLOCK_SIZE);
        bx_archive_buffer_append(&pax, pax_data.data, pax_data.len);
        bx_archive_buffer_append_zeros(&pax, pax_size - pax_data.len);
        bx_archive_buffer_free(&pax_data);

        actual_header_path = need_path_pax ? "PaxPayload" : path;
        if (need_link_pax) {
            stored_link = "";
        }
    }

    switch (kind) {
        case BX_TAR_KIND_REG: typeflag = '0'; break;
        case BX_TAR_KIND_DIR: typeflag = '5'; break;
        case BX_TAR_KIND_SYMLINK: typeflag = '2'; break;
        case BX_TAR_KIND_HARDLINK: typeflag = '1'; break;
        case BX_TAR_KIND_FIFO: typeflag = '6'; break;
        default: typeflag = '0'; break;
    }

    if (pax.len > 0u && !bx_archive_buffer_append(archive, pax.data, pax.len)) {
        bx_archive_buffer_free(&pax);
        return false;
    }
    bx_archive_buffer_free(&pax);
    return bx_tar_append_raw_header(archive,
                                    actual_header_path,
                                    stored_link,
                                    typeflag,
                                    mode,
                                    uid,
                                    gid,
                                    kind == BX_TAR_KIND_REG ? size : 0u,
                                    mtime,
                                    is_dir);
}

static bool bx_tar_write_entry_data(struct bx_archive_buffer* archive,
                                    const unsigned char* data,
                                    size_t len) {
    size_t padded = bx_tar_round_up(len, BX_TAR_BLOCK_SIZE);
    return bx_archive_buffer_append(archive, data, len)
        && bx_archive_buffer_append_zeros(archive, padded - len);
}

static bool bx_tar_read_file(const char* path, struct bx_archive_buffer* buffer, struct bx_diag_ctx* diag) {
    FILE* stream = fopen(path, "rb");
    if (stream == NULL) {
        bx_diag(diag, "%s: %s", path, strerror(errno));
        return false;
    }
    bx_archive_buffer_init(buffer);
    if (!bx_archive_buffer_read_all(stream, buffer, diag)) {
        fclose(stream);
        return false;
    }
    if (fclose(stream) != 0) {
        bx_diag(diag, "%s: %s", path, strerror(errno));
        return false;
    }
    return true;
}

static ssize_t bx_tar_find_seen_hardlink(const struct bx_tar_hardlink_seen_list* seen, dev_t dev, ino_t ino) {
    size_t i;
    for (i = 0u; i < seen->len; i++) {
        if (seen->items[i].dev == dev && seen->items[i].ino == ino) {
            return (ssize_t)i;
        }
    }
    return -1;
}

static bool bx_tar_record_seen_hardlink(struct bx_tar_hardlink_seen_list* seen,
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

static void bx_tar_seen_list_free(struct bx_tar_hardlink_seen_list* seen) {
    size_t i;
    for (i = 0u; i < seen->len; i++) {
        free(seen->items[i].first_name);
    }
    free(seen->items);
    seen->items = NULL;
    seen->len = 0u;
    seen->cap = 0u;
}

static bool bx_tar_write_fs_entry(struct bx_archive_buffer* archive,
                                  const struct bx_archive_fs_entry* fs_entry,
                                  const struct bx_tar_options* options,
                                  struct bx_tar_hardlink_seen_list* seen,
                                  struct bx_diag_ctx* diag) {
    enum bx_tar_kind kind;
    mode_t mode = fs_entry->st.st_mode & 07777u;
    uid_t uid = options->owner_set ? options->owner : fs_entry->st.st_uid;
    gid_t gid = options->group_set ? options->group : fs_entry->st.st_gid;
    struct timespec mtime = options->fixed_mtime ? options->mtime : fs_entry->st.st_mtim;
    struct bx_archive_buffer file_data;

    if (S_ISDIR(fs_entry->st.st_mode)) {
        kind = BX_TAR_KIND_DIR;
        return bx_tar_write_header(archive,
                                   fs_entry->archive_path,
                                   NULL,
                                   kind,
                                   mode,
                                   uid,
                                   gid,
                                   0u,
                                   mtime,
                                   !options->format_ustar);
    }
    if (S_ISLNK(fs_entry->st.st_mode)) {
        kind = BX_TAR_KIND_SYMLINK;
        return bx_tar_write_header(archive,
                                   fs_entry->archive_path,
                                   fs_entry->link_target,
                                   kind,
                                   mode,
                                   uid,
                                   gid,
                                   0u,
                                   mtime,
                                   !options->format_ustar);
    }
    if (S_ISFIFO(fs_entry->st.st_mode)) {
        kind = BX_TAR_KIND_FIFO;
        return bx_tar_write_header(archive,
                                   fs_entry->archive_path,
                                   NULL,
                                   kind,
                                   mode,
                                   uid,
                                   gid,
                                   0u,
                                   mtime,
                                   !options->format_ustar);
    }
    if (!S_ISREG(fs_entry->st.st_mode)) {
        bx_diag(diag, "%s: unsupported file type", fs_entry->source_path);
        return false;
    }

    if (fs_entry->st.st_nlink > 1) {
        ssize_t index = bx_tar_find_seen_hardlink(seen, fs_entry->st.st_dev, fs_entry->st.st_ino);
        if (index >= 0) {
            return bx_tar_write_header(archive,
                                       fs_entry->archive_path,
                                       seen->items[index].first_name,
                                       BX_TAR_KIND_HARDLINK,
                                       mode,
                                       uid,
                                       gid,
                                       0u,
                                       mtime,
                                       !options->format_ustar);
        }
        bx_tar_record_seen_hardlink(seen, fs_entry->st.st_dev, fs_entry->st.st_ino, fs_entry->archive_path);
    }

    bx_archive_buffer_init(&file_data);
    if (!bx_tar_read_file(fs_entry->source_path, &file_data, diag)) {
        return false;
    }
    if (!bx_tar_write_header(archive,
                             fs_entry->archive_path,
                             NULL,
                             BX_TAR_KIND_REG,
                             mode,
                             uid,
                             gid,
                             file_data.len,
                             mtime,
                             !options->format_ustar)
        || !bx_tar_write_entry_data(archive, file_data.data, file_data.len)) {
        bx_archive_buffer_free(&file_data);
        bx_diag(diag, "archive write failed: %s", strerror(errno));
        return false;
    }
    bx_archive_buffer_free(&file_data);
    return true;
}

static bool bx_tar_finish_archive(struct bx_archive_buffer* archive) {
    size_t with_trailer = archive->len + 2u * BX_TAR_BLOCK_SIZE;
    size_t padded = bx_tar_round_up(with_trailer, BX_TAR_BLOCK_SIZE * BX_TAR_RECORD_BLOCKS);
    return bx_archive_buffer_append_zeros(archive, padded - archive->len);
}

static bool bx_tar_build_create_archive(struct bx_archive_buffer* archive,
                                        const struct bx_tar_options* options,
                                        int argc,
                                        char** argv,
                                        struct bx_diag_ctx* diag) {
    struct bx_archive_fs_list files = {0};
    struct bx_tar_hardlink_seen_list seen = {0};
    int i;

    bx_archive_buffer_init(archive);

    for (i = options->operand_index; i < argc; i++) {
        const char* operand = argv[i];
        char* source = options->create_cwd ? bx_path_join(options->create_cwd, operand) : xstrdup(operand);
        bool ok = bx_archive_fs_add_path(&files, source, operand, true, options->sort_name, diag);
        free(source);
        if (!ok) {
            bx_archive_fs_list_free(&files);
            return false;
        }
    }

    for (i = 0; (size_t)i < files.len; i++) {
        if (!bx_tar_write_fs_entry(archive, &files.entries[i], options, &seen, diag)) {
            bx_archive_fs_list_free(&files);
            bx_tar_seen_list_free(&seen);
            bx_archive_buffer_free(archive);
            return false;
        }
    }

    bx_archive_fs_list_free(&files);
    bx_tar_seen_list_free(&seen);
    if (!bx_tar_finish_archive(archive)) {
        bx_archive_buffer_free(archive);
        bx_diag(diag, "archive write failed: %s", strerror(errno));
        return false;
    }
    return true;
}

static bool bx_tar_read_archive_input(const struct bx_tar_options* options,
                                      struct bx_archive_buffer* archive,
                                      struct bx_diag_ctx* diag) {
    FILE* stream;

    bx_archive_buffer_init(archive);
    if (options->archive_path == NULL || strcmp(options->archive_path, "-") == 0) {
        stream = stdin;
    }
    else {
        stream = fopen(options->archive_path, "rb");
        if (stream == NULL) {
            bx_diag(diag, "%s: %s", options->archive_path, strerror(errno));
            return false;
        }
    }

    if (!bx_archive_buffer_read_all(stream, archive, diag)) {
        if (stream != stdin) {
            fclose(stream);
        }
        bx_archive_buffer_free(archive);
        return false;
    }
    if (stream != stdin && fclose(stream) != 0) {
        bx_diag(diag, "%s: %s", options->archive_path, strerror(errno));
        bx_archive_buffer_free(archive);
        return false;
    }

    if (options->gzip
        || (options->auto_compress && bx_archive_path_has_gzip_suffix(options->archive_path))
        || ((options->mode == BX_TAR_MODE_LIST || options->mode == BX_TAR_MODE_EXTRACT)
            && bx_archive_buffer_has_gzip_magic(archive))) {
        struct bx_archive_buffer decompressed = {0};
        bx_archive_buffer_init(&decompressed);
        if (!bx_archive_run_gzip_filter(archive, &decompressed, true, diag)) {
            bx_archive_buffer_free(archive);
            return false;
        }
        bx_archive_buffer_free(archive);
        *archive = decompressed;
    }
    return true;
}

static bool bx_tar_write_archive_output(const struct bx_tar_options* options,
                                        const struct bx_archive_buffer* archive,
                                        struct bx_diag_ctx* diag) {
    struct bx_archive_buffer compressed = {0};
    const struct bx_archive_buffer* output = archive;
    FILE* stream;
    bool ok;

    if (options->gzip || (options->auto_compress && options->archive_path != NULL && bx_archive_path_has_gzip_suffix(options->archive_path))) {
        bx_archive_buffer_init(&compressed);
        if (!bx_archive_run_gzip_filter(archive, &compressed, false, diag)) {
            return false;
        }
        output = &compressed;
    }

    if (options->archive_path == NULL || strcmp(options->archive_path, "-") == 0) {
        stream = stdout;
    }
    else {
        stream = fopen(options->archive_path, "wb");
        if (stream == NULL) {
            bx_archive_buffer_free(&compressed);
            bx_diag(diag, "%s: %s", options->archive_path, strerror(errno));
            return false;
        }
    }

    ok = bx_archive_buffer_write_all(stream, output, diag);
    if (stream != stdout) {
        if (fclose(stream) != 0) {
            ok = false;
            bx_diag(diag, "%s: %s", options->archive_path, strerror(errno));
        }
    }
    bx_archive_buffer_free(&compressed);
    return ok;
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

static bool bx_tar_parse_pax_records(struct bx_tar_pax_info* pax, const unsigned char* data, size_t len) {
    size_t pos = 0u;
    while (pos < len) {
        size_t line_len = 0u;
        size_t digits_end = pos;
        size_t field_start;
        size_t field_len;
        const char* key;
        const char* value;
        char* record;
        char* equal;
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
        equal = strchr(record, '=');
        if (equal == NULL) {
            free(record);
            return false;
        }
        *equal = '\0';
        key = record;
        value = equal + 1;
        if (strcmp(key, "path") == 0) {
            free(pax->path);
            pax->path = xstrdup(value);
        }
        else if (strcmp(key, "linkpath") == 0) {
            free(pax->linkpath);
            pax->linkpath = xstrdup(value);
        }
        else if (strcmp(key, "GNU.sparse.name") == 0) {
            free(pax->path);
            pax->path = xstrdup(value);
            pax->sparse_enabled = true;
        }
        else if (strcmp(key, "GNU.sparse.major") == 0) {
            pax->sparse_major = atoi(value);
            pax->sparse_enabled = true;
        }
        else if (strcmp(key, "GNU.sparse.minor") == 0) {
            pax->sparse_minor = atoi(value);
            pax->sparse_enabled = true;
        }
        else if (strcmp(key, "GNU.sparse.realsize") == 0) {
            pax->sparse_realsize = (size_t)strtoull(value, NULL, 10);
            pax->sparse_enabled = true;
        }
        free(record);
        pos += line_len;
    }
    return true;
}

static bool bx_tar_parse_archive(const struct bx_archive_buffer* archive,
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
        char name_buf[256];
        char prefix_buf[156];
        char* name = NULL;
        struct bx_tar_entry entry;

        if (bx_tar_block_is_zero(header)) {
            if (pos + 2u * BX_TAR_BLOCK_SIZE <= archive->len
                && bx_tar_block_is_zero(archive->data + pos + BX_TAR_BLOCK_SIZE)) {
                break;
            }
            pos += BX_TAR_BLOCK_SIZE;
            continue;
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
            if (!bx_tar_parse_pax_records(&pax, archive->data + payload_start, size)) {
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

        memset(&entry, 0, sizeof(entry));
        memcpy(name_buf, header, 100u);
        name_buf[100] = '\0';
        memcpy(prefix_buf, header + 345, 155u);
        prefix_buf[155] = '\0';
        {
            const char* raw_name = (const char*)name_buf;
            const char* raw_prefix = (const char*)prefix_buf;
            if (raw_prefix[0] != '\0') {
                size_t full_len = strlen(raw_prefix) + 1u + strlen(raw_name);
                name = xmalloc(full_len + 1u);
                snprintf(name, full_len + 1u, "%s/%s", raw_prefix, raw_name);
            }
            else {
                name = xstrdup(raw_name);
            }
        }

        if (pax.path != NULL) {
            free(name);
            name = xstrdup(pax.path);
        }
        else if (gnu_long_name != NULL) {
            free(name);
            name = gnu_long_name;
            gnu_long_name = NULL;
        }
        if (typeflag == '5') {
            size_t name_len = strlen(name);
            while (name_len > 0u && name[name_len - 1u] == '/') {
                name[--name_len] = '\0';
            }
        }
        entry.name = name;
        entry.mode = 0644u;
        {
            size_t parsed_mode = 0u;
            size_t parsed_uid = 0u;
            size_t parsed_gid = 0u;
            size_t parsed_mtime = 0u;
            bx_tar_parse_octal_field(header + 100, 8u, &parsed_mode);
            bx_tar_parse_octal_field(header + 108, 8u, &parsed_uid);
            bx_tar_parse_octal_field(header + 116, 8u, &parsed_gid);
            bx_tar_parse_octal_field(header + 136, 12u, &parsed_mtime);
            entry.mode = (mode_t)parsed_mode;
            entry.uid = (uid_t)parsed_uid;
            entry.gid = (gid_t)parsed_gid;
            entry.mtime.tv_sec = (time_t)parsed_mtime;
            entry.mtime.tv_nsec = 0;
        }
        if (pax.linkpath != NULL) {
            entry.linkname = xstrdup(pax.linkpath);
        }
        else if ((typeflag == '1' || typeflag == '2') && gnu_long_link != NULL) {
            entry.linkname = gnu_long_link;
            gnu_long_link = NULL;
        }
        else if (typeflag == '1' || typeflag == '2') {
            char linkbuf[101];
            memcpy(linkbuf, header + 157, 100u);
            linkbuf[100] = '\0';
            entry.linkname = xstrdup(linkbuf);
        }

        switch (typeflag) {
            case '\0':
            case '0':
                entry.kind = BX_TAR_KIND_REG;
                break;
            case '5':
                entry.kind = BX_TAR_KIND_DIR;
                break;
            case '2':
                entry.kind = BX_TAR_KIND_SYMLINK;
                break;
            case '1':
                entry.kind = BX_TAR_KIND_HARDLINK;
                break;
            case '6':
                entry.kind = BX_TAR_KIND_FIFO;
                break;
            default:
                bx_tar_entry_free(&entry);
                bx_tar_pax_info_clear(&pax);
                free(gnu_long_name);
                free(gnu_long_link);
                bx_diag(diag, "unsupported tar entry type");
                return false;
        }

        if (entry.kind == BX_TAR_KIND_REG) {
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
                entry.size = size;
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

static bool bx_tar_entry_name_selected(const struct bx_tar_options* options,
                                       int argc,
                                       char** argv,
                                       const char* name) {
    int i;
    if (options->operand_index >= argc) {
        return true;
    }
    for (i = options->operand_index; i < argc; i++) {
        if (strcmp(argv[i], name) == 0) {
            return true;
        }
    }
    return false;
}

static char* bx_tar_sanitize_extract_name(const char* name,
                                          bool* stripped_absolute,
                                          bool* stripped_dotdot) {
    const char* p = name;
    struct bx_archive_buffer result;
    struct bx_path_components comps = {0};
    char* sanitized;

    *stripped_absolute = false;
    *stripped_dotdot = false;

    while (*p == '/') {
        *stripped_absolute = true;
        p++;
    }
    while (strncmp(p, "../", 3u) == 0) {
        *stripped_dotdot = true;
        p += 3u;
    }
    if (strcmp(p, "..") == 0) {
        *stripped_dotdot = true;
        p += 2u;
    }

    bx_path_components_append_raw(&comps, p);
    bx_archive_buffer_init(&result);
    if (comps.count == 0u) {
        bx_path_components_free(&comps);
        bx_archive_buffer_free(&result);
        return xstrdup("");
    }
    {
        size_t i;
        for (i = 0u; i < comps.count; i++) {
            if (strcmp(comps.parts[i], ".") == 0) {
                continue;
            }
            if (strcmp(comps.parts[i], "..") == 0) {
                *stripped_dotdot = true;
                continue;
            }
            if (result.len != 0u) {
                bx_archive_buffer_append_byte(&result, '/');
            }
            bx_archive_buffer_append(&result, comps.parts[i], strlen(comps.parts[i]));
        }
    }
    bx_archive_buffer_append_byte(&result, '\0');
    sanitized = xstrdup((const char*)result.data);
    bx_archive_buffer_free(&result);
    bx_path_components_free(&comps);
    return sanitized;
}

static bool bx_tar_write_sparse_file(const char* dest_path,
                                     const struct bx_tar_entry* entry,
                                     mode_t mode,
                                     struct bx_diag_ctx* diag) {
    int fd = open(dest_path, O_WRONLY | O_CREAT | O_TRUNC, mode & 07777u);
    size_t data_offset = 0u;
    size_t i;
    if (fd < 0) {
        bx_diag(diag, "%s: %s", dest_path, strerror(errno));
        return false;
    }
    for (i = 0u; i < entry->extent_count; i++) {
        size_t chunk = entry->extents[i].size;
        if (chunk == 0u) {
            continue;
        }
        if (lseek(fd, (off_t)entry->extents[i].offset, SEEK_SET) < 0) {
            bx_diag(diag, "%s: %s", dest_path, strerror(errno));
            close(fd);
            return false;
        }
        if (data_offset + chunk > entry->data_len) {
            bx_diag(diag, "%s: sparse data truncated", dest_path);
            close(fd);
            return false;
        }
        if (!bx_xwrite_all(fd, entry->data + data_offset, chunk)) {
            bx_diag(diag, "%s: %s", dest_path, strerror(errno));
            close(fd);
            return false;
        }
        data_offset += chunk;
    }
    if (ftruncate(fd, (off_t)entry->size) != 0) {
        bx_diag(diag, "%s: %s", dest_path, strerror(errno));
        close(fd);
        return false;
    }
    if (close(fd) != 0) {
        bx_diag(diag, "%s: %s", dest_path, strerror(errno));
        return false;
    }
    return true;
}

static int bx_tar_extract_entries(const struct bx_tar_entry_list* entries,
                                  const struct bx_tar_options* options,
                                  int argc,
                                  char** argv,
                                  struct bx_diag_ctx* diag) {
    struct bx_archive_pending_dirs dirs = {0};
    bool warned_absolute = false;
    bool warned_dotdot = false;
    int status = 0;
    size_t i;

    for (i = 0u; i < entries->len; i++) {
        const struct bx_tar_entry* entry = &entries->items[i];
        char* clean_name;
        char* dest_path;
        bool stripped_absolute;
        bool stripped_dotdot;

        if (!bx_tar_entry_name_selected(options, argc, argv, entry->name)) {
            continue;
        }

        clean_name = bx_tar_sanitize_extract_name(entry->name, &stripped_absolute, &stripped_dotdot);
        if (stripped_absolute && !warned_absolute) {
            fprintf(stderr, "%s: Removing leading '/' from member names\n", diag->progname);
            warned_absolute = true;
        }
        if (stripped_dotdot && !warned_dotdot) {
            fprintf(stderr, "%s: Removing leading '../' from member names\n", diag->progname);
            warned_dotdot = true;
        }
        if (clean_name[0] == '\0') {
            free(clean_name);
            continue;
        }

        if (options->to_stdout) {
            if (entry->kind == BX_TAR_KIND_REG) {
                if (entry->sparse) {
                    size_t logical = 0u;
                    size_t data_offset = 0u;
                    size_t j;
                    for (j = 0u; j < entry->extent_count; j++) {
                        size_t zero_len;
                        if (entry->extents[j].offset > logical) {
                            zero_len = entry->extents[j].offset - logical;
                            while (zero_len > 0u) {
                                unsigned char zeros[4096] = {0};
                                size_t chunk = zero_len > sizeof(zeros) ? sizeof(zeros) : zero_len;
                                if (!bx_xwrite_all(STDOUT_FILENO, zeros, chunk)) {
                                    bx_diag(diag, "write error: %s", strerror(errno));
                                    free(clean_name);
                                    bx_archive_pending_dirs_free(&dirs);
                                    return 2;
                                }
                                zero_len -= chunk;
                            }
                            logical = entry->extents[j].offset;
                        }
                        if (!bx_xwrite_all(STDOUT_FILENO, entry->data + data_offset, entry->extents[j].size)) {
                            bx_diag(diag, "write error: %s", strerror(errno));
                            free(clean_name);
                            bx_archive_pending_dirs_free(&dirs);
                            return 2;
                        }
                        data_offset += entry->extents[j].size;
                        logical = entry->extents[j].offset + entry->extents[j].size;
                    }
                    if (entry->size > logical) {
                        size_t zero_len = entry->size - logical;
                        while (zero_len > 0u) {
                            unsigned char zeros[4096] = {0};
                            size_t chunk = zero_len > sizeof(zeros) ? sizeof(zeros) : zero_len;
                            if (!bx_xwrite_all(STDOUT_FILENO, zeros, chunk)) {
                                bx_diag(diag, "write error: %s", strerror(errno));
                                free(clean_name);
                                bx_archive_pending_dirs_free(&dirs);
                                return 2;
                            }
                            zero_len -= chunk;
                        }
                    }
                }
                else if (!bx_xwrite_all(STDOUT_FILENO, entry->data, entry->data_len)) {
                    bx_diag(diag, "write error: %s", strerror(errno));
                    free(clean_name);
                    bx_archive_pending_dirs_free(&dirs);
                    return 2;
                }
            }
            free(clean_name);
            continue;
        }

        dest_path = options->extract_dir ? bx_path_join(options->extract_dir, clean_name) : xstrdup(clean_name);
        free(clean_name);

        if (options->keep_old_files && access(dest_path, F_OK) == 0 && entry->kind != BX_TAR_KIND_DIR) {
            fprintf(stderr, "%s: %s: Cannot open: File exists\n", diag->progname, entry->name);
            status = 2;
            free(dest_path);
            continue;
        }

        if (entry->kind == BX_TAR_KIND_DIR) {
            if (mkdir(dest_path, 0777u) != 0 && errno != EEXIST) {
                bx_diag(diag, "%s: %s", dest_path, strerror(errno));
                free(dest_path);
                bx_archive_pending_dirs_free(&dirs);
                return 2;
            }
            bx_archive_pending_dirs_record(&dirs, dest_path, entry->mode, true, entry->mtime);
        }
        else {
            if (!bx_archive_ensure_parent_dirs(dest_path, diag)) {
                free(dest_path);
                bx_archive_pending_dirs_free(&dirs);
                return 2;
            }
            if (entry->kind == BX_TAR_KIND_REG) {
                if (entry->sparse) {
                    if (!bx_tar_write_sparse_file(dest_path, entry, entry->mode, diag)) {
                        free(dest_path);
                        bx_archive_pending_dirs_free(&dirs);
                        return 2;
                    }
                }
                else {
                    int fd = open(dest_path, O_WRONLY | O_CREAT | O_TRUNC, entry->mode & 07777u);
                    if (fd < 0) {
                        bx_diag(diag, "%s: %s", dest_path, strerror(errno));
                        free(dest_path);
                        bx_archive_pending_dirs_free(&dirs);
                        return 2;
                    }
                    if (!bx_archive_write_regular_payload(fd, entry->data, entry->data_len, false, diag)) {
                        close(fd);
                        free(dest_path);
                        bx_archive_pending_dirs_free(&dirs);
                        return 2;
                    }
                    if (close(fd) != 0) {
                        bx_diag(diag, "%s: %s", dest_path, strerror(errno));
                        free(dest_path);
                        bx_archive_pending_dirs_free(&dirs);
                        return 2;
                    }
                }
                if (chmod(dest_path, entry->mode & 07777u) != 0) {
                    bx_diag(diag, "%s: %s", dest_path, strerror(errno));
                    free(dest_path);
                    bx_archive_pending_dirs_free(&dirs);
                    return 2;
                }
                if (!bx_archive_set_path_mtime(dest_path, entry->mtime, false, diag)) {
                    free(dest_path);
                    bx_archive_pending_dirs_free(&dirs);
                    return 2;
                }
            }
            else if (entry->kind == BX_TAR_KIND_SYMLINK) {
                unlink(dest_path);
                if (symlink(entry->linkname, dest_path) != 0) {
                    bx_diag(diag, "%s: %s", dest_path, strerror(errno));
                    free(dest_path);
                    bx_archive_pending_dirs_free(&dirs);
                    return 2;
                }
                if (!bx_archive_set_path_mtime(dest_path, entry->mtime, true, diag)) {
                    free(dest_path);
                    bx_archive_pending_dirs_free(&dirs);
                    return 2;
                }
            }
            else if (entry->kind == BX_TAR_KIND_HARDLINK) {
                char* target = options->extract_dir ? bx_path_join(options->extract_dir, entry->linkname) : xstrdup(entry->linkname);
                if (!bx_archive_ensure_parent_dirs(dest_path, diag)) {
                    free(dest_path);
                    free(target);
                    bx_archive_pending_dirs_free(&dirs);
                    return 2;
                }
                unlink(dest_path);
                if (link(target, dest_path) != 0) {
                    bx_diag(diag, "%s: %s", dest_path, strerror(errno));
                    free(dest_path);
                    free(target);
                    bx_archive_pending_dirs_free(&dirs);
                    return 2;
                }
                free(target);
            }
            else if (entry->kind == BX_TAR_KIND_FIFO) {
                unlink(dest_path);
                if (mkfifo(dest_path, entry->mode & 07777u) != 0) {
                    bx_diag(diag, "%s: %s", dest_path, strerror(errno));
                    free(dest_path);
                    bx_archive_pending_dirs_free(&dirs);
                    return 2;
                }
                if (!bx_archive_set_path_mtime(dest_path, entry->mtime, false, diag)) {
                    free(dest_path);
                    bx_archive_pending_dirs_free(&dirs);
                    return 2;
                }
            }
        }
        free(dest_path);
    }

    if (!bx_archive_pending_dirs_apply(&dirs, diag)) {
        bx_archive_pending_dirs_free(&dirs);
        return 2;
    }
    bx_archive_pending_dirs_free(&dirs);
    if (status == 2) {
        fprintf(stderr, "%s: Exiting with failure status due to previous errors\n", diag->progname);
    }
    return status;
}

static int bx_tar_list_entries(const struct bx_tar_entry_list* entries,
                               const struct bx_tar_options* options,
                               int argc,
                               char** argv,
                               struct bx_diag_ctx* diag) {
    size_t i;
    (void)options;
    for (i = 0u; i < entries->len; i++) {
        const struct bx_tar_entry* entry = &entries->items[i];
        if (!bx_tar_entry_name_selected(options, argc, argv, entry->name)) {
            continue;
        }
        if (entry->kind == BX_TAR_KIND_DIR) {
            if (printf("%s/\n", entry->name) < 0) {
                bx_diag(diag, "write error: %s", strerror(errno));
                return 2;
            }
        }
        else {
            if (printf("%s\n", entry->name) < 0) {
                bx_diag(diag, "write error: %s", strerror(errno));
                return 2;
            }
        }
    }
    return 0;
}

static bool bx_tar_write_parsed_entry(struct bx_archive_buffer* archive,
                                      const struct bx_tar_entry* entry,
                                      struct bx_diag_ctx* diag) {
    if (!bx_tar_write_header(archive,
                             entry->name,
                             entry->linkname,
                             entry->kind,
                             entry->mode,
                             entry->uid,
                             entry->gid,
                             entry->kind == BX_TAR_KIND_REG ? entry->data_len : 0u,
                             entry->mtime,
                             true)) {
        bx_diag(diag, "archive write failed: unsupported pathname");
        return false;
    }
    if (entry->kind == BX_TAR_KIND_REG && !bx_tar_write_entry_data(archive, entry->data, entry->data_len)) {
        bx_diag(diag, "archive write failed: %s", strerror(errno));
        return false;
    }
    return true;
}

static int bx_tar_rewrite_archive(const struct bx_tar_options* options,
                                  int argc,
                                  char** argv,
                                  struct bx_diag_ctx* diag) {
    struct bx_archive_buffer input = {0};
    struct bx_tar_entry_list parsed = {0};
    struct bx_archive_buffer output = {0};
    int rc = 2;
    size_t i;

    if (!bx_tar_read_archive_input(options, &input, diag)) {
        return 2;
    }
    if (!bx_tar_parse_archive(&input, &parsed, diag)) {
        bx_archive_buffer_free(&input);
        return 2;
    }
    bx_archive_buffer_free(&input);
    bx_archive_buffer_init(&output);

    for (i = 0u; i < parsed.len; i++) {
        const struct bx_tar_entry* entry = &parsed.items[i];
        bool selected = bx_tar_entry_name_selected(options, argc, argv, entry->name);
        if (options->mode == BX_TAR_MODE_DELETE && selected) {
            continue;
        }
        if (!bx_tar_write_parsed_entry(&output, entry, diag)) {
            goto out;
        }
    }

    if (options->mode == BX_TAR_MODE_APPEND) {
        struct bx_tar_options create_options = *options;
        struct bx_archive_buffer appended = {0};
        create_options.mode = BX_TAR_MODE_CREATE;
        if (!bx_tar_build_create_archive(&appended, &create_options, argc, argv, diag)) {
            goto out;
        }
        if (appended.len >= 2u * BX_TAR_BLOCK_SIZE) {
            appended.len -= 2u * BX_TAR_BLOCK_SIZE;
        }
        if (!bx_archive_buffer_append(&output, appended.data, appended.len)) {
            bx_archive_buffer_free(&appended);
            bx_diag(diag, "archive write failed: %s", strerror(errno));
            goto out;
        }
        bx_archive_buffer_free(&appended);
    }

    if (!bx_tar_finish_archive(&output)) {
        bx_diag(diag, "archive write failed: %s", strerror(errno));
        goto out;
    }
    if (!bx_tar_write_archive_output(options, &output, diag)) {
        goto out;
    }
    rc = 0;
out:
    bx_archive_buffer_free(&output);
    bx_tar_entry_list_free(&parsed);
    return rc;
}

static bool bx_tar_parse_time_arg(const char* text, struct timespec* out) {
    if (text[0] != '@') {
        return false;
    }
    out->tv_sec = (time_t)strtoll(text + 1, NULL, 10);
    out->tv_nsec = 0;
    return true;
}

static bool bx_tar_warning_keyword_supported(const char* text) {
    return strcmp(text, "decompress-program") == 0
        || strcmp(text, "no-decompress-program") == 0;
}

static bool bx_tar_parse_options(struct bx_tar_options* options,
                                 int argc,
                                 char** argv,
                                 struct bx_diag_ctx* diag) {
    int i = 1;
    bool oldstyle = false;

    memset(options, 0, sizeof(*options));
    options->operand_index = argc;

    if (i < argc && argv[i][0] != '-' && argv[i][0] != '\0') {
        oldstyle = true;
    }

    while (i < argc) {
        char* arg = argv[i];
        if (!oldstyle && arg[0] != '-') {
            options->operand_index = i;
            break;
        }
        if (!oldstyle && strcmp(arg, "--") == 0) {
            options->operand_index = i + 1;
            break;
        }
        if (!oldstyle && strncmp(arg, "--", 2u) == 0) {
            const char* value = strchr(arg, '=');
            size_t name_len = value ? (size_t)(value - arg) : strlen(arg);
            if (strncmp(arg, "--format", name_len) == 0 && name_len == 8u) {
                if (value == NULL && ++i >= argc) {
                    bx_diag(diag, "option '--format' requires an argument");
                    return false;
                }
                value = value ? value + 1 : argv[i];
                if (strcmp(value, "ustar") != 0) {
                    bx_diag(diag, "unsupported format '%s'", value);
                    return false;
                }
                options->format_ustar = true;
            }
            else if (strncmp(arg, "--sort", name_len) == 0 && name_len == 6u) {
                if (value == NULL && ++i >= argc) {
                    bx_diag(diag, "option '--sort' requires an argument");
                    return false;
                }
                value = value ? value + 1 : argv[i];
                if (strcmp(value, "name") != 0) {
                    bx_diag(diag, "unsupported sort order '%s'", value);
                    return false;
                }
                options->sort_name = true;
            }
            else if (strncmp(arg, "--mtime", name_len) == 0 && name_len == 7u) {
                if (value == NULL && ++i >= argc) {
                    bx_diag(diag, "option '--mtime' requires an argument");
                    return false;
                }
                value = value ? value + 1 : argv[i];
                if (!bx_tar_parse_time_arg(value, &options->mtime)) {
                    bx_diag(diag, "unsupported time '%s'", value);
                    return false;
                }
                options->fixed_mtime = true;
            }
            else if (strcmp(arg, "--numeric-owner") == 0) {
                /* accepted for compatibility; bx stores numeric ids directly */
            }
            else if ((strncmp(arg, "--owner", name_len) == 0 && name_len == 7u)
                     || (strncmp(arg, "--group", name_len) == 0 && name_len == 7u)) {
                bool is_owner = (arg[2] == 'o');
                if (value == NULL && ++i >= argc) {
                    bx_diag(diag, "option '%s' requires an argument", is_owner ? "--owner" : "--group");
                    return false;
                }
                value = value ? value + 1 : argv[i];
                if (is_owner) {
                    options->owner = (uid_t)strtoul(value, NULL, 10);
                    options->owner_set = true;
                }
                else {
                    options->group = (gid_t)strtoul(value, NULL, 10);
                    options->group_set = true;
                }
            }
            else if (strcmp(arg, "--delete") == 0) {
                options->mode = BX_TAR_MODE_DELETE;
            }
            else if (strcmp(arg, "--xattrs") == 0) {
                options->xattrs = true;
            }
            else if (strcmp(arg, "--acls") == 0) {
                options->acls = true;
            }
            else if (strncmp(arg, "--warning", name_len) == 0 && name_len == 9u) {
                if (value == NULL && ++i >= argc) {
                    bx_diag(diag, "option '--warning' requires an argument");
                    return false;
                }
                value = value ? value + 1 : argv[i];
                if (!bx_tar_warning_keyword_supported(value)) {
                    bx_diag(diag, "invalid argument '%s' for '--warning'", value);
                    return false;
                }
            }
            else {
                bx_diag(diag, "unrecognized option '%s'", arg);
                return false;
            }
            i++;
            continue;
        }

        {
            const char* letters = oldstyle ? arg : arg + 1;
            size_t j;
            for (j = 0u; letters[j] != '\0'; j++) {
                char ch = letters[j];
                const char* attached = &letters[j + 1u];
                switch (ch) {
                    case 'c': options->mode = BX_TAR_MODE_CREATE; break;
                    case 't': options->mode = BX_TAR_MODE_LIST; break;
                    case 'x': options->mode = BX_TAR_MODE_EXTRACT; break;
                    case 'r': options->mode = BX_TAR_MODE_APPEND; break;
                    case 'O': options->to_stdout = true; break;
                    case 'o':
                        if (options->mode != BX_TAR_MODE_EXTRACT) {
                            bx_diag(diag, "invalid option -- '%c'", ch);
                            return false;
                        }
                        break;
                    case 'k': options->keep_old_files = true; break;
                    case 'z': options->gzip = true; break;
                    case 'a': options->auto_compress = true; break;
                    case 'P': break;
                    case 'f':
                        if (*attached != '\0') {
                            options->archive_path = attached;
                            j = strlen(letters) - 1u;
                        }
                        else if (++i < argc) {
                            options->archive_path = argv[i];
                        }
                        else {
                            bx_diag(diag, "option requires an argument -- 'f'");
                            return false;
                        }
                        goto next_arg;
                    case 'C':
                        if (*attached != '\0') {
                            if (options->mode == BX_TAR_MODE_EXTRACT) {
                                options->extract_dir = attached;
                            }
                            else {
                                options->create_cwd = attached;
                            }
                            j = strlen(letters) - 1u;
                        }
                        else if (++i < argc) {
                            if (options->mode == BX_TAR_MODE_EXTRACT) {
                                options->extract_dir = argv[i];
                            }
                            else {
                                options->create_cwd = argv[i];
                            }
                        }
                        else {
                            bx_diag(diag, "option requires an argument -- 'C'");
                            return false;
                        }
                        goto next_arg;
                    default:
                        bx_diag(diag, "invalid option -- '%c'", ch);
                        return false;
                }
            }
        next_arg: ;
        }
        oldstyle = false;
        i++;
    }

    if (options->operand_index == argc) {
        options->operand_index = i;
    }
    if (options->mode == BX_TAR_MODE_NONE) {
        bx_diag(diag, "you must specify one of the '-c', '-t', '-x', '-r', or '--delete' options");
        return false;
    }
    if (options->archive_path == NULL) {
        bx_diag(diag, "archive file not specified; use -f");
        return false;
    }
    if ((options->mode == BX_TAR_MODE_CREATE || options->mode == BX_TAR_MODE_APPEND) && options->operand_index >= argc) {
        bx_diag(diag, "missing file operand");
        return false;
    }
    return true;
}

int bx_tar_run(int argc, char** argv) {
    struct bx_tar_options options;
    struct bx_diag_ctx diag = {
        .progname = bx_tar_progname(argv, argc),
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };

    if (!bx_tar_parse_options(&options, argc, argv, &diag)) {
        return 2;
    }

    if (options.mode == BX_TAR_MODE_CREATE) {
        struct bx_archive_buffer archive = {0};
        int rc;
        if (!bx_tar_build_create_archive(&archive, &options, argc, argv, &diag)) {
            return 2;
        }
        rc = bx_tar_write_archive_output(&options, &archive, &diag) ? 0 : 2;
        bx_archive_buffer_free(&archive);
        return rc;
    }
    if (options.mode == BX_TAR_MODE_APPEND || options.mode == BX_TAR_MODE_DELETE) {
        return bx_tar_rewrite_archive(&options, argc, argv, &diag);
    }
    else {
        struct bx_archive_buffer archive = {0};
        struct bx_tar_entry_list entries = {0};
        int rc;
        if (!bx_tar_read_archive_input(&options, &archive, &diag)) {
            return 2;
        }
        if (!bx_tar_parse_archive(&archive, &entries, &diag)) {
            bx_archive_buffer_free(&archive);
            return 2;
        }
        bx_archive_buffer_free(&archive);
        if (options.mode == BX_TAR_MODE_LIST) {
            rc = bx_tar_list_entries(&entries, &options, argc, argv, &diag);
        }
        else {
            rc = bx_tar_extract_entries(&entries, &options, argc, argv, &diag);
        }
        bx_tar_entry_list_free(&entries);
        return rc;
    }
}
