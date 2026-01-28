#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "applets/archive/tar/tar_id_map.h"
#include "bx/libbx.h"
#include "lib/id_parse.h"

enum bx_tar_id_map_kind {
    BX_TAR_ID_MAP_OWNER = 0,
    BX_TAR_ID_MAP_GROUP,
};

static void bx_tar_id_map_rule_free(struct bx_tar_id_map_rule* rule) {
    free(rule->source_text);
    free(rule->dest_text);
    memset(rule, 0, sizeof(*rule));
}

void bx_tar_id_map_cleanup(struct bx_tar_id_map* map) {
    size_t i;

    for (i = 0u; i < map->len; i++) {
        bx_tar_id_map_rule_free(&map->rules[i]);
    }
    free(map->rules);
    map->rules = NULL;
    map->len = 0u;
    map->cap = 0u;
}

static bool bx_tar_id_map_append_rule(struct bx_tar_id_map* map,
                                      const char* source_text,
                                      bool source_numeric,
                                      uintmax_t source_id,
                                      const char* dest_text,
                                      uintmax_t dest_id) {
    struct bx_tar_id_map_rule* slot;

    if (map->len == map->cap) {
        size_t next_cap = map->cap ? map->cap * 2u : 8u;
        map->rules = xrealloc(map->rules, next_cap * sizeof(*map->rules));
        map->cap = next_cap;
    }

    slot = &map->rules[map->len++];
    memset(slot, 0, sizeof(*slot));
    slot->source_text = xstrdup(source_text);
    slot->source_numeric = source_numeric;
    slot->source_id = source_id;
    slot->dest_text = xstrdup(dest_text);
    slot->dest_id = dest_id;
    return true;
}

static char* bx_tar_id_map_trim(char* line) {
    char* end;

    while (*line != '\0' && isspace((unsigned char)*line)) {
        line++;
    }
    end = line + strlen(line);
    while (end > line && isspace((unsigned char)end[-1])) {
        *--end = '\0';
    }
    return line;
}

static bool bx_tar_id_map_split_line(char* line,
                                     char** source_out,
                                     char** dest_out) {
    char* source = bx_tar_id_map_trim(line);
    char* source_start = source;
    char* dest;

    if (*source == '\0' || *source == '#') {
        *source_out = NULL;
        *dest_out = NULL;
        return true;
    }

    while (*source != '\0' && !isspace((unsigned char)*source)) {
        source++;
    }
    if (*source == '\0') {
        return false;
    }
    *source++ = '\0';
    dest = bx_tar_id_map_trim(source);
    while (*dest != '\0' && !isspace((unsigned char)*dest)) {
        dest++;
    }
    if (*dest == '\0') {
        *source_out = source_start;
        *dest_out = source;
        return true;
    }
    *dest++ = '\0';
    dest = bx_tar_id_map_trim(dest);
    if (*dest != '\0' && *dest != '#') {
        return false;
    }

    *source_out = source_start;
    *dest_out = source;
    return true;
}

static bool bx_tar_id_map_parse_dest(enum bx_tar_id_map_kind kind,
                                     const char* token,
                                     uintmax_t* id_out,
                                     struct bx_diag_ctx* diag) {
    if (kind == BX_TAR_ID_MAP_OWNER) {
        uid_t uid = 0;

        if (!bx_id_parse_owner(token, &uid, diag)) {
            return false;
        }
        *id_out = (uintmax_t)uid;
        return true;
    }

    {
        gid_t gid = 0;

        if (!bx_id_parse_group(token, &gid, diag)) {
            return false;
        }
        *id_out = (uintmax_t)gid;
        return true;
    }
}

static bool bx_tar_id_map_load(struct bx_tar_id_map* map,
                               const char* path,
                               enum bx_tar_id_map_kind kind,
                               struct bx_diag_ctx* diag) {
    FILE* stream = fopen(path, "rb");
    char* line = NULL;
    size_t cap = 0u;
    ssize_t len;
    size_t line_no = 0u;
    bool ok = true;

    if (stream == NULL) {
        bx_diag(diag, "%s: %s", path, strerror(errno));
        return false;
    }

    while ((len = getline(&line, &cap, stream)) >= 0) {
        char* source = NULL;
        char* dest = NULL;
        uintmax_t source_id = 0u;
        uintmax_t dest_id = 0u;
        bool source_numeric;

        line_no++;
        if (len > 0 && line[len - 1u] == '\n') {
            line[len - 1u] = '\0';
        }
        if (!bx_tar_id_map_split_line(line, &source, &dest)) {
            bx_diag(diag, "%s:%zu: malformed mapping", path, line_no);
            ok = false;
            break;
        }
        if (source == NULL) {
            continue;
        }
        source_numeric = bx_id_parse_numeric(source,
                                             (uintmax_t)((kind == BX_TAR_ID_MAP_OWNER)
                                                             ? (uid_t)-1
                                                             : (gid_t)-1),
                                             &source_id);
        if (!bx_tar_id_map_parse_dest(kind, dest, &dest_id, diag)) {
            ok = false;
            break;
        }
        if (!bx_tar_id_map_append_rule(map,
                                       source,
                                       source_numeric,
                                       source_id,
                                       dest,
                                       dest_id)) {
            ok = false;
            break;
        }
    }

    if (ferror(stream)) {
        bx_diag(diag, "%s: %s", path, strerror(errno));
        ok = false;
    }

    free(line);
    fclose(stream);
    return ok;
}

bool bx_tar_id_map_load_owner(struct bx_tar_id_map* map,
                              const char* path,
                              struct bx_diag_ctx* diag) {
    return bx_tar_id_map_load(map, path, BX_TAR_ID_MAP_OWNER, diag);
}

bool bx_tar_id_map_load_group(struct bx_tar_id_map* map,
                              const char* path,
                              struct bx_diag_ctx* diag) {
    return bx_tar_id_map_load(map, path, BX_TAR_ID_MAP_GROUP, diag);
}

static bool bx_tar_id_map_apply(const struct bx_tar_id_map* map,
                                uintmax_t source_id,
                                const char* source_name,
                                uintmax_t* mapped_id_out,
                                const char** mapped_name_out) {
    size_t i;

    for (i = 0u; i < map->len; i++) {
        const struct bx_tar_id_map_rule* rule = &map->rules[i];

        if (rule->source_numeric) {
            if (rule->source_id != source_id) {
                continue;
            }
        }
        else if (source_name == NULL || strcmp(rule->source_text, source_name) != 0) {
            continue;
        }

        *mapped_id_out = rule->dest_id;
        if (mapped_name_out != NULL) {
            *mapped_name_out = rule->dest_text;
        }
        return true;
    }

    return false;
}

bool bx_tar_id_map_apply_owner(const struct bx_tar_id_map* map,
                               uid_t source_uid,
                               const char* source_name,
                               uid_t* mapped_uid_out,
                               const char** mapped_name_out) {
    uintmax_t mapped = 0u;

    if (!bx_tar_id_map_apply(map,
                             (uintmax_t)source_uid,
                             source_name,
                             &mapped,
                             mapped_name_out)) {
        return false;
    }
    *mapped_uid_out = (uid_t)mapped;
    return true;
}

bool bx_tar_id_map_apply_group(const struct bx_tar_id_map* map,
                               gid_t source_gid,
                               const char* source_name,
                               gid_t* mapped_gid_out,
                               const char** mapped_name_out) {
    uintmax_t mapped = 0u;

    if (!bx_tar_id_map_apply(map,
                             (uintmax_t)source_gid,
                             source_name,
                             &mapped,
                             mapped_name_out)) {
        return false;
    }
    *mapped_gid_out = (gid_t)mapped;
    return true;
}
