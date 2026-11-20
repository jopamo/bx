#define _GNU_SOURCE
#include "module_plan.h"

#ifdef __linux__
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "lib/fd_ops.h"
#include "module_alias.h"

enum { MODULE_RECORD_LIMIT = 262144, ALIAS_QUERY_LIMIT = 256, SOFT_EDGE_LIMIT = 65536 };
enum visit { UNSEEN, HARD, PRE, POST, DONE };

struct entry {
    struct bx_module_dep record;
    struct bx_module_path name;
    struct bx_module_path pre;
    struct bx_module_path post;
    struct bx_module_aliases aliases;
    size_t next_alias;
    size_t order;
    struct entry* parent;
    enum visit visit;
    bool builtin;
    bool requested;
};

static int compare_paths(struct bx_module_path a, struct bx_module_path b) {
    int r = memcmp(a.text, b.text, a.len < b.len ? a.len : b.len);
    return r ? r : (a.len > b.len) - (a.len < b.len);
}

static int compare_entries(const void* left, const void* right) {
    const struct entry* a = left;
    const struct entry* b = right;
    if (a->builtin != b->builtin)
        return (int)a->builtin - (int)b->builtin;
    return compare_paths(a->record.path, b->record.path);
}

static struct entry* find_entry(struct entry* entries, size_t count, struct bx_module_path key) {
    struct entry search = {.record.path = key};
    return bsearch(&search, entries, count, sizeof(*entries), compare_entries);
}

static bool valid_path(struct bx_module_path path, bool compressed) {
    return path.len < PATH_MAX && bx_module_path_valid(path.text, path.len) &&
           bx_module_path_name(path, compressed).len != 0;
}

static int count_entries(struct bx_module_dep_iter deps, struct bx_module_dep_iter builtins,
                         size_t* count) {
    struct bx_module_dep record;
    size_t n = 0;
    int r;
    while ((r = bx_module_dep_next(&deps, &record)) > 0) {
        if (n == MODULE_RECORD_LIMIT) {
            errno = EFBIG;
            return -1;
        }
        if (!valid_path(record.path, true)) {
            errno = EINVAL;
            return -1;
        }
        n++;
    }
    if (r < 0)
        return -1;
    struct bx_module_path path;
    while (bx_module_builtin_next(&builtins, &path)) {
        if (n == MODULE_RECORD_LIMIT || !valid_path(path, false)) {
            errno = n == MODULE_RECORD_LIMIT ? EFBIG : EINVAL;
            return -1;
        }
        n++;
    }
    *count = n;
    return 0;
}

static struct entry* read_entries(struct bx_module_tree* tree, size_t* count) {
    struct bx_module_dep_iter deps, builtins;
    size_t n;
    if (bx_module_tree_deps(tree, &deps) < 0 || bx_module_tree_builtins(tree, &builtins) < 0 ||
        count_entries(deps, builtins, &n) < 0)
        return NULL;
    struct entry* entries = calloc(n ? n : 1, sizeof(*entries));
    if (entries == NULL)
        return NULL;
    size_t pos = 0;
    struct bx_module_dep record;
    while (bx_module_dep_next(&deps, &record) > 0) {
        entries[pos].record = record;
        entries[pos].name = bx_module_path_name(record.path, true);
        entries[pos].order = pos;
        pos++;
    }
    for (; pos < n; pos++) {
        bx_module_builtin_next(&builtins, &entries[pos].record.path);
        entries[pos].builtin = true;
        entries[pos].name = bx_module_path_name(entries[pos].record.path, false);
        entries[pos].order = pos;
    }
    qsort(entries, n, sizeof(*entries), compare_entries);
    for (size_t i = 1; i < n; i++) {
        if (compare_entries(&entries[i - 1], &entries[i]) == 0) {
            free(entries);
            errno = EINVAL;
            return NULL;
        }
    }
    *count = n;
    return entries;
}

static bool space(char c) {
    return c == ' ' || c == '\t';
}

static int by_name(const void* left, const void* right) {
    const struct entry* a = *(struct entry* const*)left;
    const struct entry* b = *(struct entry* const*)right;
    int r = bx_module_name_cmp(a->name, b->name);
    return r ? r : (a->order > b->order) - (a->order < b->order);
}

static size_t first_name(struct entry** names, size_t count, struct bx_module_path key) {
    size_t lo = 0, hi = count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (bx_module_name_cmp(names[mid]->name, key) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

static struct entry* find_name(struct entry** names, size_t count, struct bx_module_path key) {
    size_t i = first_name(names, count, key);
    return i < count && bx_module_name_cmp(names[i]->name, key) == 0 ? names[i] : NULL;
}

static struct bx_module_path shift_token(struct bx_module_path* input) {
    if (input->len == 0)
        return (struct bx_module_path) {0};
    size_t i = 0;
    while (i < input->len && space(input->text[i]))
        i++;
    size_t start = i;
    while (i < input->len && !space(input->text[i]))
        i++;
    struct bx_module_path token = {input->text + start, i - start};
    input->text += i;
    input->len -= i;
    return token;
}

static bool token_is(struct bx_module_path token, const char* text) {
    return token.len == strlen(text) && memcmp(token.text, text, token.len) == 0;
}

static int parse_softdep(struct bx_module_path line, struct entry** names, size_t count) {
    struct bx_module_path cmd = shift_token(&line);
    if (cmd.len == 0 || cmd.text[0] == '#')
        return 0;
    struct bx_module_path owner = shift_token(&line);
    if (!token_is(cmd, "softdep") || !bx_module_name_span_valid(owner.text, owner.len))
        goto invalid;
    struct bx_module_path pre = {0}, post = {0}, *section = NULL;
    struct bx_module_path word;
    while ((word = shift_token(&line)).len != 0) {
        if (token_is(word, "pre:") && section == NULL) {
            section = &pre;
            section->text = line.text;
        } else if (token_is(word, "post:") && section != &post) {
            section = &post;
            section->text = line.text;
        } else {
            if (section == NULL || token_is(word, "pre:") || token_is(word, "post:") ||
                !bx_module_alias_span_valid(word.text, word.len))
                goto invalid;
            section->len = (size_t)(line.text - section->text);
        }
    }
    if (section == NULL)
        goto invalid;
    for (size_t i = first_name(names, count, owner);
         i < count && bx_module_name_cmp(names[i]->name, owner) == 0; i++) {
        if (names[i]->pre.text != NULL || names[i]->post.text != NULL)
            break; /* The first rule for a name takes precedence. */
        names[i]->pre = pre;
        names[i]->post = post;
    }
    return 0;
invalid:
    errno = EINVAL;
    return -1;
}

static int read_softdeps(struct bx_module_tree* tree, struct entry** names, size_t count) {
    const char* data;
    size_t len;
    if (bx_module_tree_softdeps(tree, &data, &len) < 0)
        return -1;
    const char* end = data + len;
    while (data < end) {
        const char* nl = memchr(data, '\n', (size_t)(end - data));
        const char* stop = nl ? nl : end;
        if (parse_softdep((struct bx_module_path) {data, (size_t)(stop - data)}, names, count) < 0)
            return -1;
        data = nl ? nl + 1 : end;
    }
    return 0;
}

/* depmod lists dependents first; consume the list in reverse order. */
static struct bx_module_path next_dependency(struct bx_module_dep* record) {
    const char* data = record->dependencies;
    size_t end = record->length;
    while (end > 0 && space(data[end - 1]))
        end--;
    size_t start = end;
    while (start > 0 && !space(data[start - 1]))
        start--;
    record->length = start;
    return (struct bx_module_path) {data + start, end - start};
}

static int check_module(const struct bx_module_tree* tree, struct bx_module_path path) {
    char text[PATH_MAX];
    memcpy(text, path.text, path.len);
    text[path.len] = '\0';
    int fd = bx_module_tree_open_file(tree, text, O_PATH);
    if (fd < 0)
        return -1;
    struct stat st;
    int r = fstat(fd, &st);
    int error = r < 0 ? errno : S_ISREG(st.st_mode) ? 0 : EINVAL;
    bx_fd_cleanup(&fd);
    if (error) {
        errno = error;
        return -1;
    }
    return 0;
}

static int next_softdep(struct bx_module_tree* tree, struct entry** names, size_t count,
                        struct entry* entry, size_t* queries, size_t* edges,
                        struct entry** dependency) {
    struct bx_module_path* list = entry->visit == PRE ? &entry->pre : &entry->post;
    for (;;) {
        if (entry->next_alias < entry->aliases.count) {
            struct bx_module_path key = bx_module_alias_at(&entry->aliases, entry->next_alias++);
            *dependency = find_name(names, count, key);
            if (*dependency != NULL)
                return 1;
            errno = ENOENT; /* A matched alias points to a name absent from the indexes. */
            return -1;
        }
        free(entry->aliases.matches);
        entry->aliases = (struct bx_module_aliases) {0};
        entry->next_alias = 0;
        struct bx_module_path word = shift_token(list);
        if (word.len == 0)
            return 0;
        if (*edges == 0) {
            errno = E2BIG;
            return -1;
        }
        (*edges)--;
        struct entry* exact = find_name(names, count, word);
        if (exact != NULL && !exact->builtin) {
            *dependency = exact;
            return 1;
        }
        if (*queries == 0) {
            errno = E2BIG;
            return -1;
        }
        (*queries)--;
        char query[BX_MODULE_ALIAS_LIMIT + 1];
        memcpy(query, word.text, word.len);
        query[word.len] = '\0';
        if (bx_module_resolve_alias(tree, query, &entry->aliases) < 0) {
            if (errno == ENOENT)
                continue; /* Missing soft dependencies are optional. */
            return -1;
        }
        if (entry->aliases.count > *edges) {
            errno = E2BIG;
            return -1;
        }
        *edges -= entry->aliases.count;
    }
}

static int order_entries(struct bx_module_tree* tree, struct entry* entries, struct entry** names,
                         size_t count, struct entry* root, size_t* queries, size_t* edges,
                         struct bx_module_plan* plan) {
    struct entry* current = root;
    current->visit = HARD;
    while (current) {
        struct entry* dep;
        if (current->visit == HARD) {
            struct bx_module_path key;
            if (current->record.length == 0 || (key = next_dependency(&current->record)).len == 0) {
                current->visit = PRE;
                continue;
            }
            dep = find_entry(entries, count, key);
            if (dep == NULL) {
                errno = ENOENT;
                return -1;
            }
        } else {
            int next = next_softdep(tree, names, count, current, queries, edges, &dep);
            if (next < 0)
                return -1;
            if (next == 0) {
                if (current->visit == PRE) {
                    if (!current->builtin && check_module(tree, current->record.path) < 0)
                        return -1;
                    plan->steps[plan->count++] = (struct bx_module_step) {
                        .target = current->builtin ? current->name : current->record.path,
                        .name = current->name,
                        .builtin = current->builtin,
                        .requested = current->requested,
                    };
                    current->visit = POST;
                } else {
                    current->visit = DONE;
                    current = current->parent;
                }
                continue;
            }
        }
        /* A post dependency can refer back to the current path. An emitted
         * ancestor already satisfies a hard or pre dependency. */
        if ((dep->visit == HARD || dep->visit == PRE) && current->visit != POST)
            return BX_MODULE_PLAN_CYCLE;
        if (dep->visit == UNSEEN) {
            dep->visit = HARD;
            dep->parent = current;
            current = dep;
        }
    }
    return 0;
}

int bx_module_tree_plan(struct bx_module_tree* tree, const char* query, struct bx_module_plan* plan) {
    size_t count;
    struct entry* entries = read_entries(tree, &count);
    if (entries == NULL)
        return -1;
    struct bx_module_plan result = {0};
    struct entry** names = malloc((count ? count : 1) * sizeof(*names));
    struct bx_module_aliases aliases = {0};
    struct entry** roots = NULL;
    int r = -1;
    if (names == NULL)
        goto out;
    for (size_t i = 0; i < count; i++)
        names[i] = &entries[i];
    qsort(names, count, sizeof(*names), by_name);
    if (bx_module_resolve_alias(tree, query, &aliases) < 0)
        goto out;
    roots = malloc(aliases.count * sizeof(*roots));
    if (roots == NULL)
        goto out;
    /* Mark all requested modules before traversal. A later match can first
     * be reached as an earlier match's dependency and still needs its args. */
    for (size_t i = 0; i < aliases.count; i++) {
        roots[i] = find_name(names, count, bx_module_alias_at(&aliases, i));
        if (roots[i] == NULL) {
            errno = ENOENT;
            goto out;
        }
        roots[i]->requested = true;
    }
    if (read_softdeps(tree, names, count) < 0)
        goto out;
    result.steps = calloc(count, sizeof(*result.steps));
    if (result.steps == NULL)
        goto out;
    size_t queries = ALIAS_QUERY_LIMIT, edges = SOFT_EDGE_LIMIT;
    r = 0;
    for (size_t i = 0; i < aliases.count && r == 0; i++) {
        if (roots[i]->visit == UNSEEN)
            r = order_entries(tree, entries, names, count, roots[i], &queries, &edges, &result);
    }
out:;
    int error = errno;
    for (size_t i = 0; i < count; i++)
        free(entries[i].aliases.matches);
    free(names);
    free(roots);
    free(aliases.matches);
    free(entries);
    if (r < 0) {
        free(result.steps);
        errno = error;
        return r;
    }
    *plan = result;
    return 0;
}
#endif
