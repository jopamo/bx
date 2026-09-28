#define _GNU_SOURCE
#include "lib/file_metadata.h"

#include <acl/libacl.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/acl.h>
#include <sys/xattr.h>

#include "bx/libbx.h"

void bx_file_metadata_free(struct bx_file_metadata* metadata) {
    for (size_t i = 0; i < metadata->len; i++) {
        free(metadata->xattrs[i].name);
        free(metadata->xattrs[i].value);
    }
    free(metadata->xattrs);
    free(metadata->acl_access);
    free(metadata->acl_default);
    memset(metadata, 0, sizeof(*metadata));
}

bool bx_file_metadata_set(struct bx_file_metadata* metadata, const char* name,
                          const void* value, size_t size) {
    if (!*name || strlen(name) > 255u || size > 65536u) {
        errno = EINVAL;
        return false;
    }
    size_t i;
    for (i = 0; i < metadata->len; i++) {
        if (strcmp(metadata->xattrs[i].name, name) == 0)
            break;
    }
    if (i == metadata->len) {
        metadata->xattrs = xrealloc(metadata->xattrs,
                                    (metadata->len + 1u) * sizeof(*metadata->xattrs));
        metadata->xattrs[i] = (struct bx_file_xattr){.name = xstrdup(name)};
        metadata->len++;
    }
    free(metadata->xattrs[i].value);
    metadata->xattrs[i].value = xmalloc(size ? size : 1u);
    if (size)
        memcpy(metadata->xattrs[i].value, value, size);
    metadata->xattrs[i].size = size;
    return true;
}

void bx_file_metadata_copy(struct bx_file_metadata* dest,
                           const struct bx_file_metadata* source) {
    if (dest == source)
        return;
    bx_file_metadata_free(dest);
    for (size_t i = 0; i < source->len; i++) {
        const struct bx_file_xattr* attr = &source->xattrs[i];
        bx_file_metadata_set(dest, attr->name, attr->value, attr->size);
    }
    dest->acl_access = source->acl_access ? xstrdup(source->acl_access) : NULL;
    dest->acl_default = source->acl_default ? xstrdup(source->acl_default) : NULL;
    dest->restore_acls = source->restore_acls;
}

static bool bx_metadata_unsupported(int error) {
    return error == ENOTSUP || error == ENOSYS;
}

/* libacl has no default-ACL fd getter. Decode Linux's version-2 xattr into
 * libacl entries so both access and default ACLs use the same text format. */
static acl_t bx_metadata_default_acl_fd(int fd) {
    unsigned char* data = xmalloc(65536u);
    ssize_t size = fgetxattr(fd, "system.posix_acl_default", data, 65536u);
    acl_t acl = NULL;
    if (size < 0) {
        if (errno == ENODATA)
            acl = acl_init(0);
        goto out;
    }
    uint32_t version;
    if (size < 4 || (size - 4) % 8 != 0)
        goto invalid;
    memcpy(&version, data, sizeof(version));
    if (le32toh(version) != 2)
        goto invalid;
    acl = acl_init((int)((size - 4) / 8));
    if (!acl)
        goto out;
    for (ssize_t pos = 4; pos < size; pos += 8) {
        uint16_t tag, perms;
        uint32_t id;
        memcpy(&tag, data + pos, sizeof(tag));
        memcpy(&perms, data + pos + 2, sizeof(perms));
        memcpy(&id, data + pos + 4, sizeof(id));
        tag = le16toh(tag);
        perms = le16toh(perms);
        id = le32toh(id);
        if ((perms & ~7u) || (tag != ACL_USER && tag != ACL_GROUP && id != UINT32_MAX))
            goto invalid;
        acl_entry_t entry;
        acl_permset_t set;
        if (acl_create_entry(&acl, &entry) != 0 || acl_set_tag_type(entry, tag) != 0
            || acl_get_permset(entry, &set) != 0 || acl_clear_perms(set) != 0
            || ((perms & ACL_READ) && acl_add_perm(set, ACL_READ) != 0)
            || ((perms & ACL_WRITE) && acl_add_perm(set, ACL_WRITE) != 0)
            || ((perms & ACL_EXECUTE) && acl_add_perm(set, ACL_EXECUTE) != 0))
            goto fail;
        if (tag == ACL_USER) {
            uid_t uid = (uid_t)id;
            if (id == UINT32_MAX || (uint32_t)uid != id)
                goto invalid;
            if (acl_set_qualifier(entry, &uid) != 0)
                goto fail;
        } else if (tag == ACL_GROUP) {
            gid_t gid = (gid_t)id;
            if (id == UINT32_MAX || (uint32_t)gid != id)
                goto invalid;
            if (acl_set_qualifier(entry, &gid) != 0)
                goto fail;
        }
    }
    if (size > 4 && acl_valid(acl) != 0)
        goto fail;
    goto out;
invalid:
    errno = EINVAL;
fail: {
    int error = errno;
    if (acl)
        acl_free(acl);
    acl = NULL;
    errno = error;
}
out: {
    int error = errno;
    free(data);
    errno = error;
    return acl;
}
}

static bool bx_metadata_read_acl(char** text, int fd, const char* path, acl_type_t type,
                                  bool numeric_ids) {
    acl_t acl = fd < 0 ? acl_get_file(path, type)
        : type == ACL_TYPE_DEFAULT ? bx_metadata_default_acl_fd(fd) : acl_get_fd(fd);
    if (!acl)
        return bx_metadata_unsupported(errno);
    char* value = acl_to_any_text(acl, NULL, ',', numeric_ids ? TEXT_NUMERIC_IDS : 0);
    acl_free(acl);
    if (!value)
        return false;
    *text = xstrdup(value);
    acl_free(value);
    return true;
}

bool bx_file_metadata_read(struct bx_file_metadata* metadata, int fd, const char* path,
                           bool symlink, bool directory, bool acls, bool numeric_ids,
                           bx_file_xattr_filter filter, const void* user) {
    if (fd >= 0 && symlink) {
        errno = EINVAL;
        return false;
    }
    if (acls && !symlink) {
        if (!bx_metadata_read_acl(&metadata->acl_access, fd, path, ACL_TYPE_ACCESS, numeric_ids)
            || (directory && !bx_metadata_read_acl(&metadata->acl_default, fd, path,
                                                    ACL_TYPE_DEFAULT, numeric_ids)))
            return false;
    }
    if (!filter)
        return true;
    /* Linux bounds the name list and each value at 64 KiB. Read each in one
     * syscall, avoiding a size-query/read race when another writer changes it. */
    char* names = xmalloc(65536u);
    unsigned char* value = xmalloc(65536u);
    ssize_t size = fd >= 0 ? flistxattr(fd, names, 65536u)
        : symlink ? llistxattr(path, names, 65536u)
                           : listxattr(path, names, 65536u);
    bool ok = size >= 0 || bx_metadata_unsupported(errno);
    for (ssize_t pos = 0; ok && pos < size;) {
        const char* name = names + pos;
        pos += (ssize_t)strlen(name) + 1;
        if (!filter(name, user))
            continue;
        ssize_t len = fd >= 0 ? fgetxattr(fd, name, value, 65536u)
            : symlink ? lgetxattr(path, name, value, 65536u)
                              : getxattr(path, name, value, 65536u);
        if (len < 0) {
            if (errno != ENODATA)
                ok = false;
            continue;
        }
        ok = bx_file_metadata_set(metadata, name, value, (size_t)len);
    }
    int error = errno;
    free(value);
    free(names);
    errno = error;
    return ok;
}

/* POSIX draft ACL text can carry a fourth, numeric-ID field (star). Linux
 * libacl accepts three fields; retain the named identity and discard extras. */
static acl_t bx_metadata_acl_from_text(const char* text) {
    char* copy = xstrdup(text);
    char* out = copy;
    unsigned int colons = 0;
    for (const char* in = text; *in; in++) {
        if (*in == ',' || *in == '\n') {
            colons = 0;
            *out++ = *in;
        }
        else if (*in == ':' && ++colons >= 3u) {
            continue;
        }
        else if (colons < 3u)
            *out++ = *in;
    }
    *out = '\0';
    acl_t acl = acl_from_text(copy);
    free(copy);
    if (acl && acl_valid(acl) != 0) {
        int error = errno;
        acl_free(acl);
        errno = error;
        return NULL;
    }
    return acl;
}

static bool bx_metadata_apply_xattr(const struct bx_file_xattr* attr, int fd,
                                     const char* path, bool path_fd) {
    int rc = path_fd ? setxattr(path, attr->name, attr->value, attr->size, 0)
        : fd >= 0 ? fsetxattr(fd, attr->name, attr->value, attr->size, 0)
                   : lsetxattr(path, attr->name, attr->value, attr->size, 0);
    return rc == 0;
}

bool bx_file_metadata_apply(const struct bx_file_metadata* metadata, int fd,
                            const char* path, bool symlink, bool directory,
                            mode_t mode) {
    char descriptor_path[64];
    const char* acl_path = path;
    bool path_fd = fd >= 0 && (fcntl(fd, F_GETFL) & O_PATH) != 0;
    if (fd >= 0) {
        snprintf(descriptor_path, sizeof(descriptor_path), "/proc/self/fd/%d", fd);
        acl_path = descriptor_path;
    }
    if (metadata->restore_acls && !symlink) {
        acl_t access = metadata->acl_access
            ? bx_metadata_acl_from_text(metadata->acl_access)
            : acl_from_mode(mode & 0777u);
        if (!access)
            return false;
        int rc = fd >= 0 && !path_fd ? acl_set_fd(fd, access)
                          : acl_set_file(acl_path, ACL_TYPE_ACCESS, access);
        int error = errno;
        acl_free(access);
        if (rc != 0) {
            errno = error;
            return false;
        }
        if (directory) {
            if (metadata->acl_default && *metadata->acl_default) {
                acl_t defaults = bx_metadata_acl_from_text(metadata->acl_default);
                if (!defaults)
                    return false;
                rc = acl_set_file(acl_path, ACL_TYPE_DEFAULT, defaults);
                error = errno;
                acl_free(defaults);
                if (rc != 0) {
                    errno = error;
                    return false;
                }
            }
            else if (acl_delete_def_file(acl_path) != 0 && errno != ENODATA)
                return false;
        }
    }
    const struct bx_file_xattr* capabilities = NULL;
    for (size_t i = 0; i < metadata->len; i++) {
        const struct bx_file_xattr* attr = &metadata->xattrs[i];
        if (strcmp(attr->name, "security.capability") == 0) {
            capabilities = attr;
            continue;
        }
        if (!bx_metadata_apply_xattr(attr, fd, path_fd ? acl_path : path, path_fd))
            return false;
    }
    return !capabilities
        || bx_metadata_apply_xattr(capabilities, fd, path_fd ? acl_path : path, path_fd);
}
