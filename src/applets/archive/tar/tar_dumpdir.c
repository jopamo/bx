#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "applets/archive/tar/tar_dumpdir.h"
#include "bx/libbx.h"

static bool bx_tar_dumpdir_append(struct bx_tar_dumpdir* dumpdir, char marker, const unsigned char* name, size_t name_len) {
    struct bx_tar_dumpdir_record* record;

    if (dumpdir->len == dumpdir->cap) {
        size_t next_cap = dumpdir->cap ? dumpdir->cap * 2u : 16u;

        dumpdir->records = xrealloc(dumpdir->records, next_cap * sizeof(*dumpdir->records));
        dumpdir->cap = next_cap;
    }
    record = &dumpdir->records[dumpdir->len++];
    record->marker = marker;
    record->name = xmalloc(name_len + 1u);
    memcpy(record->name, name, name_len);
    record->name[name_len] = '\0';
    return true;
}

void bx_tar_dumpdir_free(struct bx_tar_dumpdir* dumpdir) {
    size_t i;

    if (dumpdir == NULL) {
        return;
    }
    for (i = 0u; i < dumpdir->len; i++) {
        free(dumpdir->records[i].name);
    }
    free(dumpdir->records);
    dumpdir->records = NULL;
    dumpdir->len = 0u;
    dumpdir->cap = 0u;
}

bool bx_tar_dumpdir_parse(const unsigned char* data, size_t len, struct bx_tar_dumpdir* dumpdir, struct bx_diag_ctx* diag) {
    size_t offset = 0u;
    char expected = '\0';
    bool temporary_available = false;

    if (dumpdir == NULL || (len > 0u && data == NULL)) {
        bx_diag(diag, "invalid incremental dumpdir");
        return false;
    }
    bx_tar_dumpdir_free(dumpdir);
    if (len == 0u || data[len - 1u] != '\0' || (len > 1u && data[len - 2u] != '\0')) {
        bx_diag(diag, "invalid incremental dumpdir terminator");
        return false;
    }

    while (offset < len && data[offset] != '\0') {
        const unsigned char* record_start = data + offset;
        const unsigned char* record_end = memchr(record_start, '\0', len - offset);
        size_t record_len;
        char marker;
        const unsigned char* name;
        size_t name_len;

        if (record_end == NULL) {
            bx_diag(diag, "unterminated incremental dumpdir record");
            bx_tar_dumpdir_free(dumpdir);
            return false;
        }
        record_len = (size_t)(record_end - record_start);
        if (record_len == 0u) {
            break;
        }
        marker = (char)record_start[0];
        name = record_start + 1u;
        name_len = record_len - 1u;

        if (expected != '\0' && marker != expected) {
            bx_diag(diag, "invalid incremental dumpdir sequence: expected '%c'", expected);
            bx_tar_dumpdir_free(dumpdir);
            return false;
        }
        switch (marker) {
            case 'Y':
            case 'N':
            case 'D':
                if (name_len == 0u) {
                    bx_diag(diag, "invalid incremental dumpdir entry name");
                    bx_tar_dumpdir_free(dumpdir);
                    return false;
                }
                expected = '\0';
                break;
            case 'R':
                if (name_len == 0u) {
                    if (!temporary_available) {
                        bx_diag(diag, "invalid incremental dumpdir temporary rename");
                        bx_tar_dumpdir_free(dumpdir);
                        return false;
                    }
                    temporary_available = false;
                }
                expected = 'T';
                break;
            case 'T':
                if (name_len == 0u && !temporary_available) {
                    bx_diag(diag, "invalid incremental dumpdir temporary target");
                    bx_tar_dumpdir_free(dumpdir);
                    return false;
                }
                expected = '\0';
                break;
            case 'X':
                if (name_len == 0u || temporary_available) {
                    bx_diag(diag, "invalid incremental dumpdir temporary directory");
                    bx_tar_dumpdir_free(dumpdir);
                    return false;
                }
                temporary_available = true;
                expected = '\0';
                break;
            default:
                bx_diag(diag, "invalid incremental dumpdir marker '%c'", marker);
                bx_tar_dumpdir_free(dumpdir);
                return false;
        }
        if (!bx_tar_dumpdir_append(dumpdir, marker, name, name_len)) {
            bx_tar_dumpdir_free(dumpdir);
            return false;
        }
        offset += record_len + 1u;
    }
    if (expected != '\0') {
        bx_diag(diag, "invalid incremental dumpdir sequence: expected '%c'", expected);
        bx_tar_dumpdir_free(dumpdir);
        return false;
    }
    return true;
}

const struct bx_tar_dumpdir_record* bx_tar_dumpdir_find(const struct bx_tar_dumpdir* dumpdir, const char* name) {
    size_t i;

    if (dumpdir == NULL || name == NULL) {
        return NULL;
    }
    for (i = 0u; i < dumpdir->len; i++) {
        const struct bx_tar_dumpdir_record* record = &dumpdir->records[i];

        if ((record->marker == 'Y' || record->marker == 'N' || record->marker == 'D') && strcmp(record->name, name) == 0) {
            return record;
        }
    }
    return NULL;
}
