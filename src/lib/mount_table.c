#define _POSIX_C_SOURCE 200809L

#include "mount_table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bx/libbx.h"

static void bx_mount_entry_free(struct bx_mount_entry* entry) {
    free(entry->source);
    free(entry->target);
    free(entry->fstype);
    free(entry->options);
    memset(entry, 0, sizeof(*entry));
}

static bool bx_mount_table_append(struct bx_mount_table* table, struct bx_mount_entry* entry) {
    if (table->len == table->cap) {
        size_t new_cap = table->cap == 0 ? 16 : table->cap * 2;
        struct bx_mount_entry* new_entries = xrealloc(table->entries, new_cap * sizeof(*new_entries));
        table->entries = new_entries;
        table->cap = new_cap;
    }
    table->entries[table->len++] = *entry;
    memset(entry, 0, sizeof(*entry));
    return true;
}

static int bx_mount_parse_octal_escape(const char* text) {
    int value = 0;
    for (int i = 0; i < 3; i++) {
        char ch = text[i];
        if (ch < '0' || ch > '7') {
            return -1;
        }
        value = (value << 3) | (ch - '0');
    }
    return value;
}

static char* bx_mount_unescape_field(const char* text) {
    size_t len = strlen(text);
    char* out = xmalloc(len + 1);
    size_t j = 0;

    for (size_t i = 0; i < len; i++) {
        if (text[i] == '\\' && i + 3 < len) {
            int escaped = bx_mount_parse_octal_escape(text + i + 1);
            if (escaped >= 0) {
                out[j++] = (char)escaped;
                i += 3;
                continue;
            }
        }
        out[j++] = text[i];
    }

    out[j] = '\0';
    return out;
}

bool bx_mount_table_load(struct bx_mount_table* table) {
    memset(table, 0, sizeof(*table));

    FILE* fp = fopen("/proc/self/mounts", "r");
    if (fp == NULL) {
        return false;
    }

    char* line = NULL;
    size_t linecap = 0;
    bool ok = true;

    while (getline(&line, &linecap, fp) >= 0) {
        char* saveptr = NULL;
        char* source = strtok_r(line, " \t\r\n", &saveptr);
        char* target = strtok_r(NULL, " \t\r\n", &saveptr);
        char* fstype = strtok_r(NULL, " \t\r\n", &saveptr);
        char* options = strtok_r(NULL, " \t\r\n", &saveptr);
        if (source == NULL || target == NULL || fstype == NULL || options == NULL) {
            continue;
        }

        struct bx_mount_entry entry = {
            .source = bx_mount_unescape_field(source),
            .target = bx_mount_unescape_field(target),
            .fstype = bx_mount_unescape_field(fstype),
            .options = bx_mount_unescape_field(options),
        };
        if (!bx_mount_table_append(table, &entry)) {
            bx_mount_entry_free(&entry);
            ok = false;
            break;
        }
    }

    free(line);
    fclose(fp);

    if (!ok) {
        bx_mount_table_free(table);
    }
    return ok;
}

void bx_mount_table_free(struct bx_mount_table* table) {
    if (table == NULL) {
        return;
    }
    for (size_t i = 0; i < table->len; i++) {
        bx_mount_entry_free(&table->entries[i]);
    }
    free(table->entries);
    memset(table, 0, sizeof(*table));
}

const struct bx_mount_entry* bx_mount_table_find_target(const struct bx_mount_table* table, const char* target) {
    if (table == NULL || target == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < table->len; i++) {
        if (strcmp(table->entries[i].target, target) == 0) {
            return &table->entries[i];
        }
    }
    return NULL;
}

bool bx_mount_table_is_target_or_child(const char* parent, const char* candidate) {
    size_t parent_len;

    if (parent == NULL || candidate == NULL) {
        return false;
    }
    if (strcmp(parent, candidate) == 0) {
        return true;
    }

    parent_len = strlen(parent);
    if (parent_len == 0) {
        return false;
    }
    if (strcmp(parent, "/") == 0) {
        return candidate[0] == '/';
    }
    if (strncmp(parent, candidate, parent_len) != 0) {
        return false;
    }
    return candidate[parent_len] == '/';
}
