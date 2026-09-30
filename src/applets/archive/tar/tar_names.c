#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "applets/archive/archive_common.h"
#include "applets/archive/tar/tar_names.h"
#include "bx/libbx.h"
#include "lib/path_ops.h"

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

    name = stored_name;

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
    return result;
}
