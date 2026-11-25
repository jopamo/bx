#include "option_scan.h"

#include <assert.h>
#include <string.h>

void bx_option_cursor_init(struct bx_option_cursor* cursor, char* const argv[],
                           size_t first, size_t end, enum bx_option_order order,
                           const struct bx_option_spec* specs, size_t spec_count) {
    assert(first <= end);
    *cursor = (struct bx_option_cursor) {
        .argv = argv,
        .specs = specs,
        .spec_count = spec_count,
        .index = first,
        .end = end,
        .order = order,
    };
}

static const struct bx_option_spec* find_short(const struct bx_option_cursor* cursor,
                                               unsigned char name) {
    for (size_t i = 0; i < cursor->spec_count; i++) {
        if (cursor->specs[i].short_name == name)
            return &cursor->specs[i];
    }
    return NULL;
}

static const struct bx_option_spec* find_long(const struct bx_option_cursor* cursor,
                                              const char* name, size_t length) {
    for (size_t i = 0; i < cursor->spec_count; i++) {
        const char* candidate = cursor->specs[i].long_name;
        if (candidate != NULL && strncmp(candidate, name, length) == 0 && candidate[length] == '\0')
            return &cursor->specs[i];
    }
    return NULL;
}

static struct bx_option_result scan_error(struct bx_option_cursor* cursor,
                                          struct bx_option_result result,
                                          enum bx_option_event event) {
    cursor->index = result.index;
    cursor->short_offset = 0;
    cursor->stopped = true;
    result.event = event;
    return result;
}

struct bx_option_result bx_option_next(struct bx_option_cursor* cursor) {
    struct bx_option_result result = {.index = cursor->index};
    if (cursor->stopped || cursor->index == cursor->end)
        return result;

    const char* token = cursor->argv[cursor->index];
    if (!cursor->literal && strcmp(token, "--") == 0) {
        result.index = ++cursor->index;
        if (cursor->order == BX_OPTION_PREFIX) {
            cursor->stopped = true;
            return result;
        }
        cursor->literal = true;
        if (cursor->index == cursor->end)
            return result;
        token = cursor->argv[cursor->index];
    }
    if (cursor->literal || token[0] != '-' || token[1] == '\0') {
        if (cursor->order == BX_OPTION_PREFIX) {
            cursor->stopped = true;
            return result;
        }
        cursor->index++;
        result.event = BX_OPTION_OPERAND;
        result.token = token;
        return result;
    }

    result.token = token;
    const char* attached = NULL;
    if (cursor->short_offset == 0 && token[1] == '-') {
        const char* name = token + 2;
        const char* equals = strchr(name, '=');
        size_t length = equals == NULL ? strlen(name) : (size_t)(equals - name);
        if (length != 0)
            result.spec = find_long(cursor, name, length);
        if (equals != NULL)
            attached = equals + 1;
    }
    else {
        size_t offset = cursor->short_offset != 0 ? cursor->short_offset : 1;
        result.short_name = (unsigned char)token[offset];
        result.spec = find_short(cursor, result.short_name);
        if (result.spec != NULL && token[offset + 1] != '\0') {
            if (result.spec->value == BX_OPTION_NO_VALUE) {
                cursor->short_offset = offset + 1;
                result.event = BX_OPTION_MATCH;
                return result;
            }
            attached = token + offset + 1;
        }
    }
    if (result.spec == NULL)
        return scan_error(cursor, result, BX_OPTION_UNKNOWN);

    cursor->index++;
    cursor->short_offset = 0;
    if (result.spec->value == BX_OPTION_NO_VALUE && attached != NULL)
        return scan_error(cursor, result, BX_OPTION_UNEXPECTED_VALUE);
    if (result.spec->value == BX_OPTION_REQUIRED_VALUE && attached == NULL) {
        if (cursor->index == cursor->end)
            return scan_error(cursor, result, BX_OPTION_MISSING_VALUE);
        attached = cursor->argv[cursor->index++];
    }
    result.event = BX_OPTION_MATCH;
    result.value = attached;
    return result;
}
