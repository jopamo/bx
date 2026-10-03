#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
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
#include "applets/archive/archive_temp.h"
#include "applets/archive/cpio/cpio_backend.h"
#include "applets/archive/cpio/cpio_bounds.h"
#include "bx/libbx.h"
#include "lib/cli_common.h"
#include "lib/dir_path.h"
#include "lib/fd_ops.h"
#include "lib/id_parse.h"
#include "lib/inode_ledger.h"
#include "lib/mode_parse.h"
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
    BX_CPIO_KIND_CHAR,
    BX_CPIO_KIND_BLOCK,
};

struct bx_cpio_options {
    enum bx_cpio_mode mode;
    enum bx_cpio_format format;
    const char* archive_path;
    const char* pass_dir;
    bool list;
    bool quiet;
    bool create_dirs;
    bool format_explicit;
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
    uint64_t ino;
    uint64_t device;
    dev_t rdev;
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
    uintmax_t synthetic_ino;
    size_t total_count;
    size_t seen_count;
};

struct bx_cpio_inode_map_list {
    struct bx_cpio_inode_map* items;
    size_t len;
    size_t cap;
};

struct bx_cpio_hardlink_state {
    size_t location;
    uint64_t origin;
};

struct bx_cpio_alias {
    size_t record;
    size_t previous;
};

struct bx_cpio_hardlink_state_list {
    struct bx_cpio_hardlink_state* items;
    size_t len;
    size_t cap;
    struct bx_inode_ledger groups;
    struct bx_inode_ledger materialized;
    struct bx_cpio_alias* aliases;
    size_t alias_len;
    size_t alias_cap;
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
    maps->items[maps->len].synthetic_ino = maps->len;
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

static bool bx_cpio_parse_octal_field(const unsigned char* field, size_t len, size_t* value_out) {
    size_t value = 0u;
    size_t i;
    for (i = 0u; i < len; i++) {
        unsigned char ch = field[i];
        if (ch < '0' || ch > '7') {
            return false;
        }
        if (!bx_checked_size_mul(value, 8, &value) ||
            !bx_checked_size_add(value, (size_t)(ch - '0'), &value))
            return false;
    }
    *value_out = value;
    return true;
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
    if (!bx_id_parse_numeric(spec, (uintmax_t)((uid_t)-1) - 1u, &owner) || !bx_id_parse_numeric(colon + 1, (uintmax_t)((gid_t)-1) - 1u, &group)) {
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
    int fd = bx_fd_open_cloexec(path, O_RDONLY, 0);
    if (fd < 0) {
        bx_diag(diag, "%s: %s", path, strerror(errno));
        return false;
    }
    bx_archive_buffer_init(buffer);
    if (!bx_archive_buffer_read_all(fd, buffer, diag)) {
        close(fd);
        bx_archive_buffer_free(buffer);
        return false;
    }
    if (close(fd) != 0) {
        bx_diag(diag, "%s: %s", path, strerror(errno));
        bx_archive_buffer_free(buffer);
        return false;
    }
    return true;
}

static bool bx_cpio_emit_newc_entry(struct bx_archive_buffer* archive,
                                    const char* name,
                                    uintmax_t ino,
                                    mode_t mode,
                                    uid_t uid,
                                    gid_t gid,
                                    nlink_t nlink,
                                    struct timespec mtime,
                                    dev_t device,
                                    dev_t rdev,
                                    size_t size,
                                    const unsigned char* data) {
    unsigned char header[BX_CPIO_NEWC_HEADER_LEN + 1u];
    size_t namesize;
    size_t padded;

    if (mtime.tv_sec < 0 || !bx_checked_size_add(strlen(name), 1u, &namesize)) {
        errno = EOVERFLOW;
        return false;
    }
    const uintmax_t fields[] = {ino, mode, uid, gid, nlink, (uintmax_t)mtime.tv_sec, size, major(device), minor(device), major(rdev), minor(rdev), namesize, 0};
    if (!bx_cpio_encode_newc_header(header, fields)) {
        errno = EOVERFLOW;
        return false;
    }
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
                                   uintmax_t ino,
                                   mode_t mode,
                                   uid_t uid,
                                   gid_t gid,
                                   nlink_t nlink,
                                   struct timespec mtime,
                                   dev_t device,
                                   dev_t rdev,
                                   size_t size,
                                   const unsigned char* data) {
    unsigned char header[BX_CPIO_ODC_HEADER_LEN + 1u];
    size_t namesize;

    if (mtime.tv_sec < 0 || !bx_checked_size_add(strlen(name), 1u, &namesize)) {
        errno = EOVERFLOW;
        return false;
    }
    const uintmax_t fields[] = {device, ino, mode, uid, gid, nlink, rdev, (uintmax_t)mtime.tv_sec, namesize, size};
    if (!bx_cpio_encode_odc_header(header, fields)) {
        errno = EOVERFLOW;
        return false;
    }
    if (!bx_archive_buffer_append(archive, header, BX_CPIO_ODC_HEADER_LEN)
        || !bx_archive_buffer_append(archive, name, namesize)) {
        return false;
    }
    if (size != 0u && !bx_archive_buffer_append(archive, data, size)) {
        return false;
    }
    return true;
}

static bool bx_cpio_read_name_list(const struct bx_cpio_options* options,
                                   char*** names_out,
                                   size_t* count_out,
                                   struct bx_diag_ctx* diag) {
    struct bx_archive_name_list names = {0};

    if (!bx_archive_name_list_read_fd(STDIN_FILENO, options->null_input ? '\0' : '\n', &names, diag)) {
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

static bool bx_cpio_include_source(const struct bx_archive_fs_visit_entry* entry, void* user_data) {
    (void)user_data;
    return !S_ISSOCK(entry->st->st_mode);
}

static bool bx_cpio_build_fs_list(struct bx_archive_fs_list* list,
                                  char** names,
                                  size_t count,
                                  struct bx_diag_ctx* diag) {
    size_t i;
    for (i = 0u; i < count; i++) {
        if (!bx_archive_fs_add_path_filtered(list, names[i], names[i], false, false, bx_cpio_include_source, NULL, NULL, NULL, diag)) {
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
                                      uintmax_t ino,
                                      const struct bx_cpio_options* options,
                                      bool suppress_data,
                                      struct bx_diag_ctx* diag) {
    mode_t mode = entry->st.st_mode;
    uid_t uid = options->owner_override ? options->owner : entry->st.st_uid;
    gid_t gid = options->owner_override ? options->group : entry->st.st_gid;
    nlink_t nlink = options->reproducible && S_ISDIR(mode) ? 2u : entry->st.st_nlink;
    struct timespec mtime = entry->st.st_mtim;
    uintmax_t archive_ino = options->reproducible ? ino : entry->st.st_ino;
    dev_t device = options->reproducible ? 0 : entry->st.st_dev;
    dev_t rdev = S_ISCHR(mode) || S_ISBLK(mode) ? entry->st.st_rdev : 0;
    struct bx_archive_buffer data = {0};
    const unsigned char* payload = NULL;
    size_t size = 0u;
    uintmax_t wide_limit = options->format == BX_CPIO_FORMAT_NEWC ? UINT32_MAX : UINT64_C(077777777777);

    if (strcmp(entry->archive_path, "TRAILER!!!") == 0) {
        bx_diag(diag, "%s: reserved cpio trailer name", entry->archive_path);
        return false;
    }
    if (mtime.tv_sec < 0 || (uintmax_t)mtime.tv_sec > wide_limit || (S_ISREG(mode) && (entry->st.st_size < 0 || (uintmax_t)entry->st.st_size > wide_limit))) {
        bx_diag(diag, "%s: %s", entry->archive_path, strerror(EOVERFLOW));
        return false;
    }
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
        if (!bx_cpio_emit_newc_entry(archive, entry->archive_path, archive_ino, mode, uid, gid, nlink, mtime, device, rdev, size, payload)) {
            bx_diag(diag, "%s: %s", entry->archive_path, strerror(errno));
            bx_archive_buffer_free(&data);
            return false;
        }
    }
    else {
        if (!bx_cpio_emit_odc_entry(archive, entry->archive_path, archive_ino, mode, uid, gid, nlink, mtime, device, rdev, size, payload)) {
            bx_diag(diag, "%s: %s", entry->archive_path, strerror(errno));
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
        bx_archive_fs_list_free(&files);
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
        bx_cpio_emit_newc_entry(archive, "TRAILER!!!", 0u, 0u, 0u, 0u, 1u, zero, 0u, 0u, 0u, NULL);
        while (archive->len % 512u != 0u) {
            bx_archive_buffer_append_byte(archive, 0u);
        }
    }
    else {
        struct timespec zero = {0, 0};
        bx_cpio_emit_odc_entry(archive, "TRAILER!!!", 0u, 0u, 0u, 0u, 1u, zero, 0u, 0u, 0u, NULL);
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
    int fd;
    bx_archive_buffer_init(archive);
    if (options->archive_path == NULL) {
        fd = STDIN_FILENO;
    }
    else {
        fd = bx_fd_open_cloexec(options->archive_path, O_RDONLY, 0);
        if (fd < 0) {
            bx_diag(diag, "%s: %s", options->archive_path, strerror(errno));
            return false;
        }
    }
    if (!bx_archive_buffer_read_all(fd, archive, diag)) {
        if (options->archive_path != NULL) {
            close(fd);
        }
        return false;
    }
    if (options->archive_path != NULL && close(fd) != 0) {
        bx_diag(diag, "%s: %s", options->archive_path, strerror(errno));
        return false;
    }
    return true;
}

static bool bx_cpio_parse_payload(struct bx_cpio_entry* entry, const unsigned char* data, struct bx_diag_ctx* diag) {
    if (S_ISREG(entry->mode)) {
        entry->kind = BX_CPIO_KIND_REG;
        entry->data_len = entry->size;
        entry->data = xmalloc(entry->size ? entry->size : 1u);
        memcpy(entry->data, data, entry->size);
        return true;
    }
    if (S_ISLNK(entry->mode)) {
        if (!bx_cpio_symlink_target_valid(data, entry->size)) {
            bx_diag(diag, "invalid symlink target");
            return false;
        }
        entry->kind = BX_CPIO_KIND_SYMLINK;
        entry->link_target = xmalloc(entry->size + 1u);
        memcpy(entry->link_target, data, entry->size);
        entry->link_target[entry->size] = '\0';
        return true;
    }
    if (S_ISDIR(entry->mode))
        entry->kind = BX_CPIO_KIND_DIR;
    else if (S_ISFIFO(entry->mode))
        entry->kind = BX_CPIO_KIND_FIFO;
    else if (S_ISCHR(entry->mode))
        entry->kind = BX_CPIO_KIND_CHAR;
    else if (S_ISBLK(entry->mode))
        entry->kind = BX_CPIO_KIND_BLOCK;
    else {
        bx_diag(diag, "%s: unsupported cpio file type", entry->name);
        return false;
    }
    if (entry->size != 0) {
        bx_diag(diag, "%s: unexpected payload for cpio file type", entry->name);
        return false;
    }
    return true;
}

static bool bx_cpio_finish_stream(const struct bx_archive_buffer* archive, size_t offset, struct bx_diag_ctx* diag) {
    if (!bx_cpio_zero_padding(archive->data + offset, archive->len - offset)) {
        bx_diag(diag, "trailing data after cpio trailer");
        return false;
    }
    return true;
}

static bool bx_cpio_parse_newc_archive(const struct bx_archive_buffer* archive,
                                       struct bx_cpio_entry_list* entries,
                                       struct bx_diag_ctx* diag) {
    size_t pos = 0u;
    while (pos <= archive->len && BX_CPIO_NEWC_HEADER_LEN <= archive->len - pos) {
        const unsigned char* header = archive->data + pos;
        size_t ino, mode, uid, gid, nlink, mtime, size, namesize, devmajor, devminor, rdevmajor, rdevminor, check;
        struct bx_cpio_entry entry;
        memset(&entry, 0, sizeof(entry));
        if (memcmp(header, "070701", 6u) != 0) {
            bx_diag(diag, "invalid newc header");
            return false;
        }
        if (!bx_cpio_parse_hex_field(header + 6, 8u, &ino) || !bx_cpio_parse_hex_field(header + 14, 8u, &mode) || !bx_cpio_parse_hex_field(header + 22, 8u, &uid) ||
            !bx_cpio_parse_hex_field(header + 30, 8u, &gid) || !bx_cpio_parse_hex_field(header + 38, 8u, &nlink) || !bx_cpio_parse_hex_field(header + 46, 8u, &mtime) ||
            !bx_cpio_parse_hex_field(header + 54, 8u, &size) || !bx_cpio_parse_hex_field(header + 62, 8u, &devmajor) || !bx_cpio_parse_hex_field(header + 70, 8u, &devminor) ||
            !bx_cpio_parse_hex_field(header + 78, 8u, &rdevmajor) || !bx_cpio_parse_hex_field(header + 86, 8u, &rdevminor) || !bx_cpio_parse_hex_field(header + 94, 8u, &namesize) ||
            !bx_cpio_parse_hex_field(header + 102, 8u, &check) || check != 0) {
            bx_diag(diag, "invalid newc header");
            return false;
        }
        pos += BX_CPIO_NEWC_HEADER_LEN;
        struct bx_cpio_member_bounds bounds;
        if (!bx_cpio_member_bounds(archive->data, archive->len, pos, namesize,
                                   size, 4, 4, &bounds)) {
            bx_diag(diag, "invalid or truncated newc member");
            return false;
        }
        if (!bx_cpio_zero_padding(archive->data + pos + namesize, bounds.data_offset - (pos + namesize)) ||
            !bx_cpio_zero_padding(archive->data + bounds.data_offset + size, bounds.next_offset - (bounds.data_offset + size))) {
            bx_diag(diag, "invalid newc padding");
            return false;
        }
        entry.name = xmalloc(namesize);
        memcpy(entry.name, archive->data + pos, namesize);
        pos = bounds.data_offset;
        if (strcmp(entry.name, "TRAILER!!!") == 0) {
            bx_cpio_entry_free(&entry);
            if (size == 0)
                return bx_cpio_finish_stream(archive, bounds.next_offset, diag);
            bx_diag(diag, "invalid newc trailer");
            return false;
        }
        entry.ino = (uint32_t)ino;
        entry.device = ((uint64_t)devmajor << 32) | devminor;
        entry.mode = (mode_t)mode;
        entry.uid = (uid_t)uid;
        entry.gid = (gid_t)gid;
        entry.nlink = (nlink_t)nlink;
        entry.mtime.tv_sec = (time_t)mtime;
        entry.mtime.tv_nsec = 0;
        entry.size = size;
        if ((S_ISCHR(entry.mode) || S_ISBLK(entry.mode)) && !bx_fd_device_from_numbers(rdevmajor, rdevminor, &entry.rdev)) {
            bx_diag(diag, "%s: invalid device metadata", entry.name);
            bx_cpio_entry_free(&entry);
            return false;
        }
        if (!bx_cpio_parse_payload(&entry, archive->data + pos, diag)) {
            bx_cpio_entry_free(&entry);
            return false;
        }
        pos = bounds.next_offset;
        bx_cpio_entry_list_push(entries, &entry);
    }
    bx_diag(diag, "truncated newc archive");
    return false;
}

static bool bx_cpio_parse_odc_archive(const struct bx_archive_buffer* archive,
                                      struct bx_cpio_entry_list* entries,
                                      struct bx_diag_ctx* diag) {
    size_t pos = 0u;
    while (pos <= archive->len && BX_CPIO_ODC_HEADER_LEN <= archive->len - pos) {
        const unsigned char* header = archive->data + pos;
        size_t ino, mode, uid, gid, nlink, mtime, namesize, size, device, rdev;
        struct bx_cpio_entry entry;
        memset(&entry, 0, sizeof(entry));
        if (memcmp(header, "070707", 6u) != 0) {
            bx_diag(diag, "invalid odc header");
            return false;
        }
        if (!bx_cpio_parse_octal_field(header + 6, 6u, &device) || !bx_cpio_parse_octal_field(header + 12, 6u, &ino) || !bx_cpio_parse_octal_field(header + 18, 6u, &mode) ||
            !bx_cpio_parse_octal_field(header + 24, 6u, &uid) || !bx_cpio_parse_octal_field(header + 30, 6u, &gid) || !bx_cpio_parse_octal_field(header + 36, 6u, &nlink) ||
            !bx_cpio_parse_octal_field(header + 42, 6u, &rdev) || !bx_cpio_parse_octal_field(header + 48, 11u, &mtime) || !bx_cpio_parse_octal_field(header + 59, 6u, &namesize) ||
            !bx_cpio_parse_octal_field(header + 65, 11u, &size)) {
            bx_diag(diag, "invalid odc header");
            return false;
        }
        pos += BX_CPIO_ODC_HEADER_LEN;
        struct bx_cpio_member_bounds bounds;
        if (!bx_cpio_member_bounds(archive->data, archive->len, pos, namesize,
                                   size, 1, 1, &bounds)) {
            bx_diag(diag, "invalid or truncated odc member");
            return false;
        }
        entry.name = xmalloc(namesize);
        memcpy(entry.name, archive->data + pos, namesize);
        pos = bounds.data_offset;
        if (strcmp(entry.name, "TRAILER!!!") == 0) {
            bx_cpio_entry_free(&entry);
            if (size == 0)
                return bx_cpio_finish_stream(archive, bounds.next_offset, diag);
            bx_diag(diag, "invalid odc trailer");
            return false;
        }
        entry.ino = (uint32_t)ino;
        entry.device = device;
        entry.mode = (mode_t)mode;
        entry.uid = (uid_t)uid;
        entry.gid = (gid_t)gid;
        entry.nlink = (nlink_t)nlink;
        entry.mtime.tv_sec = (time_t)mtime;
        entry.mtime.tv_nsec = 0;
        entry.size = size;
        if ((S_ISCHR(entry.mode) || S_ISBLK(entry.mode)) && !bx_fd_device_from_numbers(major((dev_t)rdev), minor((dev_t)rdev), &entry.rdev)) {
            bx_diag(diag, "%s: invalid device metadata", entry.name);
            bx_cpio_entry_free(&entry);
            return false;
        }
        if (!bx_cpio_parse_payload(&entry, archive->data + pos, diag)) {
            bx_cpio_entry_free(&entry);
            return false;
        }
        pos = bounds.next_offset;
        bx_cpio_entry_list_push(entries, &entry);
    }
    bx_diag(diag, "truncated odc archive");
    return false;
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

static struct bx_cpio_hardlink_state* bx_cpio_get_hardlink_state(struct bx_cpio_hardlink_state_list* list, uint64_t device, uint64_t ino) {
    struct stat key = {.st_dev = (dev_t)device, .st_ino = (ino_t)ino, .st_mode = S_IFREG};
    if ((uint64_t)key.st_dev != device || (uint64_t)key.st_ino != ino) {
        errno = EOVERFLOW;
        return NULL;
    }
    uint64_t index;
    if (bx_inode_ledger_lookup(&list->groups, &key, &index, NULL))
        return &list->items[index - 1u];
    if (!bx_inode_ledger_record(&list->groups, &key, list->len + 1u, 0u, BX_ARCHIVE_PENDING_METADATA_LIMIT))
        return NULL;
    if (list->len == list->cap) {
        size_t next = list->cap ? list->cap * 2u : 16u;
        list->items = xrealloc(list->items, next * sizeof(*list->items));
        list->cap = next;
    }
    list->items[list->len] = (struct bx_cpio_hardlink_state){.location = SIZE_MAX};
    return &list->items[list->len++];
}

static void bx_cpio_hardlink_states_free(struct bx_cpio_hardlink_state_list* list) {
    free(list->items);
    free(list->aliases);
    bx_inode_ledger_free(&list->groups);
    bx_inode_ledger_free(&list->materialized);
    *list = (struct bx_cpio_hardlink_state_list){0};
}

static int bx_cpio_open_live_alias(struct bx_cpio_hardlink_state_list* list, struct bx_cpio_hardlink_state* state, struct bx_archive_pending_metadata* pending, int root_fd, bool writable) {
    while (state->location != SIZE_MAX) {
        if (bx_archive_temp_pending_signal()) {
            errno = EINTR;
            return -1;
        }
        const struct bx_cpio_alias* alias = &list->aliases[state->location];
        const struct bx_archive_pending_metadata_entry* record = &pending->entries[alias->record];
        int fd = bx_archive_pending_metadata_open_fd(root_fd, pending->path_policy, record, writable);
        if (fd >= 0) {
            struct stat status;
            uint64_t origin;
            if (fstat(fd, &status) != 0) {
                int error = errno;
                close(fd);
                errno = error;
                return -1;
            }
            if (bx_inode_ledger_lookup(&list->materialized, &status, NULL, &origin) && origin == state->origin)
                return fd;
            close(fd);
            fd = -2;
        }
        if (fd != -2)
            return -1;
        state->location = alias->previous;
    }
    errno = ESTALE;
    return -1;
}

static bool bx_cpio_record_regular(struct bx_cpio_hardlink_state_list* list,
                                   struct bx_cpio_hardlink_state* state,
                                   struct bx_archive_pending_metadata* pending,
                                   int fd,
                                   const char* path,
                                   const struct bx_file_restore* restore) {
    struct stat status;
    if (fstat(fd, &status) != 0)
        return false;
    uint64_t order = pending->len + 1u;
    uint64_t origin = state && state->origin ? state->origin : order;
    if (!bx_inode_ledger_record(&list->materialized, &status, order, origin, BX_ARCHIVE_PENDING_METADATA_LIMIT))
        return false;
    if (state && list->alias_len >= BX_ARCHIVE_PENDING_METADATA_LIMIT) {
        errno = E2BIG;
        return false;
    }
    size_t record = pending->len;
    if (!bx_archive_pending_metadata_record_fd(pending, fd, path, 0, restore, order, origin))
        return false;
    if (state) {
        if (list->alias_len == list->alias_cap) {
            size_t next = list->alias_cap ? list->alias_cap * 2u : 16u;
            list->aliases = xrealloc(list->aliases, next * sizeof(*list->aliases));
            list->alias_cap = next;
        }
        list->aliases[list->alias_len] = (struct bx_cpio_alias){.record = record, .previous = state->location};
        state->location = list->alias_len++;
        state->origin = origin;
    }
    return true;
}

/* Link privately, then verify against the transiently pinned live alias. */
static bool bx_cpio_link_materialized(int root_fd, const struct bx_archive_pending_metadata_entry* source, int source_fd, int parent, const char* leaf) {
    char* source_leaf = NULL;
    int source_parent = bx_dir_path_open_destination_parent(root_fd, source->path, BX_DIR_PATH_NO_MOUNT_CROSSING, false, 0, &source_leaf);
    if (source_parent < 0)
        return false;
    struct stat expected, linked;
    bool ok = false;
    if (fstat(source_fd, &expected) != 0)
        goto done;
    struct bx_fd_staged_file stage = BX_FD_STAGED_FILE_INIT;
    if (bx_fd_staged_link_begin(&stage, parent, leaf, source_parent, source_leaf) == 0) {
        if (bx_fd_fstat_expected(stage.fd, &expected, &linked) == 0) {
            if (bx_archive_temp_pending_signal())
                errno = EINTR;
            else
                ok = bx_fd_staged_file_publish(&stage, leaf) == 0;
        }
    }
    bx_fd_staged_file_discard(&stage);
done: {
    int error = errno;
    close(source_parent);
    free(source_leaf);
    errno = error;
    return ok;
}
}

static bool bx_cpio_extract_one(const struct bx_cpio_entry* entry,
                                const struct bx_cpio_options* options,
                                int root_fd,
                                struct bx_archive_pending_metadata* dirs,
                                struct bx_cpio_hardlink_state_list* hardlinks,
                                struct bx_cpio_hardlink_state* state,
                                struct bx_diag_ctx* diag) {
    char* leaf = NULL;
    int parent = -1;
    int fd = -1;
    struct bx_fd_staged_file stage = BX_FD_STAGED_FILE_INIT;
    bool ok = false;
    struct bx_file_restore restore = {
        .mode = entry->mode,
        .mtime = entry->mtime,
        .set_mtime = options->preserve_mtime,
        .set_owner = options->owner_override,
        .set_group = options->owner_override,
        .uid = options->owner,
        .gid = options->group,
    };
    if (bx_archive_temp_pending_signal()) {
        errno = EINTR;
        goto fail;
    }
    parent = bx_dir_path_open_destination_parent(root_fd, entry->name, BX_DIR_PATH_NO_MOUNT_CROSSING, options->create_dirs, 0777, &leaf);
    if (parent < 0)
        goto fail;
    if (entry->kind == BX_CPIO_KIND_DIR) {
        restore.set_mode = true;
        if (strcmp(leaf, ".") == 0) {
            fd = bx_fd_openat_cloexec(parent, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW, 0);
        }
        else {
            if (bx_fd_mkdirat_child(parent, leaf, 0777) != 0 && errno != EEXIST)
                goto fail;
            fd = bx_fd_openat_child_nofollow(parent, leaf, O_RDONLY | O_DIRECTORY, 0);
        }
        if (fd < 0 || !bx_archive_pending_metadata_record_fd(dirs, fd, entry->name, 0, &restore, dirs->len, 0u))
            goto fail;
    }
    else {
        if (!bx_fd_at_name_is_child(leaf)) {
            errno = EINVAL;
            goto fail;
        }
        if (state && state->origin) {
            fd = bx_cpio_open_live_alias(hardlinks, state, dirs, root_fd, entry->data_len > 0);
            if (fd < 0)
                goto fail;
            /* Published aliases share later payload changes. Keep the inode
             * extractor-owned and writable until every group header ends. */
            if (entry->data_len > 0) {
                if (bx_fd_ftruncate(fd, 0) != 0 || bx_fd_lseek(fd, 0, SEEK_SET) < 0)
                    goto fail;
                if (!bx_archive_write_regular_payload(fd, entry->data, entry->data_len, options->sparse, diag))
                    goto done;
            }
            restore.mode = entry->mode & ~bx_mode_current_umask();
            restore.set_mode = true;
            if (bx_archive_temp_pending_signal()) {
                errno = EINTR;
                goto fail;
            }
            const struct bx_cpio_alias* alias = &hardlinks->aliases[state->location];
            if (!bx_cpio_link_materialized(root_fd, &dirs->entries[alias->record], fd, parent, leaf))
                goto fail;
            if (!bx_cpio_record_regular(hardlinks, state, dirs, fd, entry->name, &restore))
                goto fail;
        }
        else {
            if (entry->kind == BX_CPIO_KIND_REG) {
                if (bx_fd_staged_file_begin(&stage, parent, leaf, entry->mode & 07777u) != 0)
                    goto fail;
                restore.mode = stage.mode;
                restore.set_mode = true;
                if (!bx_archive_write_regular_payload(stage.fd, entry->data, entry->data_len, options->sparse, diag))
                    goto done;
                if (!bx_archive_prepare_regular_fd(&restore, stage.fd, entry->name, diag))
                    goto done;
                if (!bx_cpio_record_regular(hardlinks, state, dirs, stage.fd, entry->name, &restore))
                    goto fail;
                if (bx_archive_temp_pending_signal()) {
                    errno = EINTR;
                    goto fail;
                }
                if (bx_fd_staged_file_publish(&stage, leaf) != 0)
                    goto fail;
            }
            else if (entry->kind == BX_CPIO_KIND_SYMLINK) {
                if (bx_fd_staged_symlink_begin(&stage, parent, leaf, entry->link_target) != 0)
                    goto fail;
            }
            else if (entry->kind == BX_CPIO_KIND_FIFO || entry->kind == BX_CPIO_KIND_CHAR || entry->kind == BX_CPIO_KIND_BLOCK) {
                if (bx_fd_staged_node_begin(&stage, parent, leaf, entry->mode, entry->rdev) != 0)
                    goto fail;
            }
        }
        if (entry->kind != BX_CPIO_KIND_REG) {
            struct stat status;
            if (fstat(stage.fd, &status) != 0)
                goto fail;
            if ((status.st_mode & S_IFMT) != (entry->mode & S_IFMT) || ((entry->kind == BX_CPIO_KIND_CHAR || entry->kind == BX_CPIO_KIND_BLOCK) && status.st_rdev != entry->rdev)) {
                errno = ESTALE;
                goto fail;
            }
            if (entry->kind != BX_CPIO_KIND_SYMLINK && (restore.set_owner || restore.set_group)) {
                restore.mode = stage.mode;
                restore.set_mode = true;
            }
            if (!bx_archive_restore_leaf(&restore, stage.parent_fd, stage.name, &status, entry->name, diag))
                goto done;
        }
        if (entry->kind != BX_CPIO_KIND_REG) {
            if (bx_archive_temp_pending_signal()) {
                errno = EINTR;
                goto fail;
            }
            if (bx_fd_staged_file_publish(&stage, leaf) != 0)
                goto fail;
        }
    }
    ok = true;
    goto done;
fail:
    bx_diag(diag, "%s: %s", entry->name, strerror(errno));
done:
    bx_fd_staged_file_discard(&stage);
    if (!bx_fd_close(&fd, entry->name, diag))
        ok = false;
    bx_fd_cleanup(&parent);
    free(leaf);
    return ok;
}

static int bx_cpio_extract_entries(const struct bx_cpio_entry_list* entries, const struct bx_cpio_options* options, int argc, char** argv, struct bx_diag_ctx* diag) {
    struct bx_archive_pending_metadata dirs = {.path_policy = BX_DIR_PATH_NO_MOUNT_CROSSING};
    struct bx_cpio_hardlink_state_list hardlinks = {0};
    char list_output_buffer[8192];
    struct bx_line_writer list_writer;
    int status = 0;
    int root_fd = -1;
    if (options->list) {
        bx_line_writer_init(&list_writer, STDOUT_FILENO, list_output_buffer, sizeof(list_output_buffer));
    }
    else if (!options->to_stdout) {
        root_fd = bx_fd_open_cloexec(".", O_RDONLY | O_DIRECTORY, 0);
        if (root_fd < 0) {
            bx_diag(diag, ".: %s", strerror(errno));
            return 2;
        }
    }
    for (size_t i = 0; i < entries->len; i++) {
        const struct bx_cpio_entry* entry = &entries->items[i];
        if (!bx_cpio_entry_selected(options, argc, argv, entry->name))
            continue;
        bool ok = true;
        if (options->list) {
            ok = bx_line_writer_put_line(&list_writer, entry->name);
        }
        else if (options->to_stdout) {
            if (entry->kind == BX_CPIO_KIND_SYMLINK)
                ok = bx_xwrite_all(STDOUT_FILENO, entry->link_target, strlen(entry->link_target));
            else if (entry->kind == BX_CPIO_KIND_REG)
                ok = bx_xwrite_all(STDOUT_FILENO, entry->data, entry->data_len);
        }
        else {
            struct bx_cpio_hardlink_state* state = NULL;
            if (entry->kind == BX_CPIO_KIND_REG && entry->nlink > 1) {
                state = bx_cpio_get_hardlink_state(&hardlinks, entry->device, entry->ino);
                if (!state) {
                    bx_diag(diag, "%s: %s", entry->name, strerror(errno));
                    status = 2;
                    break;
                }
            }
            if (!bx_cpio_extract_one(entry, options, root_fd, &dirs, &hardlinks, state, diag)) {
                status = 2;
                break;
            }
        }
        if (!ok) {
            bx_diag(diag, "write error: %s", strerror(errno));
            status = 2;
            break;
        }
    }
    if (status == 0 && options->list && !bx_line_writer_flush(&list_writer)) {
        bx_diag(diag, "write error: %s", strerror(errno));
        status = 2;
    }
    if (status == 0 && !bx_archive_pending_metadata_apply(&dirs, root_fd, diag))
        status = 2;
    bx_archive_pending_metadata_free(&dirs);
    bx_cpio_hardlink_states_free(&hardlinks);
    bx_fd_cleanup(&root_fd);
    return status;
}

static int bx_cpio_pass_through(const struct bx_cpio_options* options, struct bx_diag_ctx* diag, uintmax_t* copied_bytes) {
    char** names = NULL;
    size_t name_count = 0;
    struct bx_archive_fs_list files = {0};
    struct bx_cpio_hardlink_state_list hardlinks = {0};
    struct bx_archive_pending_metadata dirs = {.path_policy = BX_DIR_PATH_NO_MOUNT_CROSSING};
    int status = 2;
    int root_fd = -1;
    if (!bx_cpio_read_name_list(options, &names, &name_count, diag))
        return 2;
    if (!bx_cpio_build_fs_list(&files, names, name_count, diag))
        goto done;
    root_fd = bx_fd_open_cloexec(options->pass_dir, O_RDONLY | O_DIRECTORY, 0);
    if (root_fd < 0) {
        bx_diag(diag, "%s: %s", options->pass_dir, strerror(errno));
        goto done;
    }
    status = 0;
    for (size_t i = 0; i < files.len; i++) {
        const struct bx_archive_fs_entry* file = &files.entries[i];
        struct bx_cpio_entry entry = {
            .name = file->archive_path,
            .mode = file->st.st_mode,
            .nlink = file->st.st_nlink,
            .mtime = file->st.st_mtim,
            .ino = file->st.st_ino,
            .device = file->st.st_dev,
            .rdev = file->st.st_rdev,
            .link_target = file->link_target,
        };
        struct bx_archive_buffer data = {0};
        struct bx_cpio_hardlink_state* state = NULL;
        if (S_ISDIR(file->st.st_mode)) {
            entry.kind = BX_CPIO_KIND_DIR;
        }
        else if (S_ISLNK(file->st.st_mode)) {
            entry.kind = BX_CPIO_KIND_SYMLINK;
        }
        else if (S_ISFIFO(file->st.st_mode)) {
            entry.kind = BX_CPIO_KIND_FIFO;
        }
        else if (S_ISCHR(file->st.st_mode)) {
            entry.kind = BX_CPIO_KIND_CHAR;
        }
        else if (S_ISBLK(file->st.st_mode)) {
            entry.kind = BX_CPIO_KIND_BLOCK;
        }
        else if (S_ISREG(file->st.st_mode)) {
            entry.kind = BX_CPIO_KIND_REG;
            state = entry.nlink > 1 ? bx_cpio_get_hardlink_state(&hardlinks, entry.device, entry.ino) : NULL;
            if (entry.nlink > 1 && !state) {
                status = 2;
                bx_diag(diag, "%s: %s", entry.name, strerror(errno));
                break;
            }
            if ((state == NULL || !state->origin) && !bx_cpio_read_file(file->source_path, &data, diag)) {
                bx_archive_buffer_free(&data);
                status = 2;
                break;
            }
            entry.data = data.data;
            entry.data_len = data.len;
        }
        else {
            continue;
        }
        size_t bytes = entry.kind == BX_CPIO_KIND_SYMLINK ? strlen(entry.link_target) : entry.data_len;
        if (UINTMAX_MAX - *copied_bytes < bytes) {
            bx_diag(diag, "copied byte count: %s", strerror(EOVERFLOW));
            bx_archive_buffer_free(&data);
            status = 2;
            break;
        }
        bool ok = bx_cpio_extract_one(&entry, options, root_fd, &dirs, &hardlinks, state, diag);
        if (ok)
            *copied_bytes += bytes;
        bx_archive_buffer_free(&data);
        if (!ok) {
            status = 2;
            break;
        }
    }
    if (status == 0 && !bx_archive_pending_metadata_apply(&dirs, root_fd, diag))
        status = 2;
done:
    bx_archive_pending_metadata_free(&dirs);
    bx_cpio_hardlink_states_free(&hardlinks);
    bx_archive_fs_list_free(&files);
    bx_cpio_free_name_list(names, name_count);
    bx_fd_cleanup(&root_fd);
    return status;
}

static bool bx_cpio_select_format(struct bx_cpio_options* options, const char* value, struct bx_diag_ctx* diag) {
    if (strcmp(value, "newc") == 0)
        options->format = BX_CPIO_FORMAT_NEWC;
    else if (strcmp(value, "odc") == 0)
        options->format = BX_CPIO_FORMAT_ODC;
    else {
        bx_diag(diag, "unsupported format '%s'", value);
        return false;
    }
    options->format_explicit = true;
    return true;
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
                if (!bx_cpio_select_format(options, value, diag))
                    return false;
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
                    case 'o':
                    case 'i':
                    case 'p': {
                        enum bx_cpio_mode mode = ch == 'o' ? BX_CPIO_MODE_COPY_OUT : ch == 'i' ? BX_CPIO_MODE_COPY_IN : BX_CPIO_MODE_PASS;
                        if (options->mode != BX_CPIO_MODE_NONE && options->mode != mode) {
                            bx_diag(diag, "conflicting operation modes");
                            return false;
                        }
                        options->mode = mode;
                        break;
                    }
                    case 't': options->list = true; break;
                    case 'd':
                        options->create_dirs = true;
                        break;
                    case 'm': options->preserve_mtime = true; break;
                    case '0': options->null_input = true; break;
                    case 'F':
                        if (*attached != '\0') {
                            options->archive_path = attached;
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
                            if (!bx_cpio_select_format(options, attached, diag))
                                return false;
                        }
                        else if (++i < argc) {
                            if (!bx_cpio_select_format(options, argv[i], diag))
                                return false;
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
    const char* invalid = NULL;
    bool filesystem_copy = options->mode == BX_CPIO_MODE_PASS || (options->mode == BX_CPIO_MODE_COPY_IN && !options->list && !options->to_stdout);
    if (options->list && options->mode != BX_CPIO_MODE_COPY_IN)
        invalid = "-t requires -i";
    else if (options->to_stdout && (options->mode != BX_CPIO_MODE_COPY_IN || options->list))
        invalid = "--to-stdout requires extraction without -t";
    else if (options->format_explicit && options->mode != BX_CPIO_MODE_COPY_OUT)
        invalid = "-H/--format requires -o; input format is detected automatically";
    else if (options->archive_path && options->mode == BX_CPIO_MODE_PASS)
        invalid = "-F/--file is not supported with -p";
    else if (options->reproducible && options->mode != BX_CPIO_MODE_COPY_OUT)
        invalid = "--reproducible requires -o";
    else if (options->null_input && options->mode == BX_CPIO_MODE_COPY_IN)
        invalid = "-0/--null requires -o or -p";
    else if (options->create_dirs && !filesystem_copy)
        invalid = "-d requires filesystem extraction or -p";
    else if (options->preserve_mtime && !filesystem_copy)
        invalid = "-m requires filesystem extraction or -p";
    else if (options->sparse && !filesystem_copy)
        invalid = "--sparse requires filesystem extraction or -p";
    else if (options->owner_override && !filesystem_copy && options->mode != BX_CPIO_MODE_COPY_OUT)
        invalid = "-R requires -o, filesystem extraction, or -p";
    if (invalid) {
        bx_diag(diag, "%s", invalid);
        return false;
    }
    if (options->mode == BX_CPIO_MODE_COPY_OUT && options->operand_index < argc) {
        bx_cli_diag_extra_operand(diag, argv[options->operand_index]);
        return false;
    }
    if (options->mode == BX_CPIO_MODE_PASS) {
        if (options->operand_index >= argc) {
            bx_diag(diag, "missing destination directory operand");
            return false;
        }
        if (options->operand_index + 1 < argc) {
            bx_cli_diag_extra_operand(diag, argv[options->operand_index + 1]);
            return false;
        }
        options->pass_dir = argv[options->operand_index];
        options->operand_index = argc;
    }
    return true;
}

static int bx_cpio_execute(struct bx_cpio_options options, int argc, char** argv, struct bx_diag_ctx diag, uintmax_t* copied_bytes) {
    if (options.mode == BX_CPIO_MODE_COPY_OUT) {
        char** names = NULL;
        size_t name_count = 0u;
        struct bx_archive_buffer archive = {0};
        int rc;
        if (!bx_cpio_read_name_list(&options, &names, &name_count, &diag)) {
            return 2;
        }
        if (!bx_cpio_build_archive(&archive, &options, names, name_count, &diag)) {
            bx_archive_buffer_free(&archive);
            bx_cpio_free_name_list(names, name_count);
            return 2;
        }
        rc = bx_cpio_write_archive_output(&options, &archive, &diag) ? 0 : 2;
        *copied_bytes = archive.len;
        bx_archive_buffer_free(&archive);
        bx_cpio_free_name_list(names, name_count);
        return rc;
    }
    if (options.mode == BX_CPIO_MODE_PASS) {
        return bx_cpio_pass_through(&options, &diag, copied_bytes);
    }
    else {
        struct bx_archive_buffer archive = {0};
        struct bx_cpio_entry_list entries = {0};
        enum bx_cpio_format input_format;
        int rc;
        if (!bx_cpio_read_archive_input(&options, &archive, &diag)) {
            bx_archive_buffer_free(&archive);
            return 2;
        }
        *copied_bytes = archive.len;
        if (!bx_cpio_detect_archive_format(&archive, &input_format, &diag)) {
            bx_archive_buffer_free(&archive);
            return 2;
        }
        if (input_format == BX_CPIO_FORMAT_NEWC) {
            if (!bx_cpio_parse_newc_archive(&archive, &entries, &diag)) {
                bx_cpio_entry_list_free(&entries);
                bx_archive_buffer_free(&archive);
                return 2;
            }
        }
        else {
            if (!bx_cpio_parse_odc_archive(&archive, &entries, &diag)) {
                bx_cpio_entry_list_free(&entries);
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

int bx_cpio_run(int argc, char** argv) {
    struct bx_cpio_options options;
    struct bx_diag_ctx diag = {.progname = bx_cpio_progname(argv, argc)};
    uintmax_t copied_bytes = 0;
    if (!bx_cpio_parse_options(&options, argc, argv, &diag))
        return 2;
    int rc = bx_cpio_execute(options, argc, argv, diag, &copied_bytes);
    if (rc == 0 && !options.quiet) {
        uintmax_t blocks = copied_bytes / 512u + (copied_bytes % 512u != 0);
        if (fprintf(stderr, "%" PRIuMAX " block%s\n", blocks, blocks == 1 ? "" : "s") < 0 || fflush(stderr) != 0)
            return 2;
    }
    return rc;
}
