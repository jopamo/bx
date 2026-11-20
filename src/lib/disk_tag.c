#define _XOPEN_SOURCE 700

#include "lib/disk_tag.h"

#include <errno.h>
#include <string.h>

#ifdef __linux__
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/stat.h>

/* Udev preserves valid UTF-8 in a link name. Invalid bytes are escaped. */
static size_t utf8_length(const unsigned char* value, size_t left) {
    unsigned char lead = value[0];
    size_t n;
    uint32_t code;
    if (lead >= 0xc2 && lead <= 0xdf) {
        n = 2;
        code = lead & 0x1f;
    } else if (lead >= 0xe0 && lead <= 0xef) {
        n = 3;
        code = lead & 0x0f;
    } else if (lead >= 0xf0 && lead <= 0xf4) {
        n = 4;
        code = lead & 0x07;
    } else {
        return 0;
    }
    if (left < n)
        return 0;
    for (size_t i = 1; i < n; i++) {
        if ((value[i] & 0xc0) != 0x80)
            return 0;
        code = (code << 6) | (value[i] & 0x3f);
    }
    if ((n == 3 && code < 0x800) || (n == 4 && code < 0x10000) ||
        (code >= 0xd800 && code <= 0xdfff) || code > 0x10ffff ||
        (code >= 0xfdd0 && code <= 0xfdef) || (code & 0xffff) == 0xffff)
        return 0;
    return n;
}

static int disk_tag_component(const char* value, char* name) {
    size_t len = strnlen(value, NAME_MAX + 1);
    if (len == 0 || strcmp(value, ".") == 0 || strcmp(value, "..") == 0) {
        errno = EINVAL;
        return -1;
    }
    if (len > NAME_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }

    static const char hex[] = "0123456789abcdef";
    size_t used = 0;
    for (size_t i = 0; i < len;) {
        const unsigned char* input = (const unsigned char*)value + i;
        unsigned char c = *input;
        size_t n = utf8_length(input, len - i);
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || strchr("#+-.:=@_", c) != NULL)
            n = 1;
        if ((n ? n : 4) > NAME_MAX - used) {
            errno = ENAMETOOLONG;
            return -1;
        }
        if (n) {
            memcpy(name + used, input, n);
            used += n;
            i += n;
        } else {
            memcpy(name + used, "\\x", 2);
            name[used + 2] = hex[c >> 4];
            name[used + 3] = hex[c & 0x0f];
            used += 4;
            i++;
        }
    }
    name[used] = '\0';
    return 0;
}
#endif

static const struct bx_disk_tag {
    const char* prefix;
    const char* directory;
} tags[] = {
    {"LABEL=", "/dev/disk/by-label/"},
    {"UUID=", "/dev/disk/by-uuid/"},
    {"PARTLABEL=", "/dev/disk/by-partlabel/"},
    {"PARTUUID=", "/dev/disk/by-partuuid/"},
};

static const struct bx_disk_tag* disk_tag_lookup(const char* spec, const char** value) {
    for (size_t i = 0; i < sizeof(tags) / sizeof(tags[0]); i++) {
        size_t prefix_len = strlen(tags[i].prefix);
        if (strncmp(spec, tags[i].prefix, prefix_len) == 0) {
            *value = spec + prefix_len;
            return &tags[i];
        }
    }
    return NULL;
}

const char* bx_disk_tag_value(const char* spec) {
    const char* value = NULL;
    disk_tag_lookup(spec, &value);
    return value;
}

int bx_disk_tag_resolve(const char* spec, char** device) {
    *device = NULL;
    const char* value;
    const struct bx_disk_tag* tag = disk_tag_lookup(spec, &value);
    if (!tag)
        return 0;
#ifdef __linux__
    char path[sizeof("/dev/disk/by-partlabel/") + NAME_MAX];
    size_t dir_len = strlen(tag->directory);
    memcpy(path, tag->directory, dir_len);
    if (disk_tag_component(value, path + dir_len) < 0)
        return -1;
    char* resolved = realpath(path, NULL);
    if (resolved == NULL)
        return -1;
    struct stat st;
    if (stat(resolved, &st) < 0) {
        int error = errno;
        free(resolved);
        errno = error;
        return -1;
    }
    if (!S_ISBLK(st.st_mode)) {
        free(resolved);
        errno = ENOTBLK;
        return -1;
    }
    *device = resolved;
    return 1;
#else
    errno = ENOTSUP;
    return -1;
#endif
}
