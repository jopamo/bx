#include "applets/archive/tar/tar_metadata.h"

#include <errno.h>
#include <fnmatch.h>
#include <linux/fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bx/libbx.h"

static const struct {
    unsigned int bit;
    const char* set;
    const char* clear;
    const char* alias_set;
    const char* alias_clear;
} bx_tar_flag_names[] = {
    {FS_APPEND_FL, "sappnd", "nosappnd", "sappend", "nosappend"},
    {FS_IMMUTABLE_FL, "schg", "noschg", "simmutable", "nosimmutable"},
    {FS_NODUMP_FL, "nodump", "dump", NULL, NULL},
    {FS_NOATIME_FL, "noatime", "atime", NULL, NULL},
    {FS_SYNC_FL, "sync", "nosync", NULL, NULL},
    {FS_DIRSYNC_FL, "dirsync", "nodirsync", NULL, NULL},
};

bool bx_tar_metadata_decode_flags(const char* text, unsigned int* set, unsigned int* clear) {
    *set = *clear = 0u;
    if (!text)
        return true;
    while (*text) {
        text += strspn(text, " ,\t");
        size_t length = strcspn(text, " ,\t");
        if (!length)
            break;
        bool found = false;
        for (size_t i = 0; i < sizeof(bx_tar_flag_names) / sizeof(bx_tar_flag_names[0]); i++) {
            const char* tokens[] = {bx_tar_flag_names[i].set, bx_tar_flag_names[i].clear, bx_tar_flag_names[i].alias_set, bx_tar_flag_names[i].alias_clear};
            for (size_t j = 0; j < sizeof(tokens) / sizeof(tokens[0]); j++) {
                if (tokens[j] && strlen(tokens[j]) == length && memcmp(tokens[j], text, length) == 0) {
                    if (j & 1u)
                        *clear |= bx_tar_flag_names[i].bit;
                    else
                        *set |= bx_tar_flag_names[i].bit;
                    found = true;
                    break;
                }
            }
            if (found)
                break;
        }
        if (!found) {
            errno = ENOTSUP;
            return false;
        }
        text += length;
    }
    if (*set & *clear) {
        errno = EINVAL;
        return false;
    }
    return true;
}

static bool bx_tar_metadata_matches(const struct bx_archive_name_list* masks,
                                     const char* name) {
    for (size_t i = 0; i < masks->len; i++) {
        if (fnmatch(masks->items[i], name, 0) == 0)
            return true;
    }
    return false;
}

static bool bx_tar_metadata_wanted(const struct bx_tar_metadata_options* options,
                                    const char* name, bool creating) {
    /* The text ACL representation is authoritative when explicitly enabled. */
    if (options->acls && (strcmp(name, "system.posix_acl_access") == 0
                         || strcmp(name, "system.posix_acl_default") == 0))
        return false;
    if (options->selinux && strcmp(name, "security.selinux") == 0)
        return true;
    return options->xattrs
        && (options->include.len
            ? bx_tar_metadata_matches(&options->include, name)
            : creating || strncmp(name, "user.", 5u) == 0)
        && !bx_tar_metadata_matches(&options->exclude, name);
}

static bool bx_tar_metadata_collect_filter(const char* name, const void* user) {
    const struct bx_tar_metadata_options* options = user;
    return bx_tar_metadata_wanted(options, name, true);
}

void bx_tar_metadata_select(struct bx_file_metadata* selected,
                             const struct bx_file_metadata* metadata,
                             const struct bx_tar_metadata_options* options) {
    bx_file_metadata_free(selected);
    for (size_t i = 0; i < metadata->len; i++) {
        const struct bx_file_xattr* attr = &metadata->xattrs[i];
        if (bx_tar_metadata_wanted(options, attr->name, false))
            bx_file_metadata_set(selected, attr->name, attr->value, attr->size);
    }
    selected->restore_acls = options->acls;
    if (options->acls) {
        selected->acl_access = metadata->acl_access ? xstrdup(metadata->acl_access) : NULL;
        selected->acl_default = metadata->acl_default ? xstrdup(metadata->acl_default) : NULL;
    }
}

bool bx_tar_metadata_collect(struct bx_file_metadata* metadata, int fd,
                              bool symlink, bool directory, bool numeric_ids,
                              const struct bx_tar_metadata_options* options) {
    if (!options)
        return true;
    if (!bx_file_metadata_read(metadata, fd, symlink, directory, options->acls, numeric_ids, options->xattrs || options->selinux ? bx_tar_metadata_collect_filter : NULL, options))
        return false;
    if (options->file_flags) {
        unsigned int flags;
        bool applicable;
        if (!bx_file_metadata_read_flags(fd, &flags, &applicable))
            return false;
        if (applicable) {
            char text[128];
            size_t length = 0u;
            for (size_t i = 0; i < sizeof(bx_tar_flag_names) / sizeof(bx_tar_flag_names[0]); i++) {
                const char* name = flags & bx_tar_flag_names[i].bit ? bx_tar_flag_names[i].set : bx_tar_flag_names[i].clear;
                if (i)
                    text[length++] = ',';
                size_t size = strlen(name);
                memcpy(text + length, name, size);
                length += size;
            }
            text[length] = '\0';
            free(metadata->file_flags);
            metadata->file_flags = xstrdup(text);
        }
    }
    return true;
}

bool bx_tar_metadata_parse(struct bx_file_metadata* metadata, const char* key,
                            const void* value, size_t len) {
    if (strncmp(key, "SCHILY.xattr.", 13u) == 0)
        return bx_file_metadata_set(metadata, key + 13u, value, len);
    /* Accept the legacy textual encoding, but keep one binary xattr value.
     * Both encodings replace the same slot, so archive record order wins. */
    if (strcmp(key, "RHT.security.selinux") == 0) {
        if (memchr(value, '\0', len) || len >= 65536u) {
            errno = EINVAL;
            return false;
        }
        char* context = xmalloc(len + 1u);
        memcpy(context, value, len);
        context[len] = '\0';
        bool ok = bx_file_metadata_set(metadata, "security.selinux", context, len + 1u);
        free(context);
        return ok;
    }
    char** slot = NULL;
    if (strcmp(key, "SCHILY.acl.access") == 0)
        slot = &metadata->acl_access;
    else if (strcmp(key, "SCHILY.acl.default") == 0)
        slot = &metadata->acl_default;
    else if (strcmp(key, "SCHILY.fflags") == 0)
        slot = &metadata->file_flags;
    if (slot) {
        if (memchr(value, '\0', len) || len > 65536u) {
            errno = EINVAL;
            return false;
        }
        free(*slot);
        *slot = xmalloc(len + 1u);
        memcpy(*slot, value, len);
        (*slot)[len] = '\0';
    }
    return true;
}

bool bx_tar_pax_append(struct bx_archive_buffer* pax, const char* key,
                       const void* value, size_t len) {
    size_t payload = strlen(key) + len + 3u;
    size_t total = payload + 1u;
    char prefix[32];
    for (;;) {
        int digits = snprintf(prefix, sizeof(prefix), "%zu ", total);
        size_t next = payload + (size_t)digits - 1u;
        if (next == total)
            break;
        total = next;
    }
    return bx_archive_buffer_append(pax, prefix, strlen(prefix))
        && bx_archive_buffer_append(pax, key, strlen(key))
        && bx_archive_buffer_append_byte(pax, '=')
        && bx_archive_buffer_append(pax, value, len)
        && bx_archive_buffer_append_byte(pax, '\n');
}

bool bx_tar_metadata_present(const struct bx_file_metadata* metadata) {
    return metadata && (metadata->len || metadata->acl_access || metadata->acl_default || metadata->file_flags);
}

bool bx_tar_metadata_write(struct bx_archive_buffer* pax,
                            const struct bx_file_metadata* metadata) {
    if (!metadata)
        return true;
    for (size_t i = 0; i < metadata->len; i++) {
        const struct bx_file_xattr* attr = &metadata->xattrs[i];
        char key[sizeof("SCHILY.xattr.") + 255u];
        snprintf(key, sizeof(key), "SCHILY.xattr.%s", attr->name);
        if (!bx_tar_pax_append(pax, key, attr->value, attr->size))
            return false;
    }
    return (!metadata->acl_access || bx_tar_pax_append(pax, "SCHILY.acl.access", metadata->acl_access, strlen(metadata->acl_access))) &&
           (!metadata->acl_default || bx_tar_pax_append(pax, "SCHILY.acl.default", metadata->acl_default, strlen(metadata->acl_default))) &&
           (!metadata->file_flags || bx_tar_pax_append(pax, "SCHILY.fflags", metadata->file_flags, strlen(metadata->file_flags)));
}
