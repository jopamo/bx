#include <regex.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "applets/archive/archive_common.h"
#include "applets/archive/tar/tar_names.h"
#include "bx/libbx.h"
#include "lib/path_ops.h"

static bool bx_tar_transform_parse_part(const char** cursor,
                                        char delimiter,
                                        bool regex_text,
                                        char** out) {
    struct bx_archive_buffer buffer;
    const char* p = *cursor;

    bx_archive_buffer_init(&buffer);
    while (*p != '\0') {
        if (*p == delimiter) {
            p++;
            break;
        }
        if (*p == '\\' && p[1] != '\0') {
            p++;
            if (*p == delimiter || !regex_text) {
                bx_archive_buffer_append_byte(&buffer, (unsigned char)*p);
            }
            else {
                bx_archive_buffer_append_byte(&buffer, '\\');
                bx_archive_buffer_append_byte(&buffer, (unsigned char)*p);
            }
            p++;
            continue;
        }
        bx_archive_buffer_append_byte(&buffer, (unsigned char)*p);
        p++;
    }

    if (p == *cursor || p[-1] != delimiter) {
        bx_archive_buffer_free(&buffer);
        return false;
    }

    bx_archive_buffer_append_byte(&buffer, '\0');
    *out = xstrdup((const char*)buffer.data);
    bx_archive_buffer_free(&buffer);
    *cursor = p;
    return true;
}

bool bx_tar_transform_rule_init(struct bx_tar_transform_rule* rule,
                                const char* spec,
                                struct bx_diag_ctx* diag) {
    char delimiter;
    const char* cursor;
    char* pattern = NULL;
    int reg_flags = 0;

    bx_tar_transform_rule_cleanup(rule);

    if (spec == NULL || spec[0] != 's' || spec[1] == '\0') {
        bx_diag(diag, "invalid transform expression '%s'", spec ? spec : "");
        return false;
    }

    delimiter = spec[1];
    cursor = spec + 2;

    if (!bx_tar_transform_parse_part(&cursor, delimiter, true, &pattern)
        || !bx_tar_transform_parse_part(&cursor, delimiter, false, &rule->replacement)) {
        free(pattern);
        bx_tar_transform_rule_cleanup(rule);
        bx_diag(diag, "invalid transform expression '%s'", spec);
        return false;
    }

    while (*cursor != '\0') {
        if (*cursor == 'g') {
            rule->global = true;
            cursor++;
            continue;
        }
        free(pattern);
        bx_tar_transform_rule_cleanup(rule);
        bx_diag(diag, "invalid transform flags '%s'", cursor);
        return false;
    }

    if (regcomp(&rule->regex, pattern, reg_flags) != 0) {
        free(pattern);
        bx_tar_transform_rule_cleanup(rule);
        bx_diag(diag, "invalid transform expression '%s'", spec);
        return false;
    }

    free(pattern);
    rule->active = true;
    return true;
}

void bx_tar_transform_rule_cleanup(struct bx_tar_transform_rule* rule) {
    if (!rule->active) {
        free(rule->replacement);
        rule->replacement = NULL;
        rule->global = false;
        return;
    }

    regfree(&rule->regex);
    free(rule->replacement);
    rule->replacement = NULL;
    rule->global = false;
    rule->active = false;
}

static char* bx_tar_transform_apply(const struct bx_tar_transform_rule* rule,
                                    const char* input) {
    struct bx_archive_buffer output;
    const char* cursor = input;

    if (rule == NULL || !rule->active) {
        return xstrdup(input);
    }

    bx_archive_buffer_init(&output);
    while (true) {
        regmatch_t match;
        int rc = regexec(&rule->regex, cursor, 1, &match, 0);
        size_t prefix_len;
        size_t match_len;

        if (rc != 0) {
            bx_archive_buffer_append(&output, cursor, strlen(cursor));
            break;
        }

        prefix_len = (size_t)match.rm_so;
        match_len = (size_t)(match.rm_eo - match.rm_so);
        bx_archive_buffer_append(&output, cursor, prefix_len);
        bx_archive_buffer_append(&output, rule->replacement, strlen(rule->replacement));

        cursor += match.rm_eo;
        if (!rule->global) {
            bx_archive_buffer_append(&output, cursor, strlen(cursor));
            break;
        }

        if (match_len == 0u && *cursor != '\0') {
            bx_archive_buffer_append_byte(&output, (unsigned char)*cursor);
            cursor++;
        }
        if (*cursor == '\0') {
            break;
        }
    }

    bx_archive_buffer_append_byte(&output, '\0');
    {
        char* result = xstrdup((const char*)output.data);
        bx_archive_buffer_free(&output);
        return result;
    }
}

static const char* bx_tar_map_member_name_borrow_ptr(const char* stored_name,
                                                     const struct bx_tar_name_policy* policy) {
    const char* name = stored_name;
    const char* normalized_name;

    if (stored_name[0] == '\0') {
        return stored_name;
    }
    if (policy != NULL) {
        if (policy->strip_components != 0u || policy->one_top_level != NULL) {
            return NULL;
        }
        if (policy->transform != NULL && policy->transform->active) {
            return NULL;
        }
    }

    while (name[0] == '.' && name[1] == '/') {
        name += 2;
    }
    if (name[0] == '\0' || name[0] == '/') {
        return NULL;
    }
    normalized_name = name;

    for (;;) {
        const char* slash = strchr(name, '/');
        size_t part_len = slash != NULL ? (size_t)(slash - name) : strlen(name);

        if (part_len == 0u) {
            return NULL;
        }
        if (part_len == 1u && name[0] == '.') {
            return NULL;
        }
        if (part_len == 2u && name[0] == '.' && name[1] == '.') {
            return NULL;
        }
        if (slash == NULL) {
            return normalized_name;
        }
        name = slash + 1;
    }
}

struct bx_tar_mapped_name bx_tar_map_member_name(const char* stored_name,
                                                 const struct bx_tar_name_policy* policy,
                                                 bool* stripped_absolute,
                                                 bool* stripped_dotdot) {
    struct bx_path_components components = {0};
    struct bx_archive_buffer output;
    char* transformed = NULL;
    const char* name;
    bool leading_slash = false;
    size_t start_index = 0u;
    struct bx_tar_mapped_name result = {0};

    *stripped_absolute = false;
    *stripped_dotdot = false;

    {
        const char* borrowed = bx_tar_map_member_name_borrow_ptr(stored_name, policy);

        if (borrowed != NULL) {
            result.text = borrowed;
            return result;
        }
    }

    transformed = bx_tar_transform_apply(policy ? policy->transform : NULL, stored_name);
    name = transformed;

    while (*name == '/') {
        if (policy != NULL && policy->absolute_names && policy->one_top_level == NULL) {
            leading_slash = true;
        }
        else {
            *stripped_absolute = true;
        }
        name++;
    }

    bx_path_components_append_raw(&components, name);
    for (size_t i = 0u; i < components.count; i++) {
        if (strcmp(components.parts[i], ".") == 0) {
            free(components.parts[i]);
            components.parts[i] = xstrdup("");
            continue;
        }
        if (strcmp(components.parts[i], "..") == 0) {
            *stripped_dotdot = true;
            free(components.parts[i]);
            components.parts[i] = xstrdup("");
        }
    }

    if (policy != NULL && policy->strip_components > 0u) {
        size_t dropped = 0u;
        while (dropped < policy->strip_components && start_index < components.count) {
            start_index++;
            dropped++;
        }
    }

    bx_archive_buffer_init(&output);
    if (leading_slash && start_index < components.count) {
        bx_archive_buffer_append_byte(&output, '/');
    }

    if (policy != NULL && policy->one_top_level != NULL && start_index < components.count) {
        bx_archive_buffer_append(&output, policy->one_top_level, strlen(policy->one_top_level));
    }

    for (size_t i = start_index; i < components.count; i++) {
        const char* part = components.parts[i];
        if (part[0] == '\0') {
            continue;
        }
        if (output.len != 0u && output.data[output.len - 1u] != '/') {
            bx_archive_buffer_append_byte(&output, '/');
        }
        bx_archive_buffer_append(&output, part, strlen(part));
    }

    bx_archive_buffer_append_byte(&output, '\0');
    result.owned = xstrdup((const char*)output.data);
    result.text = result.owned;

    bx_archive_buffer_free(&output);
    bx_path_components_free(&components);
    free(transformed);
    return result;
}
