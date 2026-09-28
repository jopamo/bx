#include "applets/archive/tar/tar_metadata.h"

#include <errno.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bx/libbx.h"

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
    return options->xattrs
        && (options->include.len
            ? bx_tar_metadata_matches(&options->include, name)
            : creating || strncmp(name, "user.", 5u) == 0)
        && !bx_tar_metadata_matches(&options->exclude, name);
}

static bool bx_tar_metadata_collect_filter(const char* name, const void* user) {
    const struct bx_tar_metadata_options* options = user;
    return (options->selinux && strcmp(name, "security.selinux") == 0)
        || bx_tar_metadata_wanted(options, name, true);
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
    if (options->selinux && metadata->selinux)
        selected->selinux = xstrdup(metadata->selinux);
}

bool bx_tar_metadata_collect(struct bx_file_metadata* metadata, const char* path,
                              bool symlink, bool directory, bool numeric_ids,
                              const struct bx_tar_metadata_options* options) {
    if (!options)
        return true;
    if (!bx_file_metadata_read(metadata, path, symlink, directory, options->acls,
                               numeric_ids, options->xattrs || options->selinux
                                   ? bx_tar_metadata_collect_filter : NULL,
                               options))
        return false;
    for (size_t i = 0; i < metadata->len; i++) {
        struct bx_file_xattr* attr = &metadata->xattrs[i];
        if (options->selinux && strcmp(attr->name, "security.selinux") == 0) {
            metadata->selinux = xmalloc(attr->size + 1u);
            memcpy(metadata->selinux, attr->value, attr->size);
            metadata->selinux[attr->size] = '\0';
            if (!bx_tar_metadata_wanted(options, attr->name, true)) {
                free(attr->name);
                free(attr->value);
                memmove(attr, attr + 1, (metadata->len - i - 1u) * sizeof(*attr));
                metadata->len--;
            }
            break;
        }
    }
    return true;
}

bool bx_tar_metadata_parse(struct bx_file_metadata* metadata, const char* key,
                            const void* value, size_t len) {
    if (strncmp(key, "SCHILY.xattr.", 13u) == 0)
        return bx_file_metadata_set(metadata, key + 13u, value, len);
    char** slot = NULL;
    if (strcmp(key, "SCHILY.acl.access") == 0)
        slot = &metadata->acl_access;
    else if (strcmp(key, "SCHILY.acl.default") == 0)
        slot = &metadata->acl_default;
    else if (strcmp(key, "RHT.security.selinux") == 0)
        slot = &metadata->selinux;
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
    return metadata && (metadata->len || metadata->acl_access
                         || metadata->acl_default || metadata->selinux);
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
    return (!metadata->acl_access
            || bx_tar_pax_append(pax, "SCHILY.acl.access", metadata->acl_access,
                                  strlen(metadata->acl_access)))
        && (!metadata->acl_default
            || bx_tar_pax_append(pax, "SCHILY.acl.default", metadata->acl_default,
                                  strlen(metadata->acl_default)))
        && (!metadata->selinux
            || bx_tar_pax_append(pax, "RHT.security.selinux", metadata->selinux,
                                  strlen(metadata->selinux)));
}
