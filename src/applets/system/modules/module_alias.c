#define _GNU_SOURCE
#include "module_alias.h"

#ifdef __linux__
#include <errno.h>
#include <stdlib.h>
#include <string.h>

enum { MATCH_LIMIT = 65536, SEARCH_BUDGET = 32 * 1024 * 1024 };

struct bx_module_alias_match {
    struct bx_module_path name;
    size_t order;
};

static bool alias_byte(char c) {
    return (unsigned char)c >= '!' && (unsigned char)c <= '~';
}

static char normalized(char c) {
    return c == '-' ? '_' : c;
}

bool bx_module_alias_query_valid(const char* query) {
    return bx_module_alias_span_valid(query, strlen(query));
}

bool bx_module_alias_span_valid(const char* query, size_t len) {
    if (len == 0 || len > BX_MODULE_ALIAS_LIMIT)
        return false;
    for (size_t i = 0; i < len; i++) {
        if (!alias_byte(query[i]))
            return false;
    }
    return true;
}

enum atom_type { LITERAL, ANY, STAR, CLASS };

struct atom {
    enum atom_type type;
    char value;
    const char* first;
    const char* end;
};

static char class_byte(const char** p, const char* end) {
    if (**p == '\\' && *p + 1 < end)
        (*p)++;
    return *(*p)++;
}

/* Kernel modaliases use byte ranges, not locale-dependent character classes. */
static bool class_matches(const struct atom* atom, char byte) {
    const char* p = atom->first;
    bool negate = *p == '!' || *p == '^';
    p += negate;
    bool found = false;
    while (p < atom->end) {
        char from = class_byte(&p, atom->end), to = from;
        if (p + 1 < atom->end && *p == '-') {
            p++;
            to = class_byte(&p, atom->end);
        }
        if (from <= byte && byte <= to)
            found = true;
    }
    return found != negate;
}

static int parse_pattern(struct bx_module_path pattern, struct atom* atoms, size_t* count) {
    if (pattern.len == 0 || pattern.len > BX_MODULE_ALIAS_LIMIT)
        goto invalid;
    const char* end = pattern.text + pattern.len;
    for (const char* p = pattern.text; p < end; p++) {
        if (!alias_byte(*p))
            goto invalid;
    }
    size_t n = 0;
    for (const char* p = pattern.text; p < end; p++) {
        struct atom atom = {.type = LITERAL, .value = normalized(*p)};
        if (*p == '*')
            atom.type = STAR;
        else if (*p == '?')
            atom.type = ANY;
        else if (*p == '\\') {
            if (++p == end)
                goto invalid;
            atom.value = normalized(*p);
        } else if (*p == '[') {
            atom.type = CLASS;
            atom.first = ++p;
            if (p < end && (*p == '!' || *p == '^'))
                p++;
            if (p < end && *p == ']')
                p++;
            for (; p < end && *p != ']'; p++) {
                if (*p == '\\' && p + 1 < end)
                    p++;
                else if (*p == '[')
                    goto invalid;
            }
            if (p == end)
                goto invalid;
            atom.end = p;
        }
        atoms[n++] = atom;
    }
    *count = n;
    return 0;
invalid:
    errno = EINVAL;
    return -1;
}

static int match_pattern(const char* query, struct bx_module_path pattern, size_t* budget) {
    struct atom atoms[BX_MODULE_ALIAS_LIMIT];
    size_t n;
    if (parse_pattern(pattern, atoms, &n) < 0)
        return -1;
    /* Retry only the most recent star; each retry consumes a query byte. */
    size_t i = 0, star = 0;
    const char* retry = NULL;
    while (*query != '\0') {
        size_t cost = 1;
        if (i < n && atoms[i].type == CLASS)
            cost += (size_t)(atoms[i].end - atoms[i].first);
        if (*budget < cost) {
            errno = E2BIG;
            return -1;
        }
        *budget -= cost;
        if (i < n && atoms[i].type == STAR) {
            star = ++i;
            if (i == n)
                return 1;
            retry = query;
        } else if (i < n && (atoms[i].type == ANY ||
                            (atoms[i].type == LITERAL && atoms[i].value == normalized(*query)) ||
                            (atoms[i].type == CLASS && class_matches(&atoms[i], *query)))) {
            i++;
            query++;
        } else if (retry != NULL) {
            i = star;
            query = ++retry;
        } else {
            return 0;
        }
    }
    while (i < n && atoms[i].type == STAR)
        i++;
    return i == n;
}

static int collect(struct bx_module_aliases* result, size_t* capacity, const char* query,
                   struct bx_module_path pattern, struct bx_module_path name, size_t* budget) {
    if (!bx_module_name_span_valid(name.text, name.len)) {
        errno = EINVAL;
        return -1;
    }
    int matched = match_pattern(query, pattern, budget);
    if (matched <= 0)
        return matched;
    if (result->count == *capacity) {
        if (*capacity == MATCH_LIMIT) {
            errno = E2BIG;
            return -1;
        }
        size_t next = *capacity == 0 ? 16 : *capacity * 2;
        void* resized = realloc(result->matches, next * sizeof(*result->matches));
        if (resized == NULL)
            return -1;
        result->matches = resized;
        *capacity = next;
    }
    result->matches[result->count] = (struct bx_module_alias_match) {name, result->count};
    result->count++;
    return 0;
}

static struct bx_module_path token(const char** cursor, const char* end) {
    const char* p = *cursor;
    while (p < end && (*p == ' ' || *p == '\t'))
        p++;
    const char* start = p;
    while (p < end && *p != ' ' && *p != '\t')
        p++;
    *cursor = p;
    return (struct bx_module_path) {start, (size_t)(p - start)};
}

static int collect_installed(struct bx_module_tree* tree, const char* query,
                             struct bx_module_aliases* result, size_t* cap, size_t* budget) {
    const char* data;
    size_t len;
    if (bx_module_tree_aliases(tree, &data, &len) < 0)
        return -1;
    const char* end = data + len;
    for (const char* p = data; p < end;) {
        const char* nl = memchr(p, '\n', (size_t)(end - p));
        const char* stop = nl ? nl : end;
        const char* field = p;
        p = nl ? nl + 1 : stop;
        struct bx_module_path command = token(&field, stop);
        if (command.len == 0 || command.text[0] == '#')
            continue;
        struct bx_module_path pattern = token(&field, stop);
        struct bx_module_path name = token(&field, stop);
        if (command.len != 5 || memcmp(command.text, "alias", 5) != 0 ||
            token(&field, stop).len != 0) {
            errno = EINVAL;
            return -1;
        }
        if (collect(result, cap, query, pattern, name, budget) < 0)
            return -1;
    }
    return 0;
}

static int collect_builtins(struct bx_module_tree* tree, const char* query,
                            struct bx_module_aliases* result, size_t* cap, size_t* budget) {
    const char* data;
    size_t len;
    if (bx_module_tree_builtin_info(tree, &data, &len) < 0)
        return -1;
    const char* end = data + len;
    for (const char* p = data; p < end;) {
        size_t n = strlen(p);
        const char* record = p;
        p += n + 1;
        if (n == 0)
            continue;
        const char* dot = strchr(record, '.');
        static const char field[] = ".alias=";
        if ((size_t)(p - dot - 1) < sizeof(field) - 1 ||
            memcmp(dot, field, sizeof(field) - 1) != 0)
            continue;
        const char* value = dot + sizeof(field) - 1;
        struct bx_module_path pattern = {value, (size_t)(p - value - 1)};
        struct bx_module_path name = {record, (size_t)(dot - record)};
        if (collect(result, cap, query, pattern, name, budget) < 0)
            return -1;
    }
    return 0;
}

static int by_order(const void* a, const void* b) {
    const struct bx_module_alias_match* left = a;
    const struct bx_module_alias_match* right = b;
    return (left->order > right->order) - (left->order < right->order);
}

static int by_name(const void* a, const void* b) {
    const struct bx_module_alias_match* left = a;
    const struct bx_module_alias_match* right = b;
    int r = bx_module_name_cmp(left->name, right->name);
    return r != 0 ? r : by_order(a, b);
}

static int exact_match(const char* name, struct bx_module_aliases* aliases) {
    size_t len = strlen(name);
    struct bx_module_alias_match* match = malloc(sizeof(*match) + len);
    if (match == NULL)
        return -1;
    char* copy = (char*)(match + 1);
    memcpy(copy, name, len);
    *match = (struct bx_module_alias_match) {{copy, len}, 0};
    *aliases = (struct bx_module_aliases) {match, 1};
    return 0;
}

int bx_module_resolve_alias(struct bx_module_tree* tree, const char* query,
                            struct bx_module_aliases* aliases) {
    bool name = bx_module_name_valid(query);
    int exact = name ? bx_module_tree_find_loadable(tree, query, NULL, 0) : 0;
    if (exact != 0)
        return exact < 0 ? -1 : exact_match(query, aliases);
    struct bx_module_aliases result = {0};
    size_t cap = 0, budget = SEARCH_BUDGET;
    if (collect_installed(tree, query, &result, &cap, &budget) < 0)
        goto fail;
    if (result.count == 0) {
        exact = name ? bx_module_tree_has_builtin(tree, query) : 0;
        if (exact != 0)
            return exact < 0 ? -1 : exact_match(query, aliases);
        if (collect_builtins(tree, query, &result, &cap, &budget) < 0)
            goto fail;
    }
    if (result.count == 0) {
        errno = ENOENT;
        goto fail;
    }
    qsort(result.matches, result.count, sizeof(*result.matches), by_name);
    size_t unique = 1;
    for (size_t i = 1; i < result.count; i++) {
        if (bx_module_name_cmp(result.matches[unique - 1].name, result.matches[i].name) != 0)
            result.matches[unique++] = result.matches[i];
    }
    qsort(result.matches, unique, sizeof(*result.matches), by_order);
    result.count = unique;
    *aliases = result;
    return 0;
fail: {
    int error = errno;
    free(result.matches);
    errno = error;
    return -1;
}
}

struct bx_module_path bx_module_alias_at(const struct bx_module_aliases* aliases, size_t i) {
    return aliases->matches[i].name;
}
#endif
