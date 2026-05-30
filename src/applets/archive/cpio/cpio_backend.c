#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <unistd.h>

#include "applets/archive/archive_common.h"
#include "applets/archive/archive_fs.h"
#include "applets/archive/cpio/cpio_backend.h"
#include "bx/libbx.h"
#include "lib/cli_common.h"
#include "lib/fd_ops.h"
#include "lib/id_parse.h"
#include "lib/line_writer.h"
#include "lib/path_ops.h"
#include "lib/xreadwrite.h"

#define BX_CPIO_NEWC_HEADER_LEN 110u
#define BX_CPIO_ODC_HEADER_LEN 76u

enum bx_cpio_mode {
    BX_CPIO_MODE_NONE = 0,
    BX_CPIO_MODE_COPY_OUT,
    BX_CPIO_MODE_COPY_IN,
    BX_CPIO_MODE_PASS,
};

enum bx_cpio_format {
    BX_CPIO_FORMAT_NEWC = 0,
    BX_CPIO_FORMAT_ODC,
};

enum bx_cpio_kind {
    BX_CPIO_KIND_REG = 0,
    BX_CPIO_KIND_DIR,
    BX_CPIO_KIND_SYMLINK,
    BX_CPIO_KIND_FIFO,
};

struct bx_cpio_options {
    enum bx_cpio_mode mode;
    enum bx_cpio_format format;
    const char* archive_path;
    const char* pass_dir;
    bool list;
    bool quiet;
    bool null_input;
    bool preserve_mtime;
    bool to_stdout;
    bool sparse;
    bool reproducible;
    bool owner_override;
    uid_t owner;
    gid_t group;
    int operand_index;
};

struct bx_cpio_entry {
    char* name;
    enum bx_cpio_kind kind;
    mode_t mode;
    uid_t uid;
    gid_t gid;
    nlink_t nlink;
    struct timespec mtime;
    uint32_t ino;
    size_t size;
    char* link_target;
    unsigned char* data;
    size_t data_len;
};

struct bx_cpio_entry_list {
    struct bx_cpio_entry* items;
    size_t len;
    size_t cap;
};

struct bx_cpio_inode_map {
    dev_t dev;
    ino_t ino;
    uint32_t synthetic_ino;
    size_t total_count;
    size_t seen_count;
};

struct bx_cpio_inode_map_list {
    struct bx_cpio_inode_map* items;
    size_t len;
    size_t cap;
};

struct bx_cpio_hardlink_state {
    uint32_t ino;
    char* materialized_path;
    char** deferred_paths;
    size_t deferred_len;
    size_t deferred_cap;
};

struct bx_cpio_hardlink_state_list {
    struct bx_cpio_hardlink_state* items;
    size_t len;
    size_t cap;
};

static const char* bx_cpio_progname(char** argv, int argc) {
    return bx_cli_progname((argc > 0) ? argv[0] : NULL, "cpio");
}

static void bx_cpio_entry_free(struct bx_cpio_entry* entry) {
    free(entry->name);
    free(entry->link_target);
    free(entry->data);
    entry->name = NULL;
    entry->link_target = NULL;
    entry->data = NULL;
}

static void bx_cpio_entry_list_free(struct bx_cpio_entry_list* list) {
    size_t i;
    for (i = 0u; i < list->len; i++) {
        bx_cpio_entry_free(&list->items[i]);
    }
    free(list->items);
    list->items = NULL;
    list->len = 0u;
    list->cap = 0u;
}

static bool bx_cpio_entry_list_push(struct bx_cpio_entry_list* list, const struct bx_cpio_entry* entry) {
    struct bx_cpio_entry* slot;
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

static ssize_t bx_cpio_find_inode_map(const struct bx_cpio_inode_map_list* maps, dev_t dev, ino_t ino) {
    size_t i;
    for (i = 0u; i < maps->len; i++) {
        if (maps->items[i].dev == dev && maps->items[i].ino == ino) {
            return (ssize_t)i;
        }
    }
    return -1;
}

static struct bx_cpio_inode_map* bx_cpio_get_inode_map(struct bx_cpio_inode_map_list* maps,
                                                        dev_t dev,
                                                        ino_t ino) {
    ssize_t idx = bx_cpio_find_inode_map(maps, dev, ino);
    if (idx >= 0) {
        return &maps->items[idx];
    }
    if (maps->len == maps->cap) {
        size_t next_cap = maps->cap ? maps->cap * 2u : 16u;
        maps->items = xrealloc(maps->items, next_cap * sizeof(*maps->items));
        maps->cap = next_cap;
    }
    maps->items[maps->len].dev = dev;
    maps->items[maps->len].ino = ino;
    maps->items[maps->len].synthetic_ino = (uint32_t)maps->len;
    maps->items[maps->len].total_count = 0u;
    maps->items[maps->len].seen_count = 0u;
    return &maps->items[maps->len++];
}

static void bx_cpio_inode_maps_free(struct bx_cpio_inode_map_list* maps) {
    free(maps->items);
    maps->items = NULL;
    maps->len = 0u;
    maps->cap = 0u;
}

static bool bx_cpio_parse_hex_field(const unsigned char* field, size_t len, size_t* value_out) {
    size_t value = 0u;
    size_t i;
    for (i = 0u; i < len; i++) {
        unsigned char ch = field[i];
        value <<= 4u;
        if (ch >= '0' && ch <= '9') {
            value |= (size_t)(ch - '0');
        }
        else if (ch >= 'a' && ch <= 'f') {
            value |= (size_t)(10 + ch - 'a');
        }
        else if (ch >= 'A' && ch <= 'F') {
            value |= (size_t)(10 + ch - 'A');
        }
        else {
            return false;
        }
    }
    *value_out = value;
    return true;
}

static void bx_cpio_format_hex_field(unsigned char* field, size_t len, size_t value) {
    static const char digits[] = "0123456789ABCDEF";
    size_t i;
    for (i = 0u; i < len; i++) {
        field[len - 1u - i] = (unsigned char)digits[value & 0x0fu];
        value >>= 4u;
    }
}

static bool bx_cpio_parse_octal_field(const unsigned char* field, size_t len, size_t* value_out) {
    size_t value = 0u;
    size_t i;
    for (i = 0u; i < len; i++) {
        unsigned char ch = field[i];
        if (ch < '0' || ch > '7') {
            return false;
        }
        value = (value << 3u) + (size_t)(ch - '0');
    }
    *value_out = value;
    return true;
}

static void bx_cpio_format_octal_field(unsigned char* field, size_t len, size_t value) {
    size_t i;
    for (i = 0u; i < len; i++) {
        field[len - 1u - i] = (unsigned char)('0' + (value & 0x07u));
        value >>= 3u;
    }
}

static bool bx_cpio_parse_owner_spec(const char* text,
                                     struct bx_cpio_options* options,
                                     struct bx_diag_ctx* diag) {
    char* spec = xstrdup(text);
    char* colon = strchr(spec, ':');
    uintmax_t owner = 0u;
    uintmax_t group = 0u;

    if (colon == NULL) {
        bx_diag(diag, "invalid owner spec '%s'", text);
        free(spec);
        return false;
    }

    *colon = '\0';
    if (!bx_id_parse_numeric(spec, (uintmax_t)((uid_t)-1), &owner)
        || !bx_id_parse_numeric(colon + 1, (uintmax_t)((gid_t)-1), &group)) {
        bx_diag(diag, "invalid owner spec '%s'", text);
        free(spec);
        return false;
    }

    options->owner = (uid_t)owner;
    options->group = (gid_t)group;
    options->owner_override = true;
    free(spec);
    return true;
}

static bool bx_cpio_read_file(const char* path, struct bx_archive_buffer* buffer, struct bx_diag_ctx* diag) {
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

static bool bx_cpio_emit_newc_entry(struct bx_archive_buffer* archive,
                                    const char* name,
                                    uint32_t ino,
                                    mode_t mode,
                                    uid_t uid,
                                    gid_t gid,
                                    nlink_t nlink,
                                    struct timespec mtime,
                                    size_t size,
                                    const unsigned char* data) {
    unsigned char header[BX_CPIO_NEWC_HEADER_LEN + 1u];
    size_t namesize = strlen(name) + 1u;
    size_t padded;

    memcpy(header, "070701", 6u);
    bx_cpio_format_hex_field(header + 6, 8u, ino);
    bx_cpio_format_hex_field(header + 14, 8u, mode);
    bx_cpio_format_hex_field(header + 22, 8u, uid);
    bx_cpio_format_hex_field(header + 30, 8u, gid);
    bx_cpio_format_hex_field(header + 38, 8u, nlink);
    bx_cpio_format_hex_field(header + 46, 8u, (size_t)mtime.tv_sec);
    bx_cpio_format_hex_field(header + 54, 8u, size);
    bx_cpio_format_hex_field(header + 62, 8u, 0u);
    bx_cpio_format_hex_field(header + 70, 8u, 0u);
    bx_cpio_format_hex_field(header + 78, 8u, 0u);
    bx_cpio_format_hex_field(header + 86, 8u, 0u);
    bx_cpio_format_hex_field(header + 94, 8u, namesize);
    bx_cpio_format_hex_field(header + 102, 8u, 0u);
    if (!bx_archive_buffer_append(archive, header, BX_CPIO_NEWC_HEADER_LEN)
        || !bx_archive_buffer_append(archive, name, namesize)) {
        return false;
    }
    while (archive->len % 4u != 0u) {
        if (!bx_archive_buffer_append_byte(archive, 0u)) {
            return false;
        }
    }
    if (size != 0u && !bx_archive_buffer_append(archive, data, size)) {
        return false;
    }
    padded = (4u - (archive->len % 4u)) % 4u;
    return bx_archive_buffer_append_zeros(archive, padded);
}

static bool bx_cpio_emit_odc_entry(struct bx_archive_buffer* archive,
                                   const char* name,
                                   uint32_t ino,
                                   mode_t mode,
                                   uid_t uid,
                                   gid_t gid,
                                   nlink_t nlink,
                                   struct timespec mtime,
                                   size_t size,
                                   const unsigned char* data) {
    unsigned char header[BX_CPIO_ODC_HEADER_LEN + 1u];
    size_t namesize = strlen(name) + 1u;

    memcpy(header, "070707", 6u);
    bx_cpio_format_octal_field(header + 6, 6u, 0u);
    bx_cpio_format_octal_field(header + 12, 6u, ino & 0777777u);
    bx_cpio_format_octal_field(header + 18, 6u, mode & 0777777u);
    bx_cpio_format_octal_field(header + 24, 6u, uid & 0777777u);
    bx_cpio_format_octal_field(header + 30, 6u, gid & 0777777u);
    bx_cpio_format_octal_field(header + 36, 6u, nlink & 0777777u);
    bx_cpio_format_octal_field(header + 42, 6u, 0u);
    bx_cpio_format_octal_field(header + 48, 11u, (size_t)mtime.tv_sec);
    bx_cpio_format_octal_field(header + 59, 6u, namesize);
    bx_cpio_format_octal_field(header + 65, 11u, size);
    if (!bx_archive_buffer_append(archive, header, BX_CPIO_ODC_HEADER_LEN)
        || !bx_archive_buffer_append(archive, name, namesize)) {
        return false;
    }
    if (size != 0u && !bx_archive_buffer_append(archive, data, size)) {
        return false;
    }
    if (archive->len % 2u != 0u) {
        return bx_archive_buffer_append_byte(archive, 0u);
    }
    return true;
}

static bool bx_cpio_read_name_list(const struct bx_cpio_options* options,
                                   char*** names_out,
                                   size_t* count_out,
                                   struct bx_diag_ctx* diag) {
    struct bx_archive_name_list names = {0};

    if (!bx_archive_name_list_read_stream(
            stdin,
            options->null_input ? '\0' : '\n',
            &names,
            diag)) {
        return false;
    }

    *names_out = names.items;
    *count_out = names.len;
    return true;
}

static void bx_cpio_free_name_list(char** names, size_t count) {
    size_t i;
    for (i = 0u; i < count; i++) {
        free(names[i]);
    }
    free(names);
}

static bool bx_cpio_build_fs_list(struct bx_archive_fs_list* list,
                                  char** names,
                                  size_t count,
                                  struct bx_diag_ctx* diag) {
    size_t i;
    for (i = 0u; i < count; i++) {
        if (!bx_archive_fs_add_path(list, names[i], names[i], false, false, diag)) {
            return false;
        }
    }
    return true;
}

static void bx_cpio_count_inodes(const struct bx_archive_fs_list* list,
                                 struct bx_cpio_inode_map_list* maps) {
    size_t i;
    for (i = 0u; i < list->len; i++) {
        const struct bx_archive_fs_entry* entry = &list->entries[i];
        struct bx_cpio_inode_map* map = bx_cpio_get_inode_map(maps, entry->st.st_dev, entry->st.st_ino);
        map->total_count++;
    }
}

static bool bx_cpio_emit_one_fs_entry(struct bx_archive_buffer* archive,
                                      const struct bx_archive_fs_entry* entry,
                                      uint32_t ino,
                                      const struct bx_cpio_options* options,
                                      bool suppress_data,
                                      struct bx_diag_ctx* diag) {
    mode_t mode = entry->st.st_mode;
    uid_t uid = options->owner_override ? options->owner : entry->st.st_uid;
    gid_t gid = options->owner_override ? options->group : entry->st.st_gid;
    nlink_t nlink = S_ISDIR(mode) ? 2u : entry->st.st_nlink;
    struct timespec mtime = entry->st.st_mtim;
    struct bx_archive_buffer data = {0};
    const unsigned char* payload = NULL;
    size_t size = 0u;

    if (options->reproducible) {
        uid = options->owner_override ? options->owner : 0u;
        gid = options->owner_override ? options->group : 0u;
    }

    if (S_ISLNK(mode)) {
        payload = (const unsigned char*)entry->link_target;
        size = strlen(entry->link_target);
    }
    else if (S_ISREG(mode) && !suppress_data) {
        if (!bx_cpio_read_file(entry->source_path, &data, diag)) {
            return false;
        }
        payload = data.data;
        size = data.len;
    }
    else if (S_ISREG(mode)) {
        size = 0u;
    }

    if (options->format == BX_CPIO_FORMAT_NEWC) {
        if (!bx_cpio_emit_newc_entry(archive,
                                     entry->archive_path,
                                     ino,
                                     mode,
                                     uid,
                                     gid,
                                     nlink,
                                     mtime,
                                     size,
                                     payload)) {
            bx_archive_buffer_free(&data);
            return false;
        }
    }
    else {
        if (!bx_cpio_emit_odc_entry(archive,
                                    entry->archive_path,
                                    ino,
                                    mode,
                                    uid,
                                    gid,
                                    nlink,
                                    mtime,
                                    size,
                                    payload)) {
            bx_archive_buffer_free(&data);
            return false;
        }
    }

    bx_archive_buffer_free(&data);
    return true;
}

static bool bx_cpio_build_archive(struct bx_archive_buffer* archive,
                                  const struct bx_cpio_options* options,
                                  char** names,
                                  size_t name_count,
                                  struct bx_diag_ctx* diag) {
    struct bx_archive_fs_list files = {0};
    struct bx_cpio_inode_map_list maps = {0};
    bool* emitted = NULL;
    size_t i;

    bx_archive_buffer_init(archive);
    if (!bx_cpio_build_fs_list(&files, names, name_count, diag)) {
        return false;
    }
    bx_cpio_count_inodes(&files, &maps);
    emitted = xmalloc(files.len * sizeof(*emitted));
    memset(emitted, 0, files.len * sizeof(*emitted));

    for (i = 0u; i < files.len; i++) {
        const struct bx_archive_fs_entry* entry = &files.entries[i];
        struct bx_cpio_inode_map* map = bx_cpio_get_inode_map(&maps, entry->st.st_dev, entry->st.st_ino);
        if (emitted[i]) {
            continue;
        }
        if (S_ISREG(entry->st.st_mode) && map->total_count > 1u) {
            map->seen_count++;
            if (map->seen_count < map->total_count) {
                continue;
            }
            {
                size_t j;
                for (j = 0u; j <= i; j++) {
                    const struct bx_archive_fs_entry* group_entry = &files.entries[j];
                    struct bx_cpio_inode_map* group_map = bx_cpio_get_inode_map(&maps, group_entry->st.st_dev, group_entry->st.st_ino);
                    if (emitted[j] || group_map != map) {
                        continue;
                    }
                    if (!bx_cpio_emit_one_fs_entry(archive,
                                                   group_entry,
                                                   map->synthetic_ino,
                                                   options,
                                                   j != i,
                                                   diag)) {
                        free(emitted);
                        bx_cpio_inode_maps_free(&maps);
                        bx_archive_fs_list_free(&files);
                        return false;
                    }
                    emitted[j] = true;
                }
            }
            continue;
        }
        if (!bx_cpio_emit_one_fs_entry(archive, entry, map->synthetic_ino, options, false, diag)) {
            free(emitted);
            bx_cpio_inode_maps_free(&maps);
            bx_archive_fs_list_free(&files);
            return false;
        }
        emitted[i] = true;
    }

    if (options->format == BX_CPIO_FORMAT_NEWC) {
        struct timespec zero = {0, 0};
        bx_cpio_emit_newc_entry(archive, "TRAILER!!!", 0u, 0u, 0u, 0u, 1u, zero, 0u, NULL);
        while (archive->len % 512u != 0u) {
            bx_archive_buffer_append_byte(archive, 0u);
        }
    }
    else {
        struct timespec zero = {0, 0};
        bx_cpio_emit_odc_entry(archive, "TRAILER!!!", 0u, 0u, 0u, 0u, 1u, zero, 0u, NULL);
        if (archive->len % 2u != 0u) {
            bx_archive_buffer_append_byte(archive, 0u);
        }
    }

    free(emitted);
    bx_cpio_inode_maps_free(&maps);
    bx_archive_fs_list_free(&files);
    return true;
}

static bool bx_cpio_write_archive_output(const struct bx_cpio_options* options,
                                         const struct bx_archive_buffer* archive,
                                         struct bx_diag_ctx* diag) {
    FILE* stream;
    if (options->archive_path == NULL) {
        return bx_archive_buffer_write_all(stdout, archive, diag);
    }
    stream = fopen(options->archive_path, "wb");
    if (stream == NULL) {
        bx_diag(diag, "%s: %s", options->archive_path, strerror(errno));
        return false;
    }
    if (!bx_archive_buffer_write_all(stream, archive, diag)) {
        fclose(stream);
        return false;
    }
    if (fclose(stream) != 0) {
        bx_diag(diag, "%s: %s", options->archive_path, strerror(errno));
        return false;
    }
    return true;
}

static bool bx_cpio_read_archive_input(const struct bx_cpio_options* options,
                                       struct bx_archive_buffer* archive,
                                       struct bx_diag_ctx* diag) {
    FILE* stream;
    bx_archive_buffer_init(archive);
    if (options->archive_path == NULL) {
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
        return false;
    }
    if (stream != stdin && fclose(stream) != 0) {
        bx_diag(diag, "%s: %s", options->archive_path, strerror(errno));
        return false;
    }
    return true;
}

static bool bx_cpio_parse_newc_archive(const struct bx_archive_buffer* archive,
                                       struct bx_cpio_entry_list* entries,
                                       struct bx_diag_ctx* diag) {
    size_t pos = 0u;
    while (pos + BX_CPIO_NEWC_HEADER_LEN <= archive->len) {
        const unsigned char* header = archive->data + pos;
        size_t ino, mode, uid, gid, nlink, mtime, size, namesize;
        struct bx_cpio_entry entry;
        memset(&entry, 0, sizeof(entry));
        if (memcmp(header, "070701", 6u) != 0) {
            bx_diag(diag, "invalid newc header");
            return false;
        }
        if (!bx_cpio_parse_hex_field(header + 6, 8u, &ino)
            || !bx_cpio_parse_hex_field(header + 14, 8u, &mode)
            || !bx_cpio_parse_hex_field(header + 22, 8u, &uid)
            || !bx_cpio_parse_hex_field(header + 30, 8u, &gid)
            || !bx_cpio_parse_hex_field(header + 38, 8u, &nlink)
            || !bx_cpio_parse_hex_field(header + 46, 8u, &mtime)
            || !bx_cpio_parse_hex_field(header + 54, 8u, &size)
            || !bx_cpio_parse_hex_field(header + 94, 8u, &namesize)) {
            bx_diag(diag, "invalid newc header");
            return false;
        }
        pos += BX_CPIO_NEWC_HEADER_LEN;
        if (pos + namesize > archive->len) {
            bx_diag(diag, "truncated newc archive");
            return false;
        }
        entry.name = xmalloc(namesize);
        memcpy(entry.name, archive->data + pos, namesize - 1u);
        entry.name[namesize - 1u] = '\0';
        pos += namesize;
        while (pos % 4u != 0u) {
            pos++;
        }
        if (strcmp(entry.name, "TRAILER!!!") == 0) {
            bx_cpio_entry_free(&entry);
            break;
        }
        if (pos + size > archive->len) {
            bx_cpio_entry_free(&entry);
            bx_diag(diag, "truncated newc archive");
            return false;
        }
        entry.ino = (uint32_t)ino;
        entry.mode = (mode_t)mode;
        entry.uid = (uid_t)uid;
        entry.gid = (gid_t)gid;
        entry.nlink = (nlink_t)nlink;
        entry.mtime.tv_sec = (time_t)mtime;
        entry.mtime.tv_nsec = 0;
        entry.size = size;
        if (S_ISDIR(entry.mode)) {
            entry.kind = BX_CPIO_KIND_DIR;
        }
        else if (S_ISLNK(entry.mode)) {
            entry.kind = BX_CPIO_KIND_SYMLINK;
            entry.link_target = xmalloc(size + 1u);
            memcpy(entry.link_target, archive->data + pos, size);
            entry.link_target[size] = '\0';
        }
        else if (S_ISFIFO(entry.mode)) {
            entry.kind = BX_CPIO_KIND_FIFO;
        }
        else {
            entry.kind = BX_CPIO_KIND_REG;
            entry.data_len = size;
            entry.data = xmalloc(size ? size : 1u);
            memcpy(entry.data, archive->data + pos, size);
        }
        pos += size;
        while (pos % 4u != 0u) {
            pos++;
        }
        bx_cpio_entry_list_push(entries, &entry);
    }
    return true;
}

static bool bx_cpio_parse_odc_archive(const struct bx_archive_buffer* archive,
                                      struct bx_cpio_entry_list* entries,
                                      struct bx_diag_ctx* diag) {
    size_t pos = 0u;
    while (pos + BX_CPIO_ODC_HEADER_LEN <= archive->len) {
        const unsigned char* header = archive->data + pos;
        size_t ino, mode, uid, gid, nlink, mtime, namesize, size;
        struct bx_cpio_entry entry;
        memset(&entry, 0, sizeof(entry));
        if (memcmp(header, "070707", 6u) != 0) {
            bx_diag(diag, "invalid odc header");
            return false;
        }
        if (!bx_cpio_parse_octal_field(header + 12, 6u, &ino)
            || !bx_cpio_parse_octal_field(header + 18, 6u, &mode)
            || !bx_cpio_parse_octal_field(header + 24, 6u, &uid)
            || !bx_cpio_parse_octal_field(header + 30, 6u, &gid)
            || !bx_cpio_parse_octal_field(header + 36, 6u, &nlink)
            || !bx_cpio_parse_octal_field(header + 48, 11u, &mtime)
            || !bx_cpio_parse_octal_field(header + 59, 6u, &namesize)
            || !bx_cpio_parse_octal_field(header + 65, 11u, &size)) {
            bx_diag(diag, "invalid odc header");
            return false;
        }
        pos += BX_CPIO_ODC_HEADER_LEN;
        if (pos + namesize > archive->len) {
            bx_diag(diag, "truncated odc archive");
            return false;
        }
        entry.name = xmalloc(namesize);
        memcpy(entry.name, archive->data + pos, namesize - 1u);
        entry.name[namesize - 1u] = '\0';
        pos += namesize;
        if (strcmp(entry.name, "TRAILER!!!") == 0) {
            bx_cpio_entry_free(&entry);
            break;
        }
        if (pos + size > archive->len) {
            bx_cpio_entry_free(&entry);
            bx_diag(diag, "truncated odc archive");
            return false;
        }
        entry.ino = (uint32_t)ino;
        entry.mode = (mode_t)mode;
        entry.uid = (uid_t)uid;
        entry.gid = (gid_t)gid;
        entry.nlink = (nlink_t)nlink;
        entry.mtime.tv_sec = (time_t)mtime;
        entry.mtime.tv_nsec = 0;
        entry.size = size;
        if (S_ISDIR(entry.mode)) {
            entry.kind = BX_CPIO_KIND_DIR;
        }
        else if (S_ISLNK(entry.mode)) {
            entry.kind = BX_CPIO_KIND_SYMLINK;
            entry.link_target = xmalloc(size + 1u);
            memcpy(entry.link_target, archive->data + pos, size);
            entry.link_target[size] = '\0';
        }
        else if (S_ISFIFO(entry.mode)) {
            entry.kind = BX_CPIO_KIND_FIFO;
        }
        else {
            entry.kind = BX_CPIO_KIND_REG;
            entry.data_len = size;
            entry.data = xmalloc(size ? size : 1u);
            memcpy(entry.data, archive->data + pos, size);
        }
        pos += size;
        if (pos % 2u != 0u) {
            pos++;
        }
        bx_cpio_entry_list_push(entries, &entry);
    }
    return true;
}

static bool bx_cpio_detect_archive_format(const struct bx_archive_buffer* archive,
                                          enum bx_cpio_format* format_out,
                                          struct bx_diag_ctx* diag) {
    if (archive->len < 6u) {
        bx_diag(diag, "empty or truncated archive");
        return false;
    }
    if (memcmp(archive->data, "070701", 6u) == 0) {
        *format_out = BX_CPIO_FORMAT_NEWC;
        return true;
    }
    if (memcmp(archive->data, "070707", 6u) == 0) {
        *format_out = BX_CPIO_FORMAT_ODC;
        return true;
    }
    bx_diag(diag, "unrecognized cpio archive format");
    return false;
}

static bool bx_cpio_entry_selected(const struct bx_cpio_options* options,
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

static struct bx_cpio_hardlink_state* bx_cpio_get_hardlink_state(struct bx_cpio_hardlink_state_list* list,
                                                                  uint32_t ino) {
    size_t i;
    for (i = 0u; i < list->len; i++) {
        if (list->items[i].ino == ino) {
            return &list->items[i];
        }
    }
    if (list->len == list->cap) {
        size_t next_cap = list->cap ? list->cap * 2u : 8u;
        list->items = xrealloc(list->items, next_cap * sizeof(*list->items));
        list->cap = next_cap;
    }
    memset(&list->items[list->len], 0, sizeof(*list->items));
    list->items[list->len].ino = ino;
    return &list->items[list->len++];
}

static void bx_cpio_hardlink_states_free(struct bx_cpio_hardlink_state_list* list) {
    size_t i;
    for (i = 0u; i < list->len; i++) {
        size_t j;
        free(list->items[i].materialized_path);
        for (j = 0u; j < list->items[i].deferred_len; j++) {
            free(list->items[i].deferred_paths[j]);
        }
        free(list->items[i].deferred_paths);
    }
    free(list->items);
    list->items = NULL;
    list->len = 0u;
    list->cap = 0u;
}

static bool bx_cpio_defer_hardlink_path(struct bx_cpio_hardlink_state* state, const char* path) {
    if (state->deferred_len == state->deferred_cap) {
        size_t next_cap = state->deferred_cap ? state->deferred_cap * 2u : 4u;
        state->deferred_paths = xrealloc(state->deferred_paths, next_cap * sizeof(*state->deferred_paths));
        state->deferred_cap = next_cap;
    }
    state->deferred_paths[state->deferred_len++] = xstrdup(path);
    return true;
}

static int bx_cpio_absolute_name_failure(const char* name, const struct bx_diag_ctx* diag) {
    char* parent = bx_path_parent_dir_dup(name);
    fprintf(stderr, "%s: cannot make directory `%s': Read-only file system\n", diag->progname, parent);
    fprintf(stderr, "%s: %s: Cannot open: No such file or directory\n", diag->progname, name);
    free(parent);
    return 2;
}

static int bx_cpio_extract_entries(const struct bx_cpio_entry_list* entries,
                                   const struct bx_cpio_options* options,
                                   int argc,
                                   char** argv,
                                   struct bx_diag_ctx* diag) {
    struct bx_archive_pending_dirs dirs = {0};
    struct bx_cpio_hardlink_state_list hardlinks = {0};
    char list_output_buffer[8192];
    struct bx_line_writer list_writer;
    size_t i;
    int status = 0;

    if (options->list)
        bx_line_writer_init(&list_writer, STDOUT_FILENO,
                            list_output_buffer, sizeof(list_output_buffer));

    for (i = 0u; i < entries->len; i++) {
        const struct bx_cpio_entry* entry = &entries->items[i];
        char* dest_path;

        if (!bx_cpio_entry_selected(options, argc, argv, entry->name)) {
            continue;
        }
        if (options->list) {
            if (!bx_line_writer_put_line(&list_writer, entry->name)) {
                bx_diag(diag, "write error: %s", strerror(errno));
                status = 2;
                break;
            }
            continue;
        }
        if (options->to_stdout) {
            if (entry->kind == BX_CPIO_KIND_SYMLINK) {
                if (!bx_xwrite_all(STDOUT_FILENO, entry->link_target, strlen(entry->link_target))) {
                    bx_diag(diag, "write error: %s", strerror(errno));
                    status = 2;
                    break;
                }
            }
            else if (entry->kind == BX_CPIO_KIND_REG) {
                if (!bx_xwrite_all(STDOUT_FILENO, entry->data, entry->data_len)) {
                    bx_diag(diag, "write error: %s", strerror(errno));
                    status = 2;
                    break;
                }
            }
            continue;
        }
        if (entry->name[0] == '/') {
            status = bx_cpio_absolute_name_failure(entry->name, diag);
            continue;
        }
        dest_path = xstrdup(entry->name);
        if (entry->kind == BX_CPIO_KIND_DIR) {
            if (mkdir(dest_path, 0777u) != 0 && errno != EEXIST) {
                bx_diag(diag, "%s: %s", dest_path, strerror(errno));
                free(dest_path);
                status = 2;
                break;
            }
            bx_archive_pending_dirs_record(&dirs, dest_path, entry->mode & 07777u, options->preserve_mtime, entry->mtime);
        }
        else {
            struct bx_cpio_hardlink_state* state = NULL;
            if (!bx_archive_ensure_parent_dirs(dest_path, diag)) {
                free(dest_path);
                status = 2;
                break;
            }
            if (entry->kind == BX_CPIO_KIND_REG && entry->nlink > 1u) {
                state = bx_cpio_get_hardlink_state(&hardlinks, entry->ino);
            }
            if (entry->kind == BX_CPIO_KIND_REG && entry->nlink > 1u && entry->data_len == 0u) {
                if (state->materialized_path != NULL) {
                    unlink(dest_path);
                    if (link(state->materialized_path, dest_path) != 0) {
                        bx_diag(diag, "%s: %s", dest_path, strerror(errno));
                        free(dest_path);
                        status = 2;
                        break;
                    }
                }
                else {
                    bx_cpio_defer_hardlink_path(state, dest_path);
                }
            }
            else if (entry->kind == BX_CPIO_KIND_REG) {
                int fd = bx_fd_open_cloexec(dest_path, O_WRONLY | O_CREAT | O_TRUNC, entry->mode & 07777u);
                if (fd < 0) {
                    bx_diag(diag, "%s: %s", dest_path, strerror(errno));
                    free(dest_path);
                    status = 2;
                    break;
                }
                if (!bx_archive_write_regular_payload(fd, entry->data, entry->data_len, options->sparse, diag)) {
                    close(fd);
                    free(dest_path);
                    status = 2;
                    break;
                }
                if (close(fd) != 0) {
                    bx_diag(diag, "%s: %s", dest_path, strerror(errno));
                    free(dest_path);
                    status = 2;
                    break;
                }
                if (options->preserve_mtime && !bx_archive_set_path_mtime(dest_path, entry->mtime, false, diag)) {
                    free(dest_path);
                    status = 2;
                    break;
                }
                if (entry->nlink > 1u) {
                    size_t j;
                    free(state->materialized_path);
                    state->materialized_path = xstrdup(dest_path);
                    for (j = 0u; j < state->deferred_len; j++) {
                        unlink(state->deferred_paths[j]);
                        if (link(dest_path, state->deferred_paths[j]) != 0) {
                            bx_diag(diag, "%s: %s", state->deferred_paths[j], strerror(errno));
                            free(dest_path);
                            status = 2;
                            break;
                        }
                    }
                    if (status == 2) {
                        break;
                    }
                }
            }
            else if (entry->kind == BX_CPIO_KIND_SYMLINK) {
                unlink(dest_path);
                if (symlink(entry->link_target, dest_path) != 0) {
                    bx_diag(diag, "%s: %s", dest_path, strerror(errno));
                    free(dest_path);
                    status = 2;
                    break;
                }
            }
            else if (entry->kind == BX_CPIO_KIND_FIFO) {
                unlink(dest_path);
                if (mkfifo(dest_path, entry->mode & 07777u) != 0) {
                    bx_diag(diag, "%s: %s", dest_path, strerror(errno));
                    free(dest_path);
                    status = 2;
                    break;
                }
                if (options->preserve_mtime && !bx_archive_set_path_mtime(dest_path, entry->mtime, false, diag)) {
                    free(dest_path);
                    status = 2;
                    break;
                }
            }
        }
        free(dest_path);
    }

    if (status == 0 && options->list && !bx_line_writer_flush(&list_writer)) {
        bx_diag(diag, "write error: %s", strerror(errno));
        status = 2;
    }
    if (status == 0 && !bx_archive_pending_dirs_apply(&dirs, diag)) {
        status = 2;
    }
    bx_archive_pending_dirs_free(&dirs);
    bx_cpio_hardlink_states_free(&hardlinks);
    return status;
}

static int bx_cpio_pass_through(const struct bx_cpio_options* options, struct bx_diag_ctx* diag) {
    char** names = NULL;
    size_t name_count = 0u;
    struct bx_archive_fs_list files = {0};
    struct bx_cpio_hardlink_state_list hardlinks = {0};
    struct bx_archive_pending_dirs dirs = {0};
    int status = 0;
    size_t i;

    if (!bx_cpio_read_name_list(options, &names, &name_count, diag)) {
        return 2;
    }
    if (!bx_cpio_build_fs_list(&files, names, name_count, diag)) {
        bx_cpio_free_name_list(names, name_count);
        return 2;
    }

    for (i = 0u; i < files.len; i++) {
        const struct bx_archive_fs_entry* entry = &files.entries[i];
        char* dest_path = bx_path_join(options->pass_dir, entry->archive_path);
        if (S_ISDIR(entry->st.st_mode)) {
            if (mkdir(dest_path, 0777u) != 0 && errno != EEXIST) {
                bx_diag(diag, "%s: %s", dest_path, strerror(errno));
                free(dest_path);
                status = 2;
                break;
            }
            bx_archive_pending_dirs_record(&dirs, dest_path, entry->st.st_mode & 07777u, options->preserve_mtime, entry->st.st_mtim);
        }
        else {
            struct bx_cpio_hardlink_state* state = NULL;
            if (!bx_archive_ensure_parent_dirs(dest_path, diag)) {
                free(dest_path);
                status = 2;
                break;
            }
            if (S_ISREG(entry->st.st_mode) && entry->st.st_nlink > 1u) {
                state = bx_cpio_get_hardlink_state(&hardlinks, (uint32_t)entry->st.st_ino);
            }
            if (S_ISREG(entry->st.st_mode) && entry->st.st_nlink > 1u && state->materialized_path != NULL) {
                unlink(dest_path);
                if (link(state->materialized_path, dest_path) != 0) {
                    bx_diag(diag, "%s: %s", dest_path, strerror(errno));
                    free(dest_path);
                    status = 2;
                    break;
                }
            }
            else if (S_ISREG(entry->st.st_mode)) {
                struct bx_archive_buffer data = {0};
                int fd;
                if (!bx_cpio_read_file(entry->source_path, &data, diag)) {
                    free(dest_path);
                    status = 2;
                    break;
                }
                fd = bx_fd_open_cloexec(dest_path, O_WRONLY | O_CREAT | O_TRUNC, entry->st.st_mode & 07777u);
                if (fd < 0) {
                    bx_archive_buffer_free(&data);
                    bx_diag(diag, "%s: %s", dest_path, strerror(errno));
                    free(dest_path);
                    status = 2;
                    break;
                }
                if (!bx_archive_write_regular_payload(fd, data.data, data.len, false, diag)) {
                    close(fd);
                    bx_archive_buffer_free(&data);
                    free(dest_path);
                    status = 2;
                    break;
                }
                close(fd);
                bx_archive_buffer_free(&data);
                if (options->preserve_mtime && !bx_archive_set_path_mtime(dest_path, entry->st.st_mtim, false, diag)) {
                    free(dest_path);
                    status = 2;
                    break;
                }
                if (state != NULL) {
                    free(state->materialized_path);
                    state->materialized_path = xstrdup(dest_path);
                }
            }
            else if (S_ISLNK(entry->st.st_mode)) {
                unlink(dest_path);
                if (symlink(entry->link_target, dest_path) != 0) {
                    bx_diag(diag, "%s: %s", dest_path, strerror(errno));
                    free(dest_path);
                    status = 2;
                    break;
                }
            }
            else if (S_ISFIFO(entry->st.st_mode)) {
                unlink(dest_path);
                if (mkfifo(dest_path, entry->st.st_mode & 07777u) != 0) {
                    bx_diag(diag, "%s: %s", dest_path, strerror(errno));
                    free(dest_path);
                    status = 2;
                    break;
                }
            }
        }
        free(dest_path);
    }

    if (status == 0 && !bx_archive_pending_dirs_apply(&dirs, diag)) {
        status = 2;
    }
    bx_archive_pending_dirs_free(&dirs);
    bx_cpio_hardlink_states_free(&hardlinks);
    bx_archive_fs_list_free(&files);
    bx_cpio_free_name_list(names, name_count);
    return status;
}

static bool bx_cpio_parse_options(struct bx_cpio_options* options,
                                  int argc,
                                  char** argv,
                                  struct bx_diag_ctx* diag) {
    int i;
    memset(options, 0, sizeof(*options));
    options->format = BX_CPIO_FORMAT_NEWC;
    options->operand_index = argc;

    for (i = 1; i < argc; i++) {
        char* arg = argv[i];
        if (arg[0] != '-' || strcmp(arg, "-") == 0) {
            options->operand_index = i;
            break;
        }
        if (strcmp(arg, "--") == 0) {
            options->operand_index = i + 1;
            break;
        }
        if (strncmp(arg, "--", 2u) == 0) {
            const char* value = strchr(arg, '=');
            size_t name_len = value ? (size_t)(value - arg) : strlen(arg);
            if (strcmp(arg, "--quiet") == 0) {
                options->quiet = true;
            }
            else if (strcmp(arg, "--null") == 0) {
                options->null_input = true;
            }
            else if (strcmp(arg, "--to-stdout") == 0) {
                options->to_stdout = true;
            }
            else if (strcmp(arg, "--sparse") == 0) {
                options->sparse = true;
            }
            else if (strcmp(arg, "--reproducible") == 0) {
                options->reproducible = true;
            }
            else if (strncmp(arg, "--file", name_len) == 0 && name_len == 6u) {
                if (value == NULL && ++i >= argc) {
                    bx_diag(diag, "option '--file' requires an argument");
                    return false;
                }
                options->archive_path = value ? value + 1 : argv[i];
            }
            else if (strncmp(arg, "--format", name_len) == 0 && name_len == 8u) {
                if (value == NULL && ++i >= argc) {
                    bx_diag(diag, "option '--format' requires an argument");
                    return false;
                }
                value = value ? value + 1 : argv[i];
                if (strcmp(value, "newc") == 0) {
                    options->format = BX_CPIO_FORMAT_NEWC;
                }
                else if (strcmp(value, "odc") == 0) {
                    options->format = BX_CPIO_FORMAT_ODC;
                }
                else {
                    bx_diag(diag, "unsupported format '%s'", value);
                    return false;
                }
            }
            else {
                bx_diag(diag, "unrecognized option '%s'", arg);
                return false;
            }
            continue;
        }
        {
            const char* letters = arg + 1;
            size_t j;
            for (j = 0u; letters[j] != '\0'; j++) {
                char ch = letters[j];
                const char* attached = &letters[j + 1u];
                switch (ch) {
                    case 'o': options->mode = BX_CPIO_MODE_COPY_OUT; break;
                    case 'i': options->mode = BX_CPIO_MODE_COPY_IN; break;
                    case 'p': options->mode = BX_CPIO_MODE_PASS; break;
                    case 't': options->list = true; break;
                    case 'd': break;
                    case 'm': options->preserve_mtime = true; break;
                    case '0': options->null_input = true; break;
                    case 'F':
                        if (*attached != '\0') {
                            options->archive_path = attached;
                            j = strlen(letters) - 1u;
                        }
                        else if (++i < argc) {
                            options->archive_path = argv[i];
                        }
                        else {
                            bx_diag(diag, "option requires an argument -- 'F'");
                            return false;
                        }
                        goto next_arg;
                    case 'H':
                        if (*attached != '\0') {
                            if (strcmp(attached, "newc") == 0) {
                                options->format = BX_CPIO_FORMAT_NEWC;
                            }
                            else if (strcmp(attached, "odc") == 0) {
                                options->format = BX_CPIO_FORMAT_ODC;
                            }
                            else {
                                bx_diag(diag, "unsupported format '%s'", attached);
                                return false;
                            }
                            j = strlen(letters) - 1u;
                        }
                        else if (++i < argc) {
                            if (strcmp(argv[i], "newc") == 0) {
                                options->format = BX_CPIO_FORMAT_NEWC;
                            }
                            else if (strcmp(argv[i], "odc") == 0) {
                                options->format = BX_CPIO_FORMAT_ODC;
                            }
                            else {
                                bx_diag(diag, "unsupported format '%s'", argv[i]);
                                return false;
                            }
                        }
                        else {
                            bx_diag(diag, "option requires an argument -- 'H'");
                            return false;
                        }
                        goto next_arg;
                    case 'R':
                        if (*attached != '\0') {
                            if (!bx_cpio_parse_owner_spec(attached, options, diag)) {
                                return false;
                            }
                            j = strlen(letters) - 1u;
                        }
                        else if (++i < argc) {
                            if (!bx_cpio_parse_owner_spec(argv[i], options, diag)) {
                                return false;
                            }
                        }
                        else {
                            bx_diag(diag, "option requires an argument -- 'R'");
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
    }

    if (options->operand_index == argc) {
        options->operand_index = i;
    }
    if (options->mode == BX_CPIO_MODE_NONE) {
        bx_diag(diag, "must specify one of -i, -o, or -p");
        return false;
    }
    if (options->mode == BX_CPIO_MODE_PASS) {
        if (options->operand_index >= argc) {
            bx_diag(diag, "missing destination directory operand");
            return false;
        }
        options->pass_dir = argv[options->operand_index];
        options->operand_index = argc;
    }
    return true;
}

int bx_cpio_run(int argc, char** argv) {
    struct bx_cpio_options options;
    struct bx_diag_ctx diag = {
        .progname = bx_cpio_progname(argv, argc),
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };

    if (!bx_cpio_parse_options(&options, argc, argv, &diag)) {
        return 2;
    }

    if (options.mode == BX_CPIO_MODE_COPY_OUT) {
        char** names = NULL;
        size_t name_count = 0u;
        struct bx_archive_buffer archive = {0};
        int rc;
        if (!bx_cpio_read_name_list(&options, &names, &name_count, &diag)) {
            return 2;
        }
        if (!bx_cpio_build_archive(&archive, &options, names, name_count, &diag)) {
            bx_cpio_free_name_list(names, name_count);
            return 2;
        }
        rc = bx_cpio_write_archive_output(&options, &archive, &diag) ? 0 : 2;
        bx_archive_buffer_free(&archive);
        bx_cpio_free_name_list(names, name_count);
        return rc;
    }
    if (options.mode == BX_CPIO_MODE_PASS) {
        return bx_cpio_pass_through(&options, &diag);
    }
    else {
        struct bx_archive_buffer archive = {0};
        struct bx_cpio_entry_list entries = {0};
        enum bx_cpio_format input_format;
        int rc;
        if (!bx_cpio_read_archive_input(&options, &archive, &diag)) {
            return 2;
        }
        if (!bx_cpio_detect_archive_format(&archive, &input_format, &diag)) {
            bx_archive_buffer_free(&archive);
            return 2;
        }
        if (input_format == BX_CPIO_FORMAT_NEWC) {
            if (!bx_cpio_parse_newc_archive(&archive, &entries, &diag)) {
                bx_archive_buffer_free(&archive);
                return 2;
            }
        }
        else {
            if (!bx_cpio_parse_odc_archive(&archive, &entries, &diag)) {
                bx_archive_buffer_free(&archive);
                return 2;
            }
        }
        bx_archive_buffer_free(&archive);
        rc = bx_cpio_extract_entries(&entries, &options, argc, argv, &diag);
        bx_cpio_entry_list_free(&entries);
        return rc;
    }
}
