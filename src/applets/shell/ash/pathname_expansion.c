#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "applets/shell/ash/pathname_expansion.h"
#include "applets/shell/ash/pattern.h"
#include "lib/text_buffer.h"

struct ash_pathname_component {
    char* separator;
    struct bx_text_buffer literal;
    struct ash_pattern pattern;
    bool active;
};

struct ash_pathname_plan {
    struct ash_pathname_component* components;
    size_t count;
    size_t capacity;
    char* trailing_separator;
    bool active;
};

static char* ash_pathname_join(
    const char* prefix,
    const char* separator,
    const char* component
);

static char* ash_pathname_duplicate_span(
    const char* text,
    size_t length
) {
    if (length == SIZE_MAX) {
        errno = ENOMEM;
        return NULL;
    }
    char* copy = malloc(length + 1u);
    if (copy == NULL) {
        return NULL;
    }
    if (length != 0u) {
        memcpy(copy, text, length);
    }
    copy[length] = '\0';
    return copy;
}

static void ash_pathname_component_destroy(
    struct ash_pathname_component* component
) {
    free(component->separator);
    bx_text_buffer_destroy(&component->literal);
    ash_pattern_destroy(&component->pattern);
    *component = (struct ash_pathname_component){0};
}

static void ash_pathname_plan_destroy(struct ash_pathname_plan* plan) {
    for (size_t i = 0u; i < plan->count; i++) {
        ash_pathname_component_destroy(&plan->components[i]);
    }
    free(plan->components);
    free(plan->trailing_separator);
    *plan = (struct ash_pathname_plan){0};
}

static bool ash_pathname_component_active(
    const struct ash_pattern* pattern
) {
    for (size_t i = 0u; i < pattern->root.count; i++) {
        if (pattern->root.terms[i].kind != ASH_PATTERN_LITERAL) {
            return true;
        }
    }
    return false;
}

static bool ash_pathname_pattern_span_may_expand(
    const char* pattern,
    size_t length
) {
    for (size_t i = 0u; i < length; i++) {
        if (pattern[i] == '\\' && i + 1u < length) {
            i++;
            continue;
        }
        if (pattern[i] == '*' || pattern[i] == '?' ||
            pattern[i] == '[') {
            return true;
        }
    }
    return false;
}

static bool ash_pathname_pattern_literal_append(
    struct bx_text_buffer* literal,
    const char* pattern,
    size_t length
) {
    for (size_t input = 0u; input < length; input++) {
        if (pattern[input] == '\\' && input + 1u < length) {
            input++;
        }
        if (!bx_text_buffer_append_char(literal, pattern[input])) {
            return false;
        }
    }
    return true;
}

static int ash_pathname_plan_reserve(
    struct ash_pathname_plan* plan
) {
    if (plan->count < plan->capacity) {
        return 0;
    }
    size_t capacity = plan->capacity == 0u ?
        4u : plan->capacity * 2u;
    if (capacity < plan->capacity ||
        capacity > SIZE_MAX / sizeof(*plan->components)) {
        errno = ENOMEM;
        return -1;
    }
    struct ash_pathname_component* components = realloc(
        plan->components,
        capacity * sizeof(*components)
    );
    if (components == NULL) {
        return -1;
    }
    plan->components = components;
    plan->capacity = capacity;
    return 0;
}

static int ash_pathname_plan_add_component(
    struct ash_pathname_plan* plan,
    const char* separator,
    size_t separator_length,
    const char* pattern,
    size_t pattern_length
) {
    if (!ash_pathname_pattern_span_may_expand(
            pattern,
            pattern_length
        ) &&
        plan->count != 0u &&
        !plan->components[plan->count - 1u].active) {
        struct bx_text_buffer* literal =
            &plan->components[plan->count - 1u].literal;
        return
            bx_text_buffer_append_span(
                literal,
                separator,
                separator_length
            ) &&
            ash_pathname_pattern_literal_append(
                literal,
                pattern,
                pattern_length
            ) ? 0 : -1;
    }

    struct ash_pathname_component component = {
        .separator = ash_pathname_duplicate_span(
            separator,
            separator_length
        ),
    };
    enum ash_pattern_compile_result result = ASH_PATTERN_COMPILE_OK;
    if (component.separator != NULL &&
        ash_pathname_pattern_span_may_expand(
            pattern,
            pattern_length
        )) {
        const struct ash_pattern_options options = {
            .purpose = ASH_PATTERN_PATHNAME_EXPANSION,
            .domain = ASH_PATTERN_PATHNAME,
        };
        result = ash_pattern_compile(
            pattern,
            pattern_length,
            &options,
            &component.pattern
        );
        if (result == ASH_PATTERN_COMPILE_OK) {
            component.active = ash_pathname_component_active(
                &component.pattern
            );
        }
    }
    if (component.separator == NULL ||
        result != ASH_PATTERN_COMPILE_OK) {
        if (result == ASH_PATTERN_COMPILE_NO_MEMORY) {
            errno = ENOMEM;
        }
        else if (result == ASH_PATTERN_COMPILE_LIMIT) {
            errno = ELOOP;
        }
        else if (component.separator != NULL) {
            errno = EINVAL;
        }
        ash_pathname_component_destroy(&component);
        return -1;
    }
    if (!component.active) {
        if (!ash_pathname_pattern_literal_append(
                &component.literal,
                pattern,
                pattern_length
            )) {
            ash_pathname_component_destroy(&component);
            return -1;
        }
        ash_pattern_destroy(&component.pattern);
    }

    if (!component.active && plan->count != 0u &&
        !plan->components[plan->count - 1u].active) {
        struct ash_pathname_component* previous =
            &plan->components[plan->count - 1u];
        if (!bx_text_buffer_append_text(
                &previous->literal,
                component.separator
            ) ||
            !bx_text_buffer_append_span(
                &previous->literal,
                component.literal.data,
                component.literal.length
            )) {
            ash_pathname_component_destroy(&component);
            return -1;
        }
        ash_pathname_component_destroy(&component);
        return 0;
    }

    if (ash_pathname_plan_reserve(plan) != 0) {
        ash_pathname_component_destroy(&component);
        return -1;
    }
    plan->active |= component.active;
    plan->components[plan->count++] = component;
    return 0;
}

static int ash_pathname_plan_build(
    const char* pattern,
    struct ash_pathname_plan* plan
) {
    *plan = (struct ash_pathname_plan){0};
    size_t length = strlen(pattern);
    size_t position = 0u;
    while (position < length) {
        size_t separator = position;
        while (position < length && pattern[position] == '/') {
            position++;
        }
        if (position == length) {
            plan->trailing_separator = ash_pathname_duplicate_span(
                pattern + separator,
                position - separator
            );
            if (plan->trailing_separator == NULL) {
                ash_pathname_plan_destroy(plan);
                return -1;
            }
            return 0;
        }

        size_t component = position;
        while (position < length && pattern[position] != '/') {
            position++;
        }
        if (ash_pathname_plan_add_component(
                plan,
                pattern + separator,
                component - separator,
                pattern + component,
                position - component
            ) != 0) {
            ash_pathname_plan_destroy(plan);
            return -1;
        }
    }
    plan->trailing_separator = ash_pathname_duplicate_span("", 0u);
    if (plan->trailing_separator == NULL) {
        ash_pathname_plan_destroy(plan);
        return -1;
    }
    return 0;
}

bool ash_pathname_pattern_may_expand(const char* pattern) {
    if (pattern == NULL) {
        return false;
    }
    return ash_pathname_pattern_span_may_expand(
        pattern,
        strlen(pattern)
    );
}

void ash_pathname_matches_destroy(struct ash_pathname_matches* matches) {
    if (matches == NULL) {
        return;
    }
    for (size_t i = 0u; i < matches->count; i++) {
        free(matches->values[i]);
    }
    free(matches->values);
    *matches = (struct ash_pathname_matches){0};
}

static int ash_pathname_matches_reserve(
    struct ash_pathname_matches* matches
) {
    if (matches->count == matches->capacity) {
        size_t capacity = matches->capacity == 0u ?
            8u : matches->capacity * 2u;
        if (capacity < matches->capacity ||
            capacity > SIZE_MAX / sizeof(*matches->values)) {
            errno = ENOMEM;
            return -1;
        }
        char** values = realloc(
            matches->values,
            capacity * sizeof(*values)
        );
        if (values == NULL) {
            return -1;
        }
        matches->values = values;
        matches->capacity = capacity;
    }
    return 0;
}

static int ash_pathname_matches_take(
    struct ash_pathname_matches* matches,
    char* value
) {
    if (ash_pathname_matches_reserve(matches) != 0) {
        return -1;
    }
    matches->values[matches->count++] = value;
    return 0;
}

static int ash_pathname_matches_push(
    struct ash_pathname_matches* matches,
    const char* value
) {
    char* copy = ash_pathname_duplicate_span(value, strlen(value));
    if (copy == NULL) {
        return -1;
    }
    if (ash_pathname_matches_take(matches, copy) != 0) {
        free(copy);
        return -1;
    }
    return 0;
}

static char* ash_pathname_join(
    const char* prefix,
    const char* separator,
    const char* component
) {
    size_t prefix_length = strlen(prefix);
    size_t separator_length = strlen(separator);
    size_t component_length = strlen(component);
    if (separator_length > SIZE_MAX - prefix_length ||
        component_length >
            SIZE_MAX - prefix_length - separator_length - 1u) {
        errno = ENOMEM;
        return NULL;
    }
    size_t length =
        prefix_length + separator_length + component_length;
    char* joined = malloc(length + 1u);
    if (joined == NULL) {
        return NULL;
    }
    memcpy(joined, prefix, prefix_length);
    memcpy(
        joined + prefix_length,
        separator,
        separator_length
    );
    memcpy(
        joined + prefix_length + separator_length,
        component,
        component_length + 1u
    );
    return joined;
}

static bool ash_pathname_resource_error(int error) {
    return error == ENOMEM || error == EMFILE || error == ENFILE;
}

static int ash_pathname_finish(
    const struct ash_pathname_plan* plan,
    const char* prefix,
    struct ash_pathname_matches* matches
) {
    struct stat status;
    if (plan->trailing_separator[0] != '\0') {
        if (stat(prefix, &status) != 0) {
            return ash_pathname_resource_error(errno) ? -1 : 0;
        }
        if (!S_ISDIR(status.st_mode)) {
            return 0;
        }
    }
    else if (lstat(prefix, &status) != 0) {
        return ash_pathname_resource_error(errno) ? -1 : 0;
    }

    char* path = ash_pathname_join(
        prefix,
        plan->trailing_separator,
        ""
    );
    if (path == NULL) {
        return -1;
    }
    if (ash_pathname_matches_take(matches, path) != 0) {
        free(path);
        return -1;
    }
    return 0;
}

static int ash_pathname_expand_active_component(
    const struct ash_pathname_component* component,
    const char* directory,
    struct ash_pathname_matches* next
) {
    DIR* stream = opendir(directory[0] != '\0' ? directory : ".");
    if (stream == NULL) {
        return ash_pathname_resource_error(errno) ? -1 : 0;
    }

    int result = 0;
    while (result == 0) {
        errno = 0;
        struct dirent* entry = readdir(stream);
        if (entry == NULL) {
            if (errno != 0) {
                result = -1;
            }
            break;
        }
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        enum ash_pattern_match_result matched = ash_pattern_match(
            &component->pattern,
            entry->d_name
        );
        if (matched == ASH_PATTERN_MATCH_ERROR ||
            matched == ASH_PATTERN_MATCH_UNSUPPORTED) {
            errno = EINVAL;
            result = -1;
            break;
        }
        if (matched != ASH_PATTERN_MATCH) {
            continue;
        }

        char* path = ash_pathname_join(
            directory,
            "",
            entry->d_name
        );
        if (path == NULL) {
            result = -1;
            break;
        }
        result = ash_pathname_matches_take(next, path);
        if (result != 0) {
            free(path);
        }
    }
    int read_error = errno;
    if (closedir(stream) != 0 && result == 0) {
        result = -1;
        read_error = errno;
    }
    if (result != 0) {
        errno = read_error != 0 ? read_error : EIO;
    }
    return result;
}

static int ash_pathname_expand_component(
    const struct ash_pathname_component* component,
    const struct ash_pathname_matches* current,
    struct ash_pathname_matches* next
) {
    for (size_t i = 0u; i < current->count; i++) {
        char* directory = ash_pathname_join(
            current->values[i],
            component->separator,
            ""
        );
        if (directory == NULL) {
            return -1;
        }

        int result = 0;
        if (component->active) {
            result = ash_pathname_expand_active_component(
                component,
                directory,
                next
            );
        }
        else {
            char* path = ash_pathname_join(
                directory,
                "",
                component->literal.data
            );
            if (path == NULL) {
                result = -1;
            }
            else {
                result = ash_pathname_matches_take(next, path);
                if (result != 0) {
                    free(path);
                }
            }
        }
        free(directory);
        if (result != 0) {
            return -1;
        }
    }
    return 0;
}

static int ash_pathname_execute_plan(
    const struct ash_pathname_plan* plan,
    struct ash_pathname_matches* output
) {
    struct ash_pathname_matches current = {0};
    if (ash_pathname_matches_push(&current, "") != 0) {
        return -1;
    }

    for (size_t i = 0u; i < plan->count; i++) {
        struct ash_pathname_matches next = {0};
        if (ash_pathname_expand_component(
                &plan->components[i],
                &current,
                &next
            ) != 0) {
            ash_pathname_matches_destroy(&next);
            ash_pathname_matches_destroy(&current);
            return -1;
        }
        ash_pathname_matches_destroy(&current);
        current = next;
        if (current.count == 0u) {
            break;
        }
    }

    for (size_t i = 0u; i < current.count; i++) {
        if (ash_pathname_finish(
                plan,
                current.values[i],
                output
            ) != 0) {
            ash_pathname_matches_destroy(&current);
            return -1;
        }
    }
    ash_pathname_matches_destroy(&current);
    return 0;
}

static int ash_pathname_compare(
    const void* left,
    const void* right
) {
    const char* const* left_path = left;
    const char* const* right_path = right;
    return strcoll(*left_path, *right_path);
}

enum ash_pathname_expansion_result ash_pathname_expand(
    const char* pattern,
    struct ash_pathname_matches* output
) {
    if (output == NULL) {
        errno = EINVAL;
        return ASH_PATHNAME_EXPANSION_ERROR;
    }
    *output = (struct ash_pathname_matches){0};
    if (pattern == NULL) {
        errno = EINVAL;
        return ASH_PATHNAME_EXPANSION_ERROR;
    }

    struct ash_pathname_plan plan;
    if (ash_pathname_plan_build(pattern, &plan) != 0) {
        return ASH_PATHNAME_EXPANSION_ERROR;
    }
    if (!plan.active) {
        ash_pathname_plan_destroy(&plan);
        return ASH_PATHNAME_EXPANSION_NO_MATCH;
    }

    int result = ash_pathname_execute_plan(&plan, output);
    ash_pathname_plan_destroy(&plan);
    if (result != 0) {
        ash_pathname_matches_destroy(output);
        return ASH_PATHNAME_EXPANSION_ERROR;
    }
    if (output->count == 0u) {
        return ASH_PATHNAME_EXPANSION_NO_MATCH;
    }
    qsort(
        output->values,
        output->count,
        sizeof(*output->values),
        ash_pathname_compare
    );
    return ASH_PATHNAME_EXPANSION_MATCH;
}
